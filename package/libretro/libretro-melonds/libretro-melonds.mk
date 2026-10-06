################################################################################
#
# libretro-melonds
#
################################################################################

# melonDS (Nintendo DS), aarch64 JIT. Pinned upstream commit; part of the
# default H700 core matrix (package/libretro/README.md).
LIBRETRO_MELONDS_VERSION = 66b5d2634cd0a79030562811e6e05f5532f800ba
LIBRETRO_MELONDS_SITE = $(call github,libretro,melonDS,$(LIBRETRO_MELONDS_VERSION))
LIBRETRO_MELONDS_LICENSE = GPL-3.0
LIBRETRO_MELONDS_LICENSE_FILES = LICENSE

define LIBRETRO_MELONDS_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(LIBRETRO_CORE_ENV) $(MAKE) -C $(@D) -f Makefile \
		$(LIBRETRO_CORE_MAKE_OPTS) LD="$(TARGET_CXX)" ARCH=arm64
endef

define LIBRETRO_MELONDS_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/melonds_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/melonds_libretro.so
endef

$(eval $(generic-package))
