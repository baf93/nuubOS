################################################################################
#
# libretro-pokemini
#
################################################################################

# PokeMini (Pokémon Mini). Pinned upstream commit; part of the
# default H700 core matrix (package/libretro/README.md).
LIBRETRO_POKEMINI_VERSION = 132111b76343559860532a1ccc094f93f1ed5650
LIBRETRO_POKEMINI_SITE = $(call github,libretro,PokeMini,$(LIBRETRO_POKEMINI_VERSION))
LIBRETRO_POKEMINI_LICENSE = GPL-3.0
LIBRETRO_POKEMINI_LICENSE_FILES = LICENSE

define LIBRETRO_POKEMINI_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(LIBRETRO_CORE_ENV) $(MAKE) -C $(@D) -f Makefile.libretro \
		$(LIBRETRO_CORE_MAKE_OPTS) LD="$(TARGET_CC)"
endef

define LIBRETRO_POKEMINI_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/pokemini_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/pokemini_libretro.so
endef

$(eval $(generic-package))
