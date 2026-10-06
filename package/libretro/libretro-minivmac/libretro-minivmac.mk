################################################################################
#
# libretro-minivmac
#
################################################################################

# Mini vMac (Apple Macintosh Plus/II). Pinned upstream commit; part of the
# default H700 core matrix (package/libretro/README.md).
LIBRETRO_MINIVMAC_VERSION = babdf7b53d361a7225858b68a8a64efe8b06f5e5
LIBRETRO_MINIVMAC_SITE = https://github.com/libretro/libretro-minivmac.git
LIBRETRO_MINIVMAC_SITE_METHOD = git
LIBRETRO_MINIVMAC_GIT_SUBMODULES = YES
LIBRETRO_MINIVMAC_LICENSE = GPL-2.0
LIBRETRO_MINIVMAC_LICENSE_FILES = minivmac/COPYING.txt

define LIBRETRO_MINIVMAC_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(LIBRETRO_CORE_ENV) $(MAKE) -C $(@D) -f Makefile \
		$(LIBRETRO_CORE_MAKE_OPTS) LD="$(TARGET_CC)"
endef

define LIBRETRO_MINIVMAC_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/minivmac_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/minivmac_libretro.so
endef

$(eval $(generic-package))
