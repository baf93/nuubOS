################################################################################
#
# nuubos-web
#
################################################################################

NUUBOS_WEB_VERSION = 0.1.0
NUUBOS_WEB_SITE = $(BR2_EXTERNAL_NUUBOS_PATH)/package/nuubos/nuubos-web/src
NUUBOS_WEB_SITE_METHOD = local
NUUBOS_WEB_LICENSE = MIT, CC-BY-4.0 (Web icon)
NUUBOS_WEB_LICENSE_FILES = LICENSE app/ATTRIBUTION.txt app/LICENSE-CC-BY-4.0.txt
NUUBOS_WEB_DEPENDENCIES = nuubos-notify cog

define NUUBOS_WEB_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-format-truncation \
		$(@D)/webd.c -o $(@D)/nuubos-webd
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror \
		$(@D)/webctl.c -o $(@D)/nuubos-webctl
endef

define NUUBOS_WEB_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/nuubos-webd $(TARGET_DIR)/usr/sbin/nuubos-webd
	$(INSTALL) -D -m 0755 $(@D)/nuubos-webctl $(TARGET_DIR)/usr/bin/nuubos-webctl
	$(INSTALL) -D -m 0755 $(@D)/S49nuubos-webd $(TARGET_DIR)/etc/init.d/S49nuubos-webd
	$(INSTALL) -D -m 0755 $(@D)/57-web \
		$(TARGET_DIR)/usr/lib/nuubos/lifecycle-hooks/pre-power.d/57-web
	$(INSTALL) -D -m 0644 $(@D)/app/web.app $(TARGET_DIR)/usr/share/nuubos/applications/web.app
	$(INSTALL) -D -m 0644 $(@D)/app/web.png $(TARGET_DIR)/usr/share/nuubos/applications/icons/web.png
	$(INSTALL) -D -m 0644 $(@D)/app/ATTRIBUTION.txt \
		$(TARGET_DIR)/usr/share/nuubos/applications/icons/ATTRIBUTION-web.txt
endef

$(eval $(generic-package))
