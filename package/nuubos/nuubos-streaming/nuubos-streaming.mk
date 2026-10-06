################################################################################
#
# nuubos-streaming
#
################################################################################

NUUBOS_STREAMING_VERSION = 0.1.0
NUUBOS_STREAMING_SITE = $(BR2_EXTERNAL_NUUBOS_PATH)/package/nuubos/nuubos-streaming/src
NUUBOS_STREAMING_SITE_METHOD = local
NUUBOS_STREAMING_LICENSE = MIT
NUUBOS_STREAMING_LICENSE_FILES = LICENSE
NUUBOS_STREAMING_DEPENDENCIES = nuubos-notify

define NUUBOS_STREAMING_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-format-truncation \
		$(@D)/streamd.c -o $(@D)/nuubos-streamd
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror \
		$(@D)/streamctl.c -o $(@D)/nuubos-streamctl
endef

define NUUBOS_STREAMING_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/nuubos-streamd $(TARGET_DIR)/usr/sbin/nuubos-streamd
	$(INSTALL) -D -m 0755 $(@D)/nuubos-streamctl $(TARGET_DIR)/usr/bin/nuubos-streamctl
	$(INSTALL) -D -m 0755 $(@D)/S49nuubos-streamd $(TARGET_DIR)/etc/init.d/S49nuubos-streamd
	$(INSTALL) -D -m 0755 $(@D)/55-streaming \
		$(TARGET_DIR)/usr/lib/nuubos/lifecycle-hooks/pre-power.d/55-streaming
	$(INSTALL) -D -m 0644 $(@D)/app/moonlight.app \
		$(TARGET_DIR)/usr/share/nuubos/applications/moonlight.app
	$(INSTALL) -D -m 0644 $(@D)/app/moonlight.png \
		$(TARGET_DIR)/usr/share/nuubos/applications/icons/moonlight.png
endef

$(eval $(generic-package))
