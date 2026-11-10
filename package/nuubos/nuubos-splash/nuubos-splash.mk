################################################################################
#
# nuubos-splash
#
################################################################################

NUUBOS_SPLASH_VERSION = 0.1
NUUBOS_SPLASH_SITE = $(BR2_EXTERNAL_NUUBOS_PATH)/package/nuubos/nuubos-splash/src
NUUBOS_SPLASH_SITE_METHOD = local
NUUBOS_SPLASH_LICENSE = GPL-3.0+
NUUBOS_SPLASH_LICENSE_FILES = LICENSE
NUUBOS_SPLASH_DEPENDENCIES = libdrm

define NUUBOS_SPLASH_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-I$(STAGING_DIR)/usr/include/libdrm \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror \
		$(@D)/nuubos-splash.c -ldrm \
		-o $(@D)/nuubos-splash
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-I$(STAGING_DIR)/usr/include/libdrm \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror \
		$(@D)/nuubos-charge.c -ldrm -lm \
		-o $(@D)/nuubos-charge
endef

define NUUBOS_SPLASH_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/nuubos-splash \
		$(TARGET_DIR)/usr/bin/nuubos-splash
	$(INSTALL) -D -m 0755 $(@D)/nuubos-charge \
		$(TARGET_DIR)/usr/bin/nuubos-charge
	$(INSTALL) -D -m 0755 $(@D)/S08nuubos-charge \
		$(TARGET_DIR)/etc/init.d/S08nuubos-charge
endef

$(eval $(generic-package))
