################################################################################
# nuubos-rumble
################################################################################
NUUBOS_RUMBLE_VERSION = 0.4.0
NUUBOS_RUMBLE_SITE = $(BR2_EXTERNAL_NUUBOS_PATH)/package/nuubos/nuubos-rumble/src
NUUBOS_RUMBLE_SITE_METHOD = local
NUUBOS_RUMBLE_LICENSE = MIT
NUUBOS_RUMBLE_LICENSE_FILES = LICENSE
NUUBOS_RUMBLE_DEPENDENCIES = dbus
NUUBOS_RUMBLE_DBUS_CFLAGS = -I$(STAGING_DIR)/usr/include/dbus-1.0 -I$(STAGING_DIR)/usr/lib/dbus-1.0/include
define NUUBOS_RUMBLE_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) $(NUUBOS_RUMBLE_DBUS_CFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror $(@D)/rumbled.c \
		-o $(@D)/nuubos-rumbled -ldbus-1
endef
define NUUBOS_RUMBLE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/nuubos-rumbled $(TARGET_DIR)/usr/sbin/nuubos-rumbled
	$(INSTALL) -D -m 0755 $(@D)/S48nuubos-rumbled $(TARGET_DIR)/etc/init.d/S48nuubos-rumbled
	$(INSTALL) -D -m 0644 $(@D)/org.nuubOS.Rumble.conf $(TARGET_DIR)/usr/share/dbus-1/system.d/org.nuubOS.Rumble.conf
endef
$(eval $(generic-package))
