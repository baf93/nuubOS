################################################################################
#
# libretro-sameduck
#
################################################################################

# SameDuck (Mega Duck). Pinned upstream commit; part of the
# default H700 core matrix (package/libretro/README.md).
LIBRETRO_SAMEDUCK_VERSION = 040e19c379f00dae10afe0eb0c1b2b019b985bbe
LIBRETRO_SAMEDUCK_SITE = $(call github,libretro,SameDuck,$(LIBRETRO_SAMEDUCK_VERSION))
LIBRETRO_SAMEDUCK_LICENSE = MIT
LIBRETRO_SAMEDUCK_LICENSE_FILES = LICENSE

define LIBRETRO_SAMEDUCK_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(LIBRETRO_CORE_ENV) $(MAKE) -C $(@D)/libretro -f Makefile \
		$(LIBRETRO_CORE_MAKE_OPTS) LD="$(TARGET_CC)"
endef

define LIBRETRO_SAMEDUCK_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/libretro/sameduck_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/sameduck_libretro.so
endef

$(eval $(generic-package))
