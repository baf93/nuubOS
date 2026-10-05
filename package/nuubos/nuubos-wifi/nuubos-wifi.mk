################################################################################
#
# nuubos-wifi
#
################################################################################

NUUBOS_WIFI_VERSION = 0.2.0
NUUBOS_WIFI_SITE = $(BR2_EXTERNAL_NUUBOS_PATH)/package/nuubos/nuubos-wifi/src
NUUBOS_WIFI_SITE_METHOD = local
NUUBOS_WIFI_LICENSE = MIT
NUUBOS_WIFI_LICENSE_FILES = LICENSE
NUUBOS_WIFI_DEPENDENCIES = dbus wpa_supplicant nuubos-notify

NUUBOS_WIFI_DBUS_CFLAGS = \
	-I$(STAGING_DIR)/usr/include/dbus-1.0 \
	-I$(STAGING_DIR)/usr/lib/dbus-1.0/include

define NUUBOS_WIFI_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		$(NUUBOS_WIFI_DBUS_CFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror \
		$(@D)/wifid.c \
		-o $(@D)/nuubos-wifid \
		-ldbus-1
endef

define NUUBOS_WIFI_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/nuubos-wifid \
		$(TARGET_DIR)/usr/sbin/nuubos-wifid
	$(INSTALL) -D -m 0755 $(@D)/S44nuubos-wifid \
		$(TARGET_DIR)/etc/init.d/S44nuubos-wifid
	$(INSTALL) -D -m 0755 $(@D)/nuubos-wifi-event \
		$(TARGET_DIR)/usr/libexec/nuubos-wifi-event
	$(INSTALL) -D -m 0755 $(@D)/nuubos-wifi-udhcpc \
		$(TARGET_DIR)/usr/libexec/nuubos-wifi-udhcpc
	$(INSTALL) -D -m 0644 $(@D)/org.nuubOS.Wifi.conf \
		$(TARGET_DIR)/usr/share/dbus-1/system.d/org.nuubOS.Wifi.conf
endef

$(eval $(generic-package))
