################################################################################
#
# libretro-cap32
#
################################################################################

# Caprice32 (Amstrad CPC). Pinned upstream commit; part of the
# default H700 core matrix (package/libretro/README.md).
LIBRETRO_CAP32_VERSION = c23855092f5f49981a5096f4d39033ff573ebca7
LIBRETRO_CAP32_SITE = $(call github,libretro,libretro-cap32,$(LIBRETRO_CAP32_VERSION))
LIBRETRO_CAP32_LICENSE = GPL-2.0
LIBRETRO_CAP32_LICENSE_FILES = cap32/COPYING.txt

define LIBRETRO_CAP32_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(LIBRETRO_CORE_ENV) $(MAKE) -C $(@D) -f Makefile \
		$(LIBRETRO_CORE_MAKE_OPTS) LD="$(TARGET_CC)"
endef

define LIBRETRO_CAP32_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/cap32_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/cap32_libretro.so
endef

$(eval $(generic-package))
