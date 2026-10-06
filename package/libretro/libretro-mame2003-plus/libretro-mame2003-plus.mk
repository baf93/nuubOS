################################################################################
#
# libretro-mame2003-plus
#
################################################################################

# MAME 2003-Plus (arcade, MAME 0.78 sets). Pinned upstream commit; part of the
# default H700 core matrix (package/libretro/README.md).
LIBRETRO_MAME2003_PLUS_VERSION = a0ab0427b99ca7e0ece0d13d2ee39bd2a775fafb
LIBRETRO_MAME2003_PLUS_SITE = $(call github,libretro,mame2003-plus-libretro,$(LIBRETRO_MAME2003_PLUS_VERSION))
LIBRETRO_MAME2003_PLUS_LICENSE = MAME (non-commercial)
LIBRETRO_MAME2003_PLUS_LICENSE_FILES = LICENSE.md

define LIBRETRO_MAME2003_PLUS_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(LIBRETRO_CORE_ENV) $(MAKE) -C $(@D) -f Makefile \
		$(LIBRETRO_CORE_MAKE_OPTS) LD="$(TARGET_CC)"
endef

define LIBRETRO_MAME2003_PLUS_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/mame2003_plus_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/mame2003_plus_libretro.so
endef

$(eval $(generic-package))
