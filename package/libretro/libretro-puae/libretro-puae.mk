################################################################################
#
# libretro-puae
#
################################################################################

# PUAE (Commodore Amiga), built-in AROS Kickstart replacement. Pinned upstream commit; part of the
# default H700 core matrix (package/libretro/README.md).
LIBRETRO_PUAE_VERSION = 6536174a80d74e6c325aaa5390ff091fac8761d0
LIBRETRO_PUAE_SITE = $(call github,libretro,libretro-uae,$(LIBRETRO_PUAE_VERSION))
LIBRETRO_PUAE_LICENSE = GPL-2.0
LIBRETRO_PUAE_LICENSE_FILES = COPYING

define LIBRETRO_PUAE_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(LIBRETRO_CORE_ENV) $(MAKE) -C $(@D) -f Makefile \
		$(LIBRETRO_CORE_MAKE_OPTS) LD="$(TARGET_CC)"
endef

define LIBRETRO_PUAE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/puae_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/puae_libretro.so
endef

$(eval $(generic-package))
