################################################################################
# nuubos-system
################################################################################
NUUBOS_SYSTEM_VERSION = 0.2.0
NUUBOS_SYSTEM_SITE = $(BR2_EXTERNAL_NUUBOS_PATH)/package/nuubos/nuubos-system/src
NUUBOS_SYSTEM_SITE_METHOD = local
NUUBOS_SYSTEM_LICENSE = MIT
NUUBOS_SYSTEM_LICENSE_FILES = LICENSE
NUUBOS_SYSTEM_DEPENDENCIES = nuubos-input nuubos-status nuubos-notify
NUUBOS_SYSTEM_PRODUCT_VERSION = $(shell cat $(BR2_EXTERNAL_NUUBOS_PATH)/VERSION 2>/dev/null || echo unknown)
NUUBOS_SYSTEM_BUILD_ID = $(shell git -C $(BR2_EXTERNAL_NUUBOS_PATH) describe --always --dirty --abbrev=12 2>/dev/null || echo unknown)
NUUBOS_SYSTEM_VERSION_CPPFLAGS = \
	-DNUUBOS_PRODUCT_VERSION=\"$(NUUBOS_SYSTEM_PRODUCT_VERSION)\" \
	-DNUUBOS_BUILD_ID=\"$(NUUBOS_SYSTEM_BUILD_ID)\"

define NUUBOS_SYSTEM_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -std=c11 -D_GNU_SOURCE \
		$(NUUBOS_SYSTEM_VERSION_CPPFLAGS) \
		-Wall -Wextra -Werror $(@D)/systemd.c -o $(@D)/nuubos-systemd
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -std=c11 -D_GNU_SOURCE \
		-Wall -Wextra -Werror $(@D)/systemctl.c -o $(@D)/nuubos-systemctl
endef

define NUUBOS_SYSTEM_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/nuubos-systemd $(TARGET_DIR)/usr/sbin/nuubos-systemd
	$(INSTALL) -D -m 0755 $(@D)/nuubos-systemctl $(TARGET_DIR)/usr/bin/nuubos-systemctl
	$(INSTALL) -D -m 0755 $(@D)/lifecycle-pre-power $(TARGET_DIR)/usr/lib/nuubos/lifecycle-pre-power
	$(INSTALL) -D -m 0755 $(@D)/lifecycle-lighting $(TARGET_DIR)/usr/lib/nuubos/lifecycle-lighting
	$(INSTALL) -D -m 0755 $(@D)/lifecycle-display $(TARGET_DIR)/usr/lib/nuubos/lifecycle-display
	$(INSTALL) -d -m 0755 $(TARGET_DIR)/usr/lib/nuubos/lifecycle-hooks/pre-power.d
	$(INSTALL) -D -m 0755 $(@D)/S49nuubos-system $(TARGET_DIR)/etc/init.d/S49nuubos-system
endef

$(eval $(generic-package))
