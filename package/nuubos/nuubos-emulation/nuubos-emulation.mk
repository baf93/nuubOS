################################################################################
#
# nuubos-emulation
#
################################################################################

NUUBOS_EMULATION_VERSION = 0.1.0
NUUBOS_EMULATION_SITE = $(BR2_EXTERNAL_NUUBOS_PATH)/package/nuubos/nuubos-emulation/src
NUUBOS_EMULATION_SITE_METHOD = local
NUUBOS_EMULATION_LICENSE = GPL-3.0+, CC-BY-4.0 (Netplay icon)
NUUBOS_EMULATION_LICENSE_FILES = LICENSE app/ATTRIBUTION.txt app/LICENSE-CC-BY-4.0.txt
NUUBOS_EMULATION_DEPENDENCIES = nuubos-notify libcurl

define NUUBOS_EMULATION_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-format-truncation \
		$(@D)/emud.c -o $(@D)/nuubos-emud
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror \
		$(@D)/emuctl.c -o $(@D)/nuubos-emuctl
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror \
		$(@D)/achievementsctl.c -o $(@D)/nuubos-achievementsctl -lcurl
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-format-truncation \
		$(@D)/libretroctl.c -o $(@D)/nuubos-libretroctl -lcurl
endef

define NUUBOS_EMULATION_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/nuubos-emud $(TARGET_DIR)/usr/sbin/nuubos-emud
	$(INSTALL) -D -m 0755 $(@D)/nuubos-emuctl $(TARGET_DIR)/usr/bin/nuubos-emuctl
	$(INSTALL) -D -m 0755 $(@D)/nuubos-achievementsctl $(TARGET_DIR)/usr/bin/nuubos-achievementsctl
	$(INSTALL) -D -m 0755 $(@D)/nuubos-libretroctl $(TARGET_DIR)/usr/bin/nuubos-libretroctl
	$(INSTALL) -D -m 0644 $(@D)/core-install.job $(TARGET_DIR)/usr/share/nuubos/jobs/core-install.job
	$(INSTALL) -D -m 0644 $(@D)/cheats-update.job $(TARGET_DIR)/usr/share/nuubos/jobs/cheats-update.job
	$(INSTALL) -D -m 0644 $(@D)/libretro.conf \
		$(TARGET_DIR)/usr/share/nuubos/emulation/libretro.conf
	$(INSTALL) -D -m 0644 $(@D)/unsupported-cores.conf \
		$(TARGET_DIR)/usr/share/nuubos/emulation/unsupported-cores.conf
	$(INSTALL) -D -m 0644 $(@D)/app/netplay.app \
		$(TARGET_DIR)/usr/share/nuubos/applications/netplay.app
	$(INSTALL) -D -m 0644 $(@D)/app/netplay.png \
		$(TARGET_DIR)/usr/share/nuubos/applications/icons/netplay.png
	$(INSTALL) -D -m 0644 $(@D)/app/retroarch.app \
		$(TARGET_DIR)/usr/share/nuubos/applications/retroarch.app
	$(INSTALL) -D -m 0644 $(@D)/app/retroarch.png \
		$(TARGET_DIR)/usr/share/nuubos/applications/icons/retroarch.png
	$(INSTALL) -D -m 0644 $(@D)/app/ATTRIBUTION.txt \
		$(TARGET_DIR)/usr/share/nuubos/applications/icons/ATTRIBUTION-netplay.txt
	$(INSTALL) -D -m 0644 $(@D)/app/ATTRIBUTION.txt \
		$(TARGET_DIR)/usr/share/nuubos/applications/icons/ATTRIBUTION-retroarch.txt
	$(INSTALL) -D -m 0755 $(@D)/S49nuubos-emud $(TARGET_DIR)/etc/init.d/S49nuubos-emud
	$(INSTALL) -D -m 0755 $(@D)/50-emulation \
		$(TARGET_DIR)/usr/lib/nuubos/lifecycle-hooks/pre-power.d/50-emulation
	$(INSTALL) -D -m 0755 $(@D)/nuubos-biosctl $(TARGET_DIR)/usr/bin/nuubos-biosctl
	$(INSTALL) -D -m 0644 $(@D)/bios.conf \
		$(TARGET_DIR)/usr/share/nuubos/emulation/bios.conf
	$(INSTALL) -D -m 0644 $(@D)/cores.conf \
		$(TARGET_DIR)/usr/share/nuubos/emulation/cores.conf
	$(INSTALL) -D -m 0644 $(@D)/retroarch.cfg \
		$(TARGET_DIR)/usr/share/nuubos/emulation/retroarch.cfg
	$(INSTALL) -D -m 0644 $(@D)/retroarch-core-options.cfg \
		$(TARGET_DIR)/usr/share/nuubos/emulation/retroarch-core-options.cfg
	$(INSTALL) -D -m 0644 $(@D)/core-option-updates.conf \
		$(TARGET_DIR)/usr/share/nuubos/emulation/core-option-updates.conf
	$(INSTALL) -D -m 0644 $(@D)/shaders/sharp-bilinear.glsl \
		$(TARGET_DIR)/usr/share/nuubos/emulation/shaders/sharp-bilinear.glsl
	$(INSTALL) -D -m 0644 $(@D)/shaders/global.glslp \
		$(TARGET_DIR)/usr/share/nuubos/emulation/shaders/global.glslp
	$(INSTALL) -D -m 0644 $(@D)/shaders/crt.glsl \
		$(TARGET_DIR)/usr/share/nuubos/emulation/shaders/crt.glsl
	# the bezel is drawn by RetroArch since 2026-10-18 (patch 0010)
	rm -f $(TARGET_DIR)/usr/share/nuubos/emulation/shaders/bezel-wordmark.png \
		$(TARGET_DIR)/usr/share/nuubos/emulation/shaders/bezel.glsl \
		$(TARGET_DIR)/usr/share/nuubos/emulation/shaders/bezel-picture.glsl
	$(INSTALL) -D -m 0644 $(@D)/shaders/bezel-logo.png \
		$(TARGET_DIR)/usr/share/nuubos/emulation/shaders/bezel-logo.png
	$(INSTALL) -D -m 0644 $(@D)/display.conf \
		$(TARGET_DIR)/usr/share/nuubos/emulation/display.conf
	$(INSTALL) -D -m 0644 "$(@D)/autoconfig/udev/nuubOS Gamepad.cfg" \
		"$(TARGET_DIR)/usr/share/retroarch/autoconfig/udev/nuubOS Gamepad.cfg"
endef

$(eval $(generic-package))
