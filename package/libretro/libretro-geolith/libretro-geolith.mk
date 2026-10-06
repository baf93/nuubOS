################################################################################
#
# libretro-geolith
#
################################################################################

# Geolith (Neo Geo AES/MVS). Pinned upstream commit; part of the
# default H700 core matrix (package/libretro/README.md).
LIBRETRO_GEOLITH_VERSION = 194024931935eff2092e36fc4f8e53e62ed11097
LIBRETRO_GEOLITH_SITE = $(call github,libretro,geolith-libretro,$(LIBRETRO_GEOLITH_VERSION))
LIBRETRO_GEOLITH_LICENSE = BSD-3-Clause
LIBRETRO_GEOLITH_LICENSE_FILES = LICENSE

define LIBRETRO_GEOLITH_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(LIBRETRO_CORE_ENV) $(MAKE) -C $(@D)/libretro -f Makefile \
		$(LIBRETRO_CORE_MAKE_OPTS) LD="$(TARGET_CC)"
endef

define LIBRETRO_GEOLITH_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/libretro/geolith_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/geolith_libretro.so
endef

$(eval $(generic-package))
