################################################################################
#
# libretro-ppsspp
#
################################################################################

# PPSSPP (PlayStation Portable) libretro core, GLES, aarch64 JIT. Tagged
# release with its submodules (bundled FFmpeg for PSP video/audio). Part of
# the default H700 core matrix (package/libretro/README.md).
LIBRETRO_PPSSPP_VERSION = v1.20.4
LIBRETRO_PPSSPP_SITE = https://github.com/hrydgard/ppsspp.git
LIBRETRO_PPSSPP_SITE_METHOD = git
LIBRETRO_PPSSPP_GIT_SUBMODULES = YES
LIBRETRO_PPSSPP_LICENSE = GPL-2.0+
LIBRETRO_PPSSPP_LICENSE_FILES = LICENSE.TXT
LIBRETRO_PPSSPP_DEPENDENCIES = libgles libegl
LIBRETRO_PPSSPP_SUPPORTS_IN_SOURCE_BUILD = NO
LIBRETRO_PPSSPP_CONF_OPTS = \
	-DLIBRETRO=ON -DUSING_GLES2=ON -DUSING_EGL=OFF -DUSING_FBDEV=OFF \
	-DUSING_X11_VULKAN=OFF -DVULKAN=OFF -DUSE_WAYLAND_WSI=OFF \
	-DUSE_DISCORD=OFF -DUSE_MINIUPNPC=OFF -DUSE_SYSTEM_FFMPEG=OFF \
	-DUSE_FFMPEG=ON -DHEADLESS=OFF -DUNITTEST=OFF -DSIMULATOR=OFF \
	-DUSE_SYSTEM_LIBZIP=OFF -DUSE_SYSTEM_ZSTD=OFF -DUSE_SYSTEM_SNAPPY=OFF \
	-DUSE_SYSTEM_LIBPNG=OFF -DUSE_SYSTEM_LIBSDL2=OFF \
	-DBUILD_SHARED_LIBS=OFF

# The core loads its fonts/UI atlas from <system>/PPSSPP: shipped here and
# copied into /userdata/bios by nuubos-emud when missing.
define LIBRETRO_PPSSPP_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(LIBRETRO_PPSSPP_BUILDDIR)/lib/ppsspp_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/ppsspp_libretro.so
	rm -rf $(TARGET_DIR)/usr/share/nuubos/emulation/system/PPSSPP
	mkdir -p $(TARGET_DIR)/usr/share/nuubos/emulation/system/PPSSPP
	cp -a $(@D)/assets/. $(TARGET_DIR)/usr/share/nuubos/emulation/system/PPSSPP/
endef

$(eval $(cmake-package))
