################################################################################
#
# libretro-mu
#
################################################################################

# Mu (Palm OS PDAs). Pinned upstream commit; part of the
# default H700 core matrix (package/libretro/README.md).
LIBRETRO_MU_VERSION = afaeb157b8ba38a4a9bdf426ba663f7efb2cc6f8
LIBRETRO_MU_SITE = $(call github,libretro,Mu,$(LIBRETRO_MU_VERSION))
LIBRETRO_MU_LICENSE = CC-BY-NC-3.0
LIBRETRO_MU_LICENSE_FILES = LICENSE

define LIBRETRO_MU_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(LIBRETRO_CORE_ENV) $(MAKE) -C $(@D)/libretroBuildSystem -f Makefile \
		$(LIBRETRO_CORE_MAKE_OPTS) LD="$(TARGET_CC)"
endef

define LIBRETRO_MU_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/libretroBuildSystem/mu_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/mu_libretro.so
endef

$(eval $(generic-package))
