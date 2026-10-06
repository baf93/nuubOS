################################################################################
#
# libretro-beetle-vb
#
################################################################################

# Beetle VB (Virtual Boy). Pinned upstream commit; part of the
# default H700 core matrix (package/libretro/README.md).
LIBRETRO_BEETLE_VB_VERSION = 83ed42608601fb7b01d41e4f8fb2007a37b8c84e
LIBRETRO_BEETLE_VB_SITE = $(call github,libretro,beetle-vb-libretro,$(LIBRETRO_BEETLE_VB_VERSION))
LIBRETRO_BEETLE_VB_LICENSE = GPL-2.0
LIBRETRO_BEETLE_VB_LICENSE_FILES = COPYING

define LIBRETRO_BEETLE_VB_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(LIBRETRO_CORE_ENV) $(MAKE) -C $(@D) -f Makefile \
		$(LIBRETRO_CORE_MAKE_OPTS) LD="$(TARGET_CXX)"
endef

define LIBRETRO_BEETLE_VB_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/mednafen_vb_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/mednafen_vb_libretro.so
endef

$(eval $(generic-package))
