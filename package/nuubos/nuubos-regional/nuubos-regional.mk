################################################################################
# nuubos-regional
################################################################################
NUUBOS_REGIONAL_VERSION = 0.1.0
NUUBOS_REGIONAL_SITE = $(BR2_EXTERNAL_NUUBOS_PATH)/package/nuubos/nuubos-regional/src
NUUBOS_REGIONAL_SITE_METHOD = local
NUUBOS_REGIONAL_LICENSE = MIT
NUUBOS_REGIONAL_LICENSE_FILES = LICENSE
NUUBOS_REGIONAL_DEPENDENCIES = kbd tzdata

define NUUBOS_REGIONAL_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror $(@D)/regionald.c -o $(@D)/nuubos-regionald
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror $(@D)/regionalctl.c -o $(@D)/nuubos-regionalctl
endef

define NUUBOS_REGIONAL_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/nuubos-regionald $(TARGET_DIR)/usr/sbin/nuubos-regionald
	$(INSTALL) -D -m 0755 $(@D)/nuubos-regionalctl $(TARGET_DIR)/usr/bin/nuubos-regionalctl
	$(INSTALL) -D -m 0755 $(@D)/S47nuubos-regional $(TARGET_DIR)/etc/init.d/S47nuubos-regional
endef

$(eval $(generic-package))
