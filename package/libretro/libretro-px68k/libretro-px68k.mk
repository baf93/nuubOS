################################################################################
#
# libretro-px68k
#
################################################################################

# PX68K (Sharp X68000). Pinned upstream commit; part of the
# default H700 core matrix (package/libretro/README.md).
LIBRETRO_PX68K_VERSION = 0ad84d7058a12b7db4f7f7a906e87fad4e2f26f6
LIBRETRO_PX68K_SITE = $(call github,libretro,px68k-libretro,$(LIBRETRO_PX68K_VERSION))
LIBRETRO_PX68K_LICENSE = GPL-2.0
LIBRETRO_PX68K_LICENSE_FILES = COPYING

define LIBRETRO_PX68K_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(LIBRETRO_CORE_ENV) $(MAKE) -C $(@D) -f Makefile.libretro \
		$(LIBRETRO_CORE_MAKE_OPTS) LD="$(TARGET_CXX)"
endef

define LIBRETRO_PX68K_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/px68k_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/px68k_libretro.so
endef

$(eval $(generic-package))
