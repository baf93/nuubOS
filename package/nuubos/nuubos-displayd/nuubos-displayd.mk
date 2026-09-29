################################################################################
#
# nuubos-displayd
#
################################################################################

NUUBOS_DISPLAYD_VERSION = 1.0
NUUBOS_DISPLAYD_SITE = $(BR2_EXTERNAL_NUUBOS_PATH)/package/nuubos/nuubos-displayd/src
NUUBOS_DISPLAYD_SITE_METHOD = local
NUUBOS_DISPLAYD_LICENSE = MIT
NUUBOS_DISPLAYD_LICENSE_FILES = LICENSE
NUUBOS_DISPLAYD_DEPENDENCIES = libdrm

define NUUBOS_DISPLAYD_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror \
		-I$(STAGING_DIR)/usr/include/libdrm \
		$(@D)/displayd.c $(@D)/kms.c \
		-o $(@D)/nuubos-displayd -ldrm
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror \
		$(@D)/displayctl.c \
		-o $(@D)/nuubos-displayctl
endef

define NUUBOS_DISPLAYD_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/nuubos-displayd \
		$(TARGET_DIR)/usr/sbin/nuubos-displayd
	$(INSTALL) -D -m 0755 $(@D)/nuubos-displayctl \
		$(TARGET_DIR)/usr/bin/nuubos-displayctl
	$(INSTALL) -D -m 0755 $(@D)/S07nuubos-display-brightness \
		$(TARGET_DIR)/etc/init.d/S07nuubos-display-brightness
	$(INSTALL) -D -m 0755 $(@D)/S08nuubos-displayd \
		$(TARGET_DIR)/etc/init.d/S08nuubos-displayd
endef

$(eval $(generic-package))
