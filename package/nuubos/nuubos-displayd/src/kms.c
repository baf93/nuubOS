#include "kms.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define MAX_DRM_CARDS 8

struct output {
	drmModeConnector *connector;
	uint32_t crtc_id;
	unsigned int crtc_index;
	uint32_t primary_plane_id;
	drmModeModeInfo mode;
};

static uint32_t cached_fb_id;
static int persistent_drm_fd = -1;

static int open_sun4i_drm(void)
{
	char path[64];
	int i;

	if (persistent_drm_fd >= 0)
		return persistent_drm_fd;

	for (i = 0; i < MAX_DRM_CARDS; i++) {
		drmVersionPtr version;
		int fd;

		snprintf(path, sizeof(path), "/dev/dri/card%d", i);

		fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;

		version = drmGetVersion(fd);
		if (version &&
		    version->name &&
		    strcmp(version->name, "sun4i-drm") == 0) {
			drmFreeVersion(version);
			persistent_drm_fd = fd;
			return persistent_drm_fd;
		}

		if (version)
			drmFreeVersion(version);

		close(fd);
	}

	errno = ENODEV;
	return -1;
}

static bool connector_is_hdmi(const drmModeConnector *connector)
{
	return connector->connector_type == DRM_MODE_CONNECTOR_HDMIA ||
	       connector->connector_type == DRM_MODE_CONNECTOR_HDMIB;
}

static int find_connectors(int fd, drmModeRes *resources,
			   drmModeConnector **internal,
			   drmModeConnector **hdmi)
{
	int i;

	*internal = NULL;
	*hdmi = NULL;

	for (i = 0; i < resources->count_connectors; i++) {
		drmModeConnector *connector;

		connector = drmModeGetConnector(fd, resources->connectors[i]);
		if (!connector)
			continue;

		if (connector_is_hdmi(connector)) {
			if (!*hdmi) {
				*hdmi = connector;
				continue;
			}
		} else if (!*internal &&
			   connector->connection == DRM_MODE_CONNECTED &&
			   connector->count_modes > 0) {
			*internal = connector;
			continue;
		}

		drmModeFreeConnector(connector);
	}

	if (!*internal || !*hdmi) {
		if (*internal) {
			drmModeFreeConnector(*internal);
			*internal = NULL;
		}

		if (*hdmi) {
			drmModeFreeConnector(*hdmi);
			*hdmi = NULL;
		}

		errno = ENODEV;
		return -1;
	}

	return 0;
}

static int find_crtc(int fd, drmModeRes *resources,
		     drmModeConnector *connector,
		     uint32_t *crtc_id,
		     unsigned int *crtc_index)
{
	int i;

	for (i = 0; i < connector->count_encoders; i++) {
		drmModeEncoder *encoder;
		int j;

		encoder = drmModeGetEncoder(fd, connector->encoders[i]);
		if (!encoder)
			continue;

		for (j = 0; j < resources->count_crtcs; j++) {
			if (!(encoder->possible_crtcs & (1U << j)))
				continue;

			*crtc_id = resources->crtcs[j];
			*crtc_index = (unsigned int)j;

			drmModeFreeEncoder(encoder);
			return 0;
		}

		drmModeFreeEncoder(encoder);
	}

	errno = ENODEV;
	return -1;
}

static int get_property(int fd, uint32_t object_id,
			uint32_t object_type,
			const char *name,
			uint32_t *property_id,
			uint64_t *value)
{
	drmModeObjectPropertiesPtr properties;
	uint32_t i;
	int ret = -1;

	properties = drmModeObjectGetProperties(fd, object_id, object_type);
	if (!properties)
		return -1;

	for (i = 0; i < properties->count_props; i++) {
		drmModePropertyPtr property;

		property = drmModeGetProperty(fd, properties->props[i]);
		if (!property)
			continue;

		if (strcmp(property->name, name) == 0) {
			if (property_id)
				*property_id = property->prop_id;

			if (value)
				*value = properties->prop_values[i];

			ret = 0;
			drmModeFreeProperty(property);
			break;
		}

		drmModeFreeProperty(property);
	}

	drmModeFreeObjectProperties(properties);

	if (ret < 0)
		errno = ENOENT;

	return ret;
}

static int find_primary_plane(int fd, unsigned int crtc_index,
			      uint32_t *plane_id)
{
	drmModePlaneResPtr plane_resources;
	uint32_t i;
	int ret = -1;

	plane_resources = drmModeGetPlaneResources(fd);
	if (!plane_resources)
		return -1;

	for (i = 0; i < plane_resources->count_planes; i++) {
		drmModePlanePtr plane;
		uint64_t type;

		plane = drmModeGetPlane(fd, plane_resources->planes[i]);
		if (!plane)
			continue;

		if (!(plane->possible_crtcs & (1U << crtc_index))) {
			drmModeFreePlane(plane);
			continue;
		}

		if (get_property(fd, plane->plane_id,
				 DRM_MODE_OBJECT_PLANE,
				 "type", NULL, &type) == 0 &&
		    type == DRM_PLANE_TYPE_PRIMARY) {
			*plane_id = plane->plane_id;
			ret = 0;
			drmModeFreePlane(plane);
			break;
		}

		drmModeFreePlane(plane);
	}

	drmModeFreePlaneResources(plane_resources);

	if (ret < 0)
		errno = ENODEV;

	return ret;
}

static double mode_vrefresh(const drmModeModeInfo *mode)
{
	unsigned int num;
	unsigned int den;

	if (!mode->htotal || !mode->vtotal)
		return 0.0;

	num = mode->clock;
	den = mode->htotal * mode->vtotal;

	if (mode->flags & DRM_MODE_FLAG_INTERLACE)
		num *= 2;

	if (mode->flags & DRM_MODE_FLAG_DBLSCAN)
		den *= 2;

	if (mode->vscan > 1)
		den *= mode->vscan;

	return (double)num * 1000.0 / (double)den;
}

static drmModeModeInfo select_internal_mode(drmModeConnector *connector)
{
	int i;

	for (i = 0; i < connector->count_modes; i++) {
		if (connector->modes[i].type & DRM_MODE_TYPE_PREFERRED)
			return connector->modes[i];
	}

	return connector->modes[0];
}

static drmModeModeInfo select_hdmi_mode(drmModeConnector *connector)
{
	drmModeModeInfo *best = NULL;
	long long best_score = LLONG_MIN;
	int i;

	for (i = 0; i < connector->count_modes; i++) {
		drmModeModeInfo *mode = &connector->modes[i];
		long long score;
		long long area;
		int refresh;
		bool near_60;
		bool exact_1080p60;

		if (mode->hdisplay > 1920 || mode->vdisplay > 1080)
			continue;

		refresh = (int)(mode_vrefresh(mode) + 0.5);
		near_60 = refresh >= 59 && refresh <= 61;
		exact_1080p60 =
			mode->hdisplay == 1920 &&
			mode->vdisplay == 1080 &&
			near_60;

		area = (long long)mode->hdisplay * mode->vdisplay;

		score = area * 1000LL;

		if (near_60)
			score += 2000000000000LL;

		if (exact_1080p60)
			score += 4000000000000LL;

		if (mode->type & DRM_MODE_TYPE_PREFERRED)
			score += 100LL;

		if (score > best_score) {
			best_score = score;
			best = mode;
		}
	}

	if (!best)
		best = &connector->modes[0];

	return *best;
}

static int get_scanout_fb(int fd,
			  uint32_t internal_plane_id,
			  uint32_t hdmi_plane_id,
			  uint32_t *fb_id,
			  uint32_t *width,
			  uint32_t *height)
{
	uint32_t planes[2] = {
		internal_plane_id,
		hdmi_plane_id,
	};
	unsigned int i;

	if (cached_fb_id) {
		drmModeFBPtr fb = drmModeGetFB(fd, cached_fb_id);

		if (fb) {
			*fb_id = cached_fb_id;
			*width = fb->width;
			*height = fb->height;
			drmModeFreeFB(fb);
			return 0;
		}

		cached_fb_id = 0;
	}

	for (i = 0; i < 2; i++) {
		drmModePlanePtr plane;
		drmModeFBPtr fb;

		plane = drmModeGetPlane(fd, planes[i]);
		if (!plane)
			continue;

		if (!plane->fb_id) {
			drmModeFreePlane(plane);
			continue;
		}

		fb = drmModeGetFB(fd, plane->fb_id);
		if (!fb) {
			drmModeFreePlane(plane);
			continue;
		}

		cached_fb_id = plane->fb_id;

		*fb_id = plane->fb_id;
		*width = fb->width;
		*height = fb->height;

		drmModeFreeFB(fb);
		drmModeFreePlane(plane);
		return 0;
	}

	errno = ENODEV;
	return -1;
}

static int atomic_add(int fd, drmModeAtomicReqPtr request,
		      uint32_t object_id, uint32_t object_type,
		      const char *property_name, uint64_t value)
{
	uint32_t property_id;

	if (get_property(fd, object_id, object_type,
			 property_name, &property_id, NULL) < 0) {
		fprintf(stderr,
			"nuubos-displayd: property lookup failed: object=%u property=%s errno=%d (%s)\n",
			object_id, property_name, errno, strerror(errno));
		return -1;
	}

	if (drmModeAtomicAddProperty(request, object_id,
				     property_id, value) < 0) {
		if (!errno)
			errno = EINVAL;

		fprintf(stderr,
			"nuubos-displayd: atomic add failed: object=%u property=%s value=%llu errno=%d (%s)\n",
			object_id, property_name,
			(unsigned long long)value,
			errno, strerror(errno));
		return -1;
	}

	return 0;
}

static void fit_1to1(uint32_t source_width,
			 uint32_t source_height,
			 uint32_t target_width,
			 uint32_t target_height,
			 uint32_t *src_x,
			 uint32_t *src_y,
			 uint32_t *dst_x,
			 uint32_t *dst_y,
			 uint32_t *width,
			 uint32_t *height)
{
	*width = source_width < target_width ?
		source_width : target_width;
	*height = source_height < target_height ?
		source_height : target_height;

	*src_x = (source_width - *width) / 2;
	*src_y = (source_height - *height) / 2;

	*dst_x = (target_width - *width) / 2;
	*dst_y = (target_height - *height) / 2;
}

static int configure_plane(int fd,
			   drmModeAtomicReqPtr request,
			   uint32_t plane_id,
			   uint32_t crtc_id,
			   uint32_t fb_id,
			   uint32_t fb_width,
			   uint32_t fb_height,
			   uint32_t mode_width,
			   uint32_t mode_height)
{
	uint32_t src_x;
	uint32_t src_y;
	uint32_t dst_x;
	uint32_t dst_y;
	uint32_t width;
	uint32_t height;

	fit_1to1(fb_width, fb_height,
		 mode_width, mode_height,
		 &src_x, &src_y,
		 &dst_x, &dst_y,
		 &width, &height);

	if (atomic_add(fd, request, plane_id, DRM_MODE_OBJECT_PLANE,
		       "FB_ID", fb_id) < 0 ||
	    atomic_add(fd, request, plane_id, DRM_MODE_OBJECT_PLANE,
		       "CRTC_ID", crtc_id) < 0 ||
	    atomic_add(fd, request, plane_id, DRM_MODE_OBJECT_PLANE,
		       "SRC_X", (uint64_t)src_x << 16) < 0 ||
	    atomic_add(fd, request, plane_id, DRM_MODE_OBJECT_PLANE,
		       "SRC_Y", (uint64_t)src_y << 16) < 0 ||
	    atomic_add(fd, request, plane_id, DRM_MODE_OBJECT_PLANE,
		       "SRC_W", (uint64_t)width << 16) < 0 ||
	    atomic_add(fd, request, plane_id, DRM_MODE_OBJECT_PLANE,
		       "SRC_H", (uint64_t)height << 16) < 0 ||
	    atomic_add(fd, request, plane_id, DRM_MODE_OBJECT_PLANE,
		       "CRTC_X", dst_x) < 0 ||
	    atomic_add(fd, request, plane_id, DRM_MODE_OBJECT_PLANE,
		       "CRTC_Y", dst_y) < 0 ||
	    atomic_add(fd, request, plane_id, DRM_MODE_OBJECT_PLANE,
		       "CRTC_W", width) < 0 ||
	    atomic_add(fd, request, plane_id, DRM_MODE_OBJECT_PLANE,
		       "CRTC_H", height) < 0)
		return -1;

	return 0;
}

static int disable_plane(int fd, drmModeAtomicReqPtr request,
			 uint32_t plane_id)
{
	if (atomic_add(fd, request, plane_id, DRM_MODE_OBJECT_PLANE,
		       "FB_ID", 0) < 0 ||
	    atomic_add(fd, request, plane_id, DRM_MODE_OBJECT_PLANE,
		       "CRTC_ID", 0) < 0)
		return -1;

	return 0;
}

int nuubos_kms_apply(bool use_hdmi, char *mode_desc, size_t mode_desc_size)
{
	drmModeConnector *internal_connector = NULL;
	drmModeConnector *hdmi_connector = NULL;
	drmModeAtomicReqPtr request = NULL;
	drmModeResPtr resources = NULL;
	struct output internal = { 0 };
	struct output hdmi = { 0 };
	struct output *target;
	struct output *other;
	uint32_t fb_id;
	uint32_t fb_width;
	uint32_t fb_height;
	uint32_t mode_blob = 0;
	bool master = false;
	int fd = -1;
	int ret = -1;
	int saved_errno = 0;
	const char *stage = "open-drm";

	fd = open_sun4i_drm();
	if (fd < 0)
		goto out;

	stage = "client-capabilities";
	if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) < 0 ||
	    drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1) < 0)
		goto out;

	stage = "drm-master";
	if (drmSetMaster(fd) < 0)
		goto out;

	master = true;

	stage = "resources";
	resources = drmModeGetResources(fd);
	if (!resources)
		goto out;

	stage = "connectors";
	if (find_connectors(fd, resources,
			    &internal_connector,
			    &hdmi_connector) < 0)
		goto out;

	internal.connector = internal_connector;
	hdmi.connector = hdmi_connector;

	stage = "crtcs";
	if (find_crtc(fd, resources, internal.connector,
		      &internal.crtc_id,
		      &internal.crtc_index) < 0 ||
	    find_crtc(fd, resources, hdmi.connector,
		      &hdmi.crtc_id,
		      &hdmi.crtc_index) < 0)
		goto out;

	stage = "primary-planes";
	if (find_primary_plane(fd, internal.crtc_index,
			       &internal.primary_plane_id) < 0 ||
	    find_primary_plane(fd, hdmi.crtc_index,
			       &hdmi.primary_plane_id) < 0)
		goto out;

	internal.mode = select_internal_mode(internal.connector);

	if (use_hdmi) {
		if (hdmi.connector->connection != DRM_MODE_CONNECTED ||
		    hdmi.connector->count_modes == 0) {
			errno = ENODEV;
			goto out;
		}

		hdmi.mode = select_hdmi_mode(hdmi.connector);
		target = &hdmi;
		other = &internal;
	} else {
		target = &internal;
		other = &hdmi;
	}

	stage = "scanout-fb";
	if (get_scanout_fb(fd,
			   internal.primary_plane_id,
			   hdmi.primary_plane_id,
			   &fb_id,
			   &fb_width,
			   &fb_height) < 0)
		goto out;

	stage = "mode-blob";
	if (drmModeCreatePropertyBlob(fd, &target->mode,
				      sizeof(target->mode),
				      &mode_blob) < 0)
		goto out;

	stage = "atomic-alloc";
	request = drmModeAtomicAlloc();
	if (!request)
		goto out;

	stage = "connector-routing";
	if (atomic_add(fd, request,
		       internal.connector->connector_id,
		       DRM_MODE_OBJECT_CONNECTOR,
		       "CRTC_ID",
		       use_hdmi ? 0 : internal.crtc_id) < 0 ||
	    atomic_add(fd, request,
		       hdmi.connector->connector_id,
		       DRM_MODE_OBJECT_CONNECTOR,
		       "CRTC_ID",
		       use_hdmi ? hdmi.crtc_id : 0) < 0)
		goto out;

	stage = "crtc-state";
	if (atomic_add(fd, request,
		       target->crtc_id,
		       DRM_MODE_OBJECT_CRTC,
		       "MODE_ID", mode_blob) < 0 ||
	    atomic_add(fd, request,
		       target->crtc_id,
		       DRM_MODE_OBJECT_CRTC,
		       "ACTIVE", 1) < 0 ||
	    atomic_add(fd, request,
		       other->crtc_id,
		       DRM_MODE_OBJECT_CRTC,
		       "MODE_ID", 0) < 0 ||
	    atomic_add(fd, request,
		       other->crtc_id,
		       DRM_MODE_OBJECT_CRTC,
		       "ACTIVE", 0) < 0)
		goto out;

	stage = "disable-old-plane";
	if (disable_plane(fd, request,
			  other->primary_plane_id) < 0)
		goto out;

	stage = "configure-target-plane";
	if (configure_plane(fd, request,
			    target->primary_plane_id,
			    target->crtc_id,
			    fb_id,
			    fb_width,
			    fb_height,
			    target->mode.hdisplay,
			    target->mode.vdisplay) < 0)
		goto out;

	stage = "atomic-commit";
	if (drmModeAtomicCommit(fd, request,
				DRM_MODE_ATOMIC_ALLOW_MODESET,
				NULL) < 0)
		goto out;

	if (mode_desc && mode_desc_size > 0) {
		snprintf(mode_desc, mode_desc_size,
			 "%ux%u@%d",
			 target->mode.hdisplay,
			 target->mode.vdisplay,
			 (int)(mode_vrefresh(&target->mode) + 0.5));
	}

	ret = 0;

out:
	saved_errno = errno;

	if (ret < 0)
		fprintf(stderr,
			"nuubos-displayd: KMS failed at stage=%s errno=%d (%s)\n",
			stage, saved_errno, strerror(saved_errno));

	if (request)
		drmModeAtomicFree(request);

	if (mode_blob)
		drmModeDestroyPropertyBlob(fd, mode_blob);

	if (internal_connector)
		drmModeFreeConnector(internal_connector);

	if (hdmi_connector)
		drmModeFreeConnector(hdmi_connector);

	if (resources)
		drmModeFreeResources(resources);

	if (master)
		drmDropMaster(fd);

	/*
	 * Keep the sun4i DRM file open for the lifetime of the daemon.
	 * Closing the last DRM file lets fbdev lastclose restore its
	 * own console configuration, undoing our atomic routing.
	 */
	if (fd >= 0 && fd != persistent_drm_fd)
		close(fd);

	errno = saved_errno;
	return ret;
}
