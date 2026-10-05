/* SPDX-License-Identifier: MIT */
/*
 * drm-grab: development-only screenshot of the scanout framebuffer.
 *
 * Reads the framebuffer currently attached to each active CRTC (what is
 * really on the panel/HDMI) through DRM_IOCTL_MODE_GETFB2 + PRIME mmap and
 * writes a binary PPM per CRTC: <prefix>-crtc<N>.ppm. Linear 32-bit RGB
 * formats only. Never shipped in the image; copy to /tmp on the target.
 *
 * Usage: drm-grab [/dev/dri/cardN] [prefix]
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

static int dump_fb(int fd, uint32_t fb_id, const char *path)
{
	drmModeFB2Ptr fb = drmModeGetFB2(fd, fb_id);
	int dmafd = -1, rc = 1;
	size_t size;
	uint8_t *map;
	FILE *out;

	if (!fb) {
		fprintf(stderr, "GETFB2 %u failed: %s\n", fb_id, strerror(errno));
		return 1;
	}
	fprintf(stderr, "fb %u: %ux%u format=%.4s modifier=0x%llx pitch=%u\n",
		fb_id, fb->width, fb->height, (char *)&fb->pixel_format,
		(unsigned long long)fb->modifier, fb->pitches[0]);

	if (fb->modifier != DRM_FORMAT_MOD_LINEAR &&
	    fb->modifier != DRM_FORMAT_MOD_INVALID) {
		fprintf(stderr, "non-linear modifier, cannot read\n");
		goto done;
	}
	if (fb->pixel_format != DRM_FORMAT_XRGB8888 &&
	    fb->pixel_format != DRM_FORMAT_ARGB8888 &&
	    fb->pixel_format != DRM_FORMAT_XBGR8888 &&
	    fb->pixel_format != DRM_FORMAT_ABGR8888) {
		fprintf(stderr, "unsupported format\n");
		goto done;
	}
	if (drmPrimeHandleToFD(fd, fb->handles[0], DRM_CLOEXEC | DRM_RDWR, &dmafd) &&
	    drmPrimeHandleToFD(fd, fb->handles[0], DRM_CLOEXEC, &dmafd)) {
		fprintf(stderr, "PRIME export failed: %s\n", strerror(errno));
		goto done;
	}
	size = (size_t)fb->pitches[0] * fb->height + fb->offsets[0];
	map = mmap(NULL, size, PROT_READ, MAP_SHARED, dmafd, 0);
	if (map == MAP_FAILED) {
		fprintf(stderr, "mmap failed: %s\n", strerror(errno));
		goto done;
	}
	out = fopen(path, "wb");
	if (out) {
		int bgr = fb->pixel_format == DRM_FORMAT_XBGR8888 ||
			  fb->pixel_format == DRM_FORMAT_ABGR8888;
		fprintf(out, "P6\n%u %u\n255\n", fb->width, fb->height);
		for (uint32_t y = 0; y < fb->height; y++) {
			const uint8_t *row = map + fb->offsets[0] + (size_t)y * fb->pitches[0];
			for (uint32_t x = 0; x < fb->width; x++) {
				const uint8_t *p = row + x * 4;
				uint8_t rgb[3] = { bgr ? p[0] : p[2], p[1], bgr ? p[2] : p[0] };
				fwrite(rgb, 1, 3, out);
			}
		}
		fclose(out);
		fprintf(stderr, "wrote %s\n", path);
		rc = 0;
	}
	munmap(map, size);
done:
	if (dmafd >= 0)
		close(dmafd);
	drmModeFreeFB2(fb);
	return rc;
}

int main(int argc, char **argv)
{
	const char *dev = argc > 1 ? argv[1] : "/dev/dri/card0";
	const char *prefix = argc > 2 ? argv[2] : "/tmp/drm-grab";
	drmModeResPtr res;
	int fd, rc = 1;
	char path[256];

	fd = open(dev, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror(dev);
		return 1;
	}
	res = drmModeGetResources(fd);
	if (!res) {
		perror("GetResources");
		return 1;
	}
	for (int i = 0; i < res->count_crtcs; i++) {
		drmModeCrtcPtr crtc = drmModeGetCrtc(fd, res->crtcs[i]);
		if (crtc && crtc->buffer_id && crtc->mode_valid) {
			fprintf(stderr, "crtc %d: %ux%u\n", i, crtc->mode.hdisplay, crtc->mode.vdisplay);
			snprintf(path, sizeof(path), "%s-crtc%d.ppm", prefix, i);
			if (dump_fb(fd, crtc->buffer_id, path) == 0)
				rc = 0;
		}
		drmModeFreeCrtc(crtc);
	}
	drmModeFreeResources(res);
	close(fd);
	return rc;
}
