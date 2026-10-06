################################################################################
#
# libretro-xmil
#
################################################################################

# X Millennium (Sharp X1). Pinned upstream commit; part of the
# default H700 core matrix (package/libretro/README.md).
LIBRETRO_XMIL_VERSION = 3106aa54ffb244cca4019a6e95dacaa698781bc3
LIBRETRO_XMIL_SITE = $(call github,libretro,xmil-libretro,$(LIBRETRO_XMIL_VERSION))
LIBRETRO_XMIL_LICENSE = BSD-3-Clause
LIBRETRO_XMIL_LICENSE_FILES = LICENSE

define LIBRETRO_XMIL_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(LIBRETRO_CORE_ENV) $(MAKE) -C $(@D)/libretro -f Makefile \
		$(LIBRETRO_CORE_MAKE_OPTS) LD="$(TARGET_CC)"
endef

define LIBRETRO_XMIL_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/libretro/x1_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/x1_libretro.so
endef

$(eval $(generic-package))
