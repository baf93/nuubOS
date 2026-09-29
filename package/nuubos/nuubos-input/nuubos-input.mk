################################################################################
# nuubos-input
################################################################################
NUUBOS_INPUT_VERSION = 0.1.0
NUUBOS_INPUT_SITE = $(BR2_EXTERNAL_NUUBOS_PATH)/package/nuubos/nuubos-input/src
NUUBOS_INPUT_SITE_METHOD = local
NUUBOS_INPUT_LICENSE = MIT
NUUBOS_INPUT_LICENSE_FILES = LICENSE

define NUUBOS_INPUT_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -std=c11 -D_GNU_SOURCE \
		$(@D)/inputd.c -o $(@D)/nuubos-inputd
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -std=c11 -D_GNU_SOURCE \
		$(@D)/inputctl.c -o $(@D)/nuubos-inputctl
endef

define NUUBOS_INPUT_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/nuubos-inputd $(TARGET_DIR)/usr/sbin/nuubos-inputd
	$(INSTALL) -D -m 0755 $(@D)/nuubos-inputctl $(TARGET_DIR)/usr/bin/nuubos-inputctl
	$(INSTALL) -D -m 0755 $(@D)/S47nuubos-inputd $(TARGET_DIR)/etc/init.d/S47nuubos-inputd
	$(INSTALL) -D -m 0644 $(@D)/input-bindings.conf $(TARGET_DIR)/etc/nuubos/input-bindings.conf
endef
$(eval $(generic-package))
