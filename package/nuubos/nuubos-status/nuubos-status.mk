################################################################################
#
# nuubos-status
#
################################################################################

NUUBOS_STATUS_VERSION = 0.1.1
NUUBOS_STATUS_SITE = $(BR2_EXTERNAL_NUUBOS_PATH)/package/nuubos/nuubos-status/src
NUUBOS_STATUS_SITE_METHOD = local
NUUBOS_STATUS_LICENSE = MIT
NUUBOS_STATUS_LICENSE_FILES = LICENSE
NUUBOS_STATUS_DEPENDENCIES = wpa_supplicant bluez5_utils nuubos-notify

define NUUBOS_STATUS_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror \
		$(@D)/statusd.c \
		-o $(@D)/nuubos-statusd
endef

define NUUBOS_STATUS_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/nuubos-statusd \
		$(TARGET_DIR)/usr/sbin/nuubos-statusd
	$(INSTALL) -D -m 0755 $(@D)/S48nuubos-status \
		$(TARGET_DIR)/etc/init.d/S48nuubos-status
	$(INSTALL) -D -m 0755 $(@D)/nuubos-status-wifi-event \
		$(TARGET_DIR)/usr/libexec/nuubos-status-wifi-event
	$(INSTALL) -D -m 0755 $(@D)/nuubos-status-time-sync \
		$(TARGET_DIR)/usr/libexec/nuubos-status-time-sync
endef

$(eval $(generic-package))
