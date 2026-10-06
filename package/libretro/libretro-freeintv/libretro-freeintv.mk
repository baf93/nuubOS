################################################################################
#
# libretro-freeintv
#
################################################################################

# FreeIntv (Mattel Intellivision). Pinned upstream commit; part of the
# default H700 core matrix (package/libretro/README.md).
LIBRETRO_FREEINTV_VERSION = ef3e0fe322bec62a7f916c0bb0834c08c348d0b4
LIBRETRO_FREEINTV_SITE = $(call github,libretro,FreeIntv,$(LIBRETRO_FREEINTV_VERSION))
LIBRETRO_FREEINTV_LICENSE = GPL-3.0
LIBRETRO_FREEINTV_LICENSE_FILES = LICENSE

define LIBRETRO_FREEINTV_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(LIBRETRO_CORE_ENV) $(MAKE) -C $(@D) -f Makefile \
		$(LIBRETRO_CORE_MAKE_OPTS) LD="$(TARGET_CC)"
endef

define LIBRETRO_FREEINTV_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/freeintv_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/freeintv_libretro.so
endef

$(eval $(generic-package))
