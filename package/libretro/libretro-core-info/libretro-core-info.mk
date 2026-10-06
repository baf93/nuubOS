################################################################################
#
# libretro-core-info
#
################################################################################

LIBRETRO_CORE_INFO_VERSION = 5a74858ab2f7a50cebb5a6330895bc38899531c0
LIBRETRO_CORE_INFO_SITE = $(call github,libretro,libretro-core-info,$(LIBRETRO_CORE_INFO_VERSION))
LIBRETRO_CORE_INFO_LICENSE = MIT
LIBRETRO_CORE_INFO_LICENSE_FILES = COPYING

define LIBRETRO_CORE_INFO_INSTALL_TARGET_CMDS
	mkdir -p $(TARGET_DIR)/usr/share/libretro/info
	$(INSTALL) -m 0644 $(@D)/*.info $(TARGET_DIR)/usr/share/libretro/info/
endef

$(eval $(generic-package))
