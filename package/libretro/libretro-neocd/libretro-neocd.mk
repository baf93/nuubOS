################################################################################
#
# libretro-neocd
#
################################################################################

# NeoCD (Neo Geo CD). Pinned upstream commit; part of the
# default H700 core matrix (package/libretro/README.md).
LIBRETRO_NEOCD_VERSION = b1e04c738cb48a1dae0574b8877f6a116d270ca1
LIBRETRO_NEOCD_SITE = $(call github,libretro,neocd_libretro,$(LIBRETRO_NEOCD_VERSION))
LIBRETRO_NEOCD_LICENSE = LGPL-3.0
LIBRETRO_NEOCD_LICENSE_FILES = LICENSE.md

define LIBRETRO_NEOCD_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(LIBRETRO_CORE_ENV) $(MAKE) -C $(@D) -f Makefile \
		$(LIBRETRO_CORE_MAKE_OPTS) LD="$(TARGET_CXX)"
endef

define LIBRETRO_NEOCD_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/neocd_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/neocd_libretro.so
endef

$(eval $(generic-package))
