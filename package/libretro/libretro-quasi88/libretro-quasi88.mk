################################################################################
#
# libretro-quasi88
#
################################################################################

# QUASI88 (NEC PC-8801). Pinned upstream commit; part of the
# default H700 core matrix (package/libretro/README.md).
LIBRETRO_QUASI88_VERSION = 7884be5277776a5d8c4fdc4e574e9b5b589945c9
LIBRETRO_QUASI88_SITE = $(call github,libretro,quasi88-libretro,$(LIBRETRO_QUASI88_VERSION))
LIBRETRO_QUASI88_LICENSE = BSD-3-Clause
LIBRETRO_QUASI88_LICENSE_FILES = LICENSE

define LIBRETRO_QUASI88_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(LIBRETRO_CORE_ENV) $(MAKE) -C $(@D) -f Makefile \
		$(LIBRETRO_CORE_MAKE_OPTS) LD="$(TARGET_CXX)"
endef

define LIBRETRO_QUASI88_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/quasi88_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/quasi88_libretro.so
endef

$(eval $(generic-package))
