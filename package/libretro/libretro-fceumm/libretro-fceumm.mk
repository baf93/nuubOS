################################################################################
#
# libretro-fceumm
#
################################################################################

# FCEUmm (NES / Famicom / FDS). Pinned upstream commit; part of the
# default H700 core matrix (package/libretro/README.md).
LIBRETRO_FCEUMM_VERSION = 7a542dab1e87679921962a9f056186eca425c0c2
LIBRETRO_FCEUMM_SITE = $(call github,libretro,libretro-fceumm,$(LIBRETRO_FCEUMM_VERSION))
LIBRETRO_FCEUMM_LICENSE = GPL-2.0+
LIBRETRO_FCEUMM_LICENSE_FILES = Copying

define LIBRETRO_FCEUMM_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(LIBRETRO_CORE_ENV) $(MAKE) -C $(@D) -f Makefile.libretro \
		$(LIBRETRO_CORE_MAKE_OPTS) LD="$(TARGET_CC)"
endef

define LIBRETRO_FCEUMM_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/fceumm_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/fceumm_libretro.so
endef

$(eval $(generic-package))
