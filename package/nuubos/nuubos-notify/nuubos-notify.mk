################################################################################
#
# nuubos-notify
#
################################################################################

NUUBOS_NOTIFY_VERSION = 0.1.0
NUUBOS_NOTIFY_SITE = $(BR2_EXTERNAL_NUUBOS_PATH)/package/nuubos/nuubos-notify/src
NUUBOS_NOTIFY_SITE_METHOD = local
NUUBOS_NOTIFY_LICENSE = MIT
NUUBOS_NOTIFY_LICENSE_FILES = LICENSE
NUUBOS_NOTIFY_INSTALL_STAGING = YES

define NUUBOS_NOTIFY_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror \
		$(@D)/notifyd.c -o $(@D)/nuubos-notifyd
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror \
		$(@D)/notifyctl.c -o $(@D)/nuubos-notifyctl
endef

define NUUBOS_NOTIFY_INSTALL_STAGING_CMDS
	$(INSTALL) -D -m 0644 $(@D)/notify.h \
		$(STAGING_DIR)/usr/include/nuubos/notify.h
endef

define NUUBOS_NOTIFY_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/nuubos-notifyd \
		$(TARGET_DIR)/usr/sbin/nuubos-notifyd
	$(INSTALL) -D -m 0755 $(@D)/nuubos-notifyctl \
		$(TARGET_DIR)/usr/bin/nuubos-notifyctl
	$(INSTALL) -D -m 0755 $(@D)/S40nuubos-notifyd \
		$(TARGET_DIR)/etc/init.d/S40nuubos-notifyd
endef

$(eval $(generic-package))
