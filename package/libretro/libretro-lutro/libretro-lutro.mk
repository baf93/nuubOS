################################################################################
#
# libretro-lutro
#
################################################################################

# Lutro (Lua game framework). Pinned upstream commit; part of the
# default H700 core matrix (package/libretro/README.md).
LIBRETRO_LUTRO_VERSION = 6224157a615b18507bc0b117a3398c7a324cd3e5
LIBRETRO_LUTRO_SITE = $(call github,libretro,libretro-lutro,$(LIBRETRO_LUTRO_VERSION))
LIBRETRO_LUTRO_LICENSE = MIT
LIBRETRO_LUTRO_LICENSE_FILES = LICENSE

define LIBRETRO_LUTRO_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(LIBRETRO_CORE_ENV) $(MAKE) -C $(@D) -f Makefile \
		$(LIBRETRO_CORE_MAKE_OPTS) LD="$(TARGET_CC)"
endef

define LIBRETRO_LUTRO_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/lutro_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/lutro_libretro.so
endef

$(eval $(generic-package))
