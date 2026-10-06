################################################################################
#
# libretro-mupen64plus-next
#
################################################################################

# Mupen64Plus-Next (Nintendo 64), aarch64 dynarec + GLideN64 on GLES3. Pinned upstream commit; part of the
# default H700 core matrix (package/libretro/README.md).
LIBRETRO_MUPEN64PLUS_NEXT_VERSION = 12edd2c74a517ff86dfa8cfc71ad75e4c10486d5
LIBRETRO_MUPEN64PLUS_NEXT_SITE = $(call github,libretro,mupen64plus-libretro-nx,$(LIBRETRO_MUPEN64PLUS_NEXT_VERSION))
LIBRETRO_MUPEN64PLUS_NEXT_LICENSE = GPL-2.0
LIBRETRO_MUPEN64PLUS_NEXT_LICENSE_FILES = LICENSE
LIBRETRO_MUPEN64PLUS_NEXT_DEPENDENCIES = libgles

define LIBRETRO_MUPEN64PLUS_NEXT_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(LIBRETRO_CORE_ENV) $(MAKE) -C $(@D) -f Makefile \
		$(LIBRETRO_CORE_MAKE_OPTS) LD="$(TARGET_CXX)" FORCE_GLES3=1 ARCH=aarch64
endef

define LIBRETRO_MUPEN64PLUS_NEXT_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/mupen64plus_next_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/mupen64plus_next_libretro.so
endef

$(eval $(generic-package))
