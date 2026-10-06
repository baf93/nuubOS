################################################################################
#
# libretro-hatari
#
################################################################################

# Hatari (Atari ST/STE/TT/Falcon) libretro core, built from the upstream
# CMake tree; boots the bundled EmuTOS when no TOS image is provided. IPF
# (capsimage) support is off: its library is not free software. Part of the
# default H700 core matrix (package/libretro/README.md).
LIBRETRO_HATARI_VERSION = 5831f66e05ae19435bd9d8ef1c6f9c93998ff6f4
LIBRETRO_HATARI_SITE = $(call github,libretro,hatari,$(LIBRETRO_HATARI_VERSION))
LIBRETRO_HATARI_LICENSE = GPL-2.0+
LIBRETRO_HATARI_LICENSE_FILES = gpl.txt
LIBRETRO_HATARI_DEPENDENCIES = udev zlib
LIBRETRO_HATARI_SUPPORTS_IN_SOURCE_BUILD = NO
LIBRETRO_HATARI_CONF_OPTS = \
	-DENABLE_LIBRETRO=ON -DENABLE_HATARI=OFF -DENABLE_TOOLS=OFF \
	-DENABLE_STATIC_ZLIB=OFF -DENABLE_STATIC_CAPSIMAGE=OFF \
	-DENABLE_CAPSIMAGE=OFF -DCMAKE_C_STANDARD_LIBRARIES="-lm -ludev"

define LIBRETRO_HATARI_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $$(find $(LIBRETRO_HATARI_BUILDDIR) -name 'hatari_libretro.so' | head -1) \
		$(TARGET_DIR)/usr/lib/libretro/hatari_libretro.so
endef

$(eval $(cmake-package))
