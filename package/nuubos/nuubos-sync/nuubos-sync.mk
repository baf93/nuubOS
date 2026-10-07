################################################################################
#
# nuubos-sync
#
################################################################################

NUUBOS_SYNC_VERSION = 0.1.0
NUUBOS_SYNC_SITE = $(BR2_EXTERNAL_NUUBOS_PATH)/package/nuubos/nuubos-sync/src
NUUBOS_SYNC_SITE_METHOD = local
NUUBOS_SYNC_LICENSE = MIT
NUUBOS_SYNC_LICENSE_FILES = LICENSE
NUUBOS_SYNC_DEPENDENCIES = cjson libcurl

define NUUBOS_SYNC_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror \
		$(@D)/syncd.c -o $(@D)/nuubos-syncd
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-format-truncation \
		$(@D)/syncctl.c -o $(@D)/nuubos-syncctl -lcjson -lcurl
endef

define NUUBOS_SYNC_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/nuubos-syncd $(TARGET_DIR)/usr/sbin/nuubos-syncd
	$(INSTALL) -D -m 0755 $(@D)/nuubos-syncctl $(TARGET_DIR)/usr/bin/nuubos-syncctl
	$(INSTALL) -D -m 0755 $(@D)/S49nuubos-syncd $(TARGET_DIR)/etc/init.d/S49nuubos-syncd
endef

$(eval $(generic-package))
