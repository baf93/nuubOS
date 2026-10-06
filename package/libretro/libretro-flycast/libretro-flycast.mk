################################################################################
#
# libretro-flycast
#
################################################################################

# Flycast (Sega Dreamcast / NAOMI): Cortex-A53 GLES profile, arm64 dynarec,
# low-end settings. Pinned upstream commit; part of the default H700 core
# matrix (package/libretro/README.md).
LIBRETRO_FLYCAST_VERSION = d0fa8b5407251c9c92b44b7c9eb83e3bf84e16c1
LIBRETRO_FLYCAST_SITE = $(call github,libretro,flycast,$(LIBRETRO_FLYCAST_VERSION))
LIBRETRO_FLYCAST_LICENSE = GPL-2.0
LIBRETRO_FLYCAST_LICENSE_FILES = LICENSE
LIBRETRO_FLYCAST_DEPENDENCIES = libgles

define LIBRETRO_FLYCAST_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(LIBRETRO_CORE_ENV) $(MAKE) -C $(@D) -f Makefile \
		platform=arm64_cortex_a53_gles2 HAVE_OPENMP=0 LD="$(TARGET_CXX)" \
		AS="$(TARGET_CC)" CC_AS="$(TARGET_CC)"
endef

define LIBRETRO_FLYCAST_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/flycast_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/flycast_libretro.so
endef

$(eval $(generic-package))
