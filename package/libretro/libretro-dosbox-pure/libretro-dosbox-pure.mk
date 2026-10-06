################################################################################
#
# libretro-dosbox-pure
#
################################################################################

# DOSBox Pure (MS-DOS). Pinned upstream release; part of the
# default H700 core matrix (package/libretro/README.md).
# Upstream moved from GitHub to Codeberg.
LIBRETRO_DOSBOX_PURE_VERSION = 1.0-preview6
LIBRETRO_DOSBOX_PURE_SITE = https://codeberg.org/schelling/dosbox-pure/archive
LIBRETRO_DOSBOX_PURE_SOURCE = $(LIBRETRO_DOSBOX_PURE_VERSION).tar.gz
LIBRETRO_DOSBOX_PURE_LICENSE = GPL-2.0
LIBRETRO_DOSBOX_PURE_LICENSE_FILES = LICENSE

define LIBRETRO_DOSBOX_PURE_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(LIBRETRO_CORE_ENV) $(MAKE) -C $(@D) -f Makefile \
		$(LIBRETRO_CORE_MAKE_OPTS) LD="$(TARGET_CXX)"
endef

define LIBRETRO_DOSBOX_PURE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/dosbox_pure_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/dosbox_pure_libretro.so
endef

$(eval $(generic-package))
