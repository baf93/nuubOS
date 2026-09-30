################################################################################
#
# nuubos-bluetooth
#
################################################################################

NUUBOS_BLUETOOTH_VERSION = 0.1.0
NUUBOS_BLUETOOTH_SITE = $(BR2_EXTERNAL_NUUBOS_PATH)/package/nuubos/nuubos-bluetooth/src
NUUBOS_BLUETOOTH_SITE_METHOD = local
NUUBOS_BLUETOOTH_LICENSE = MIT
NUUBOS_BLUETOOTH_LICENSE_FILES = LICENSE
NUUBOS_BLUETOOTH_DEPENDENCIES = dbus bluez5_utils

NUUBOS_BLUETOOTH_DBUS_CFLAGS = \
	-I$(STAGING_DIR)/usr/include/dbus-1.0 \
	-I$(STAGING_DIR)/usr/lib/dbus-1.0/include

define NUUBOS_BLUETOOTH_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		$(NUUBOS_BLUETOOTH_DBUS_CFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror \
		$(@D)/bluetoothd.c \
		-o $(@D)/nuubos-bluetoothd \
		-ldbus-1
endef

define NUUBOS_BLUETOOTH_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/nuubos-bluetoothd \
		$(TARGET_DIR)/usr/sbin/nuubos-bluetoothd
	$(INSTALL) -D -m 0755 $(@D)/S45nuubos-bluetoothd \
		$(TARGET_DIR)/etc/init.d/S45nuubos-bluetoothd
	$(INSTALL) -D -m 0644 $(@D)/org.nuubOS.Bluetooth.conf \
		$(TARGET_DIR)/usr/share/dbus-1/system.d/org.nuubOS.Bluetooth.conf
endef

$(eval $(generic-package))
