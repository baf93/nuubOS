################################################################################
#
# libretro-vecx
#
################################################################################

# vecx (GCE Vectrex). Pinned upstream commit; part of the
# default H700 core matrix (package/libretro/README.md).
LIBRETRO_VECX_VERSION = 8f671cc9d737f2890c3ce19e177e2984dcae121f
LIBRETRO_VECX_SITE = $(call github,libretro,libretro-vecx,$(LIBRETRO_VECX_VERSION))
LIBRETRO_VECX_LICENSE = GPL-3.0
LIBRETRO_VECX_LICENSE_FILES = LICENSE.md

define LIBRETRO_VECX_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(LIBRETRO_CORE_ENV) $(MAKE) -C $(@D) -f Makefile.libretro \
		$(LIBRETRO_CORE_MAKE_OPTS) LD="$(TARGET_CC)" HAS_GPU=0
endef

define LIBRETRO_VECX_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/vecx_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/vecx_libretro.so
endef

$(eval $(generic-package))
