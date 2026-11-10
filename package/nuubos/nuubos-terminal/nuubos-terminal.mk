################################################################################
#
# nuubos-terminal
#
################################################################################

NUUBOS_TERMINAL_VERSION = 0.1.0
NUUBOS_TERMINAL_SITE = $(BR2_EXTERNAL_NUUBOS_PATH)/package/nuubos/nuubos-terminal/src
NUUBOS_TERMINAL_SITE_METHOD = local
NUUBOS_TERMINAL_LICENSE = GPL-3.0+, CC-BY-4.0 (Terminal icon)
NUUBOS_TERMINAL_LICENSE_FILES = LICENSE app/ATTRIBUTION.txt app/LICENSE-CC-BY-4.0.txt
NUUBOS_TERMINAL_DEPENDENCIES = foot

define NUUBOS_TERMINAL_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-format-truncation \
		$(@D)/termd.c -o $(@D)/nuubos-termd
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror \
		$(@D)/termctl.c -o $(@D)/nuubos-termctl
endef

define NUUBOS_TERMINAL_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/nuubos-termd $(TARGET_DIR)/usr/sbin/nuubos-termd
	$(INSTALL) -D -m 0755 $(@D)/nuubos-termctl $(TARGET_DIR)/usr/bin/nuubos-termctl
	$(INSTALL) -D -m 0755 $(@D)/S49nuubos-termd $(TARGET_DIR)/etc/init.d/S49nuubos-termd
	$(INSTALL) -D -m 0755 $(@D)/57-terminal \
		$(TARGET_DIR)/usr/lib/nuubos/lifecycle-hooks/pre-power.d/57-terminal
	$(INSTALL) -D -m 0644 $(@D)/foot.ini $(TARGET_DIR)/etc/nuubos/terminal/foot.ini
	$(INSTALL) -D -m 0644 $(@D)/app/terminal.app $(TARGET_DIR)/usr/share/nuubos/applications/terminal.app
	$(INSTALL) -D -m 0644 $(@D)/app/terminal.png $(TARGET_DIR)/usr/share/nuubos/applications/icons/terminal.png
	$(INSTALL) -D -m 0644 $(@D)/app/ATTRIBUTION.txt \
		$(TARGET_DIR)/usr/share/nuubos/applications/icons/ATTRIBUTION-terminal.txt
endef

$(eval $(generic-package))
