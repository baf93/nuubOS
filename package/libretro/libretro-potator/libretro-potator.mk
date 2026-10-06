################################################################################
#
# libretro-potator
#
################################################################################

# Potator (Watara Supervision). Pinned upstream commit; part of the
# default H700 core matrix (package/libretro/README.md).
LIBRETRO_POTATOR_VERSION = 227c5f6f3ce74d32e9002ce24c1420288559a860
LIBRETRO_POTATOR_SITE = $(call github,libretro,potator,$(LIBRETRO_POTATOR_VERSION))
LIBRETRO_POTATOR_LICENSE = Unlicense
LIBRETRO_POTATOR_LICENSE_FILES = LICENSE

define LIBRETRO_POTATOR_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(LIBRETRO_CORE_ENV) $(MAKE) -C $(@D)/platform/libretro -f Makefile \
		$(LIBRETRO_CORE_MAKE_OPTS) LD="$(TARGET_CC)"
endef

define LIBRETRO_POTATOR_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/platform/libretro/potator_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/potator_libretro.so
endef

$(eval $(generic-package))
