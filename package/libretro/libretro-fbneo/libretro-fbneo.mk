################################################################################
#
# libretro-fbneo
#
################################################################################

# FinalBurn Neo (Arcade / Neo Geo). Pinned upstream commit; part of the
# default H700 core matrix (package/libretro/README.md).
LIBRETRO_FBNEO_VERSION = 63c4190785cadd5ff84483399375871ed6e98754
LIBRETRO_FBNEO_SITE = $(call github,libretro,FBNeo,$(LIBRETRO_FBNEO_VERSION))
LIBRETRO_FBNEO_LICENSE = FBNeo (non-commercial)
LIBRETRO_FBNEO_LICENSE_FILES = src/license.txt

define LIBRETRO_FBNEO_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(LIBRETRO_CORE_ENV) $(MAKE) -C $(@D)/src/burner/libretro -f Makefile \
		$(LIBRETRO_CORE_MAKE_OPTS) LD="$(TARGET_CXX)"
endef

define LIBRETRO_FBNEO_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/src/burner/libretro/fbneo_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/fbneo_libretro.so
endef

$(eval $(generic-package))
