################################################################################
#
# moonlight-embedded
#
################################################################################

MOONLIGHT_EMBEDDED_VERSION = 2.7.1
MOONLIGHT_EMBEDDED_SOURCE = moonlight-embedded-$(MOONLIGHT_EMBEDDED_VERSION).tar.xz
MOONLIGHT_EMBEDDED_SITE = https://github.com/moonlight-stream/moonlight-embedded/releases/download/v$(MOONLIGHT_EMBEDDED_VERSION)
# The release tarball (with its submodules) has no top-level directory.
MOONLIGHT_EMBEDDED_STRIP_COMPONENTS = 0
# moonlight-embedded and moonlight-common-c GPL-3.0+, enet MIT,
# h264bitstream LGPL-2.1+, SDL_GameControllerDB zlib.
MOONLIGHT_EMBEDDED_LICENSE = GPL-3.0+, MIT, LGPL-2.1+, Zlib
MOONLIGHT_EMBEDDED_LICENSE_FILES = LICENSE
MOONLIGHT_EMBEDDED_DEPENDENCIES = \
	host-pkgconf host-wayland alsa-lib expat ffmpeg libcurl libdrm \
	libevdev openssl opus udev util-linux wayland wayland-protocols

# Only the nuubOS Wayland/V4L2 request platform, ALSA (PipeWire's ALSA
# plugin) and evdev input; the address of the host is always given, so no
# avahi discovery.
MOONLIGHT_EMBEDDED_CONF_OPTS = \
	-DENABLE_SDL=OFF \
	-DENABLE_X11=OFF \
	-DENABLE_CEC=OFF \
	-DENABLE_PULSE=OFF \
	-DENABLE_FFMPEG=OFF \
	-DENABLE_AVAHI=OFF \
	-DENABLE_WAYLAND=ON \
	-DWAYLAND_SCANNER=$(HOST_DIR)/bin/wayland-scanner \
	-DWAYLAND_PROTOCOLS_DIR=$(STAGING_DIR)/usr/share/wayland-protocols

# No man page, no generic configuration: nuubos-streamd passes every option.
define MOONLIGHT_EMBEDDED_REMOVE_EXTRAS
	rm -f $(TARGET_DIR)/etc/moonlight.conf
	rm -rf $(TARGET_DIR)/usr/share/man/man1/moonlight.1*
endef
MOONLIGHT_EMBEDDED_POST_INSTALL_TARGET_HOOKS += MOONLIGHT_EMBEDDED_REMOVE_EXTRAS

$(eval $(cmake-package))
