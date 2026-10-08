################################################################################
# nuubos-lighting
################################################################################
NUUBOS_LIGHTING_VERSION = 1.0.0
NUUBOS_LIGHTING_SITE = $(BR2_EXTERNAL_NUUBOS_PATH)/package/nuubos/nuubos-lighting/src
NUUBOS_LIGHTING_SITE_METHOD = local
NUUBOS_LIGHTING_LICENSE = MIT
NUUBOS_LIGHTING_LICENSE_FILES = LICENSE
NUUBOS_LIGHTING_DEPENDENCIES = dbus nuubos-rgb
NUUBOS_LIGHTING_DBUS_CFLAGS = -I$(STAGING_DIR)/usr/include/dbus-1.0 -I$(STAGING_DIR)/usr/lib/dbus-1.0/include
define NUUBOS_LIGHTING_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) $(NUUBOS_LIGHTING_DBUS_CFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror $(@D)/lightingd.c \
		-o $(@D)/nuubos-lightingd -ldbus-1 -lm
endef
define NUUBOS_LIGHTING_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/nuubos-lightingd $(TARGET_DIR)/usr/sbin/nuubos-lightingd
	$(INSTALL) -D -m 0755 $(@D)/S49nuubos-lightingd $(TARGET_DIR)/etc/init.d/S49nuubos-lightingd
	$(INSTALL) -D -m 0644 $(@D)/org.nuubOS.Lighting.conf $(TARGET_DIR)/usr/share/dbus-1/system.d/org.nuubOS.Lighting.conf
endef
$(eval $(generic-package))
