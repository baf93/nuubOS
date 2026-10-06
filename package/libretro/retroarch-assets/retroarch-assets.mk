################################################################################
#
# retroarch-assets
#
################################################################################

RETROARCH_ASSETS_VERSION = d9f969054dc7fbb6fa89519036d2b971e0855b51
RETROARCH_ASSETS_SITE = $(call github,libretro,retroarch-assets,$(RETROARCH_ASSETS_VERSION))
RETROARCH_ASSETS_LICENSE = CC-BY-4.0
RETROARCH_ASSETS_LICENSE_FILES = COPYING

# Ozone (the default RetroArch Advanced menu) only: its icons and fonts,
# the XMB monochrome icons it uses for content/systems and the western
# fallback + OSD fonts. CJK/Thai/Ethiopic fonts, wallpapers, sounds and the
# other menu themes are not installed (about 15 MB instead of ~600 MB).
RETROARCH_ASSETS_DIRS = ozone xmb/monochrome/png
RETROARCH_ASSETS_FILES = pkg/fallback-font.ttf pkg/osd-font.ttf

define RETROARCH_ASSETS_INSTALL_TARGET_CMDS
	rm -rf $(TARGET_DIR)/usr/share/retroarch/assets
	$(foreach d,$(RETROARCH_ASSETS_DIRS),\
		mkdir -p $(TARGET_DIR)/usr/share/retroarch/assets/$(d) && \
		cp -a $(@D)/$(d)/. $(TARGET_DIR)/usr/share/retroarch/assets/$(d)/$(sep))
	$(foreach f,$(RETROARCH_ASSETS_FILES),\
		$(INSTALL) -D -m 0644 $(@D)/$(f) \
			$(TARGET_DIR)/usr/share/retroarch/assets/$(f)$(sep))
	$(INSTALL) -D -m 0644 $(@D)/COPYING \
		$(TARGET_DIR)/usr/share/retroarch/assets/COPYING
endef

$(eval $(generic-package))
