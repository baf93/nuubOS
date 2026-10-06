################################################################################
#
# libretro-atari800
#
################################################################################

# Atari800 (Atari 400/800/XL/XE/5200). Pinned upstream commit; part of the
# default H700 core matrix (package/libretro/README.md).
LIBRETRO_ATARI800_VERSION = 4e7fbc73765c1a9670c7506616046ad1d4ccda51
LIBRETRO_ATARI800_SITE = $(call github,libretro,libretro-atari800,$(LIBRETRO_ATARI800_VERSION))
LIBRETRO_ATARI800_LICENSE = GPL-2.0
LIBRETRO_ATARI800_LICENSE_FILES = atari800/COPYING

define LIBRETRO_ATARI800_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(LIBRETRO_CORE_ENV) $(MAKE) -C $(@D) -f Makefile \
		$(LIBRETRO_CORE_MAKE_OPTS) LD="$(TARGET_CC)"
endef

define LIBRETRO_ATARI800_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/atari800_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/atari800_libretro.so
endef

$(eval $(generic-package))
