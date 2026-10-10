################################################################################
#
# nuubos-controllers
#
################################################################################

NUUBOS_CONTROLLERS_VERSION = 0.4.0
NUUBOS_CONTROLLERS_SITE = $(BR2_EXTERNAL_NUUBOS_PATH)/package/nuubos/nuubos-controllers/src
NUUBOS_CONTROLLERS_SITE_METHOD = local
NUUBOS_CONTROLLERS_LICENSE = MIT
NUUBOS_CONTROLLERS_LICENSE_FILES = LICENSE
NUUBOS_CONTROLLERS_DEPENDENCIES = dbus nuubos-input nuubos-notify

NUUBOS_CONTROLLERS_DBUS_CFLAGS = \
	-I$(STAGING_DIR)/usr/include/dbus-1.0 \
	-I$(STAGING_DIR)/usr/lib/dbus-1.0/include

define NUUBOS_CONTROLLERS_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		$(NUUBOS_CONTROLLERS_DBUS_CFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror \
		$(@D)/controllersd.c \
		-o $(@D)/nuubos-controllersd \
		-ldbus-1
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		$(NUUBOS_CONTROLLERS_DBUS_CFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror \
		$(@D)/controllersctl.c \
		-o $(@D)/nuubos-controllersctl \
		-ldbus-1
endef

define NUUBOS_CONTROLLERS_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/nuubos-controllersd \
		$(TARGET_DIR)/usr/sbin/nuubos-controllersd
	$(INSTALL) -D -m 0755 $(@D)/nuubos-controllersctl \
		$(TARGET_DIR)/usr/bin/nuubos-controllersctl
	$(INSTALL) -D -m 0755 $(@D)/S48nuubos-controllersd \
		$(TARGET_DIR)/etc/init.d/S48nuubos-controllersd
	$(INSTALL) -D -m 0644 $(@D)/org.nuubOS.Controllers.conf \
		$(TARGET_DIR)/usr/share/dbus-1/system.d/org.nuubOS.Controllers.conf
endef

$(eval $(generic-package))
