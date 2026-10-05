################################################################################
# nuubos-localization
################################################################################
NUUBOS_LOCALIZATION_VERSION = 0.1.0
NUUBOS_LOCALIZATION_SITE = $(BR2_EXTERNAL_NUUBOS_PATH)/package/nuubos/nuubos-localization/src
NUUBOS_LOCALIZATION_SITE_METHOD = local
NUUBOS_LOCALIZATION_LICENSE = MIT
NUUBOS_LOCALIZATION_LICENSE_FILES = LICENSE

define NUUBOS_LOCALIZATION_BUILD_CMDS
	$(@D)/i18n/validate-i18n.sh $(@D)/i18n
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror $(@D)/localizationd.c -o $(@D)/nuubos-localizationd
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror $(@D)/localizationctl.c -o $(@D)/nuubos-localizationctl
endef

define NUUBOS_LOCALIZATION_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/nuubos-localizationd $(TARGET_DIR)/usr/sbin/nuubos-localizationd
	$(INSTALL) -D -m 0755 $(@D)/nuubos-localizationctl $(TARGET_DIR)/usr/bin/nuubos-localizationctl
	$(INSTALL) -D -m 0755 $(@D)/S48nuubos-localization $(TARGET_DIR)/etc/init.d/S48nuubos-localization
	$(INSTALL) -d -m 0755 $(TARGET_DIR)/usr/share/nuubos/i18n
	cp -a $(@D)/i18n/*.lang $(TARGET_DIR)/usr/share/nuubos/i18n/
endef

$(eval $(generic-package))
