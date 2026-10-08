################################################################################
#
# nuubos-rgb
#
################################################################################

NUUBOS_RGB_VERSION = 2.0
NUUBOS_RGB_SITE = $(BR2_EXTERNAL_NUUBOS_PATH)/package/nuubos/nuubos-rgb/src
NUUBOS_RGB_SITE_METHOD = local
NUUBOS_RGB_LICENSE = MIT
NUUBOS_RGB_LICENSE_FILES = LICENSE

define NUUBOS_RGB_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror \
		$(@D)/rgb_common.c $(@D)/rgbd.c \
		-o $(@D)/nuubos-rgbd
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror \
		$(@D)/rgb_common.c $(@D)/rgbctl.c \
		-o $(@D)/nuubos-rgbctl
endef

define NUUBOS_RGB_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/nuubos-rgbd \
		$(TARGET_DIR)/usr/sbin/nuubos-rgbd
	$(INSTALL) -D -m 0755 $(@D)/nuubos-rgbctl \
		$(TARGET_DIR)/usr/bin/nuubos-rgbctl
	$(INSTALL) -D -m 0755 $(@D)/S07nuubos-rgb \
		$(TARGET_DIR)/etc/init.d/S07nuubos-rgb
endef

$(eval $(generic-package))
