################################################################################
#
# libretro-freechaf
#
################################################################################

# FreeChaF (Fairchild Channel F). Pinned upstream commit; part of the
# default H700 core matrix (package/libretro/README.md).
LIBRETRO_FREECHAF_VERSION = 76c7a84f1f7e80f3e6f2bba96fe100cb24e99124
LIBRETRO_FREECHAF_SITE = https://github.com/libretro/FreeChaF.git
LIBRETRO_FREECHAF_SITE_METHOD = git
LIBRETRO_FREECHAF_GIT_SUBMODULES = YES
LIBRETRO_FREECHAF_LICENSE = GPL-3.0
LIBRETRO_FREECHAF_LICENSE_FILES = LICENSE

define LIBRETRO_FREECHAF_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(LIBRETRO_CORE_ENV) $(MAKE) -C $(@D) -f Makefile \
		$(LIBRETRO_CORE_MAKE_OPTS) LD="$(TARGET_CC)"
endef

define LIBRETRO_FREECHAF_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/freechaf_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/freechaf_libretro.so
endef

$(eval $(generic-package))
