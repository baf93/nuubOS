################################################################################
#
# nuubos-media
#
################################################################################

NUUBOS_MEDIA_VERSION = 0.1.0
NUUBOS_MEDIA_SITE = $(BR2_EXTERNAL_NUUBOS_PATH)/package/nuubos/nuubos-media/src
NUUBOS_MEDIA_SITE_METHOD = local
NUUBOS_MEDIA_LICENSE = MIT, CC-BY-4.0 (Media icon)
NUUBOS_MEDIA_LICENSE_FILES = LICENSE app/ATTRIBUTION.txt app/LICENSE-CC-BY-4.0.txt
NUUBOS_MEDIA_DEPENDENCIES = nuubos-notify cjson libcurl

define NUUBOS_MEDIA_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-format-truncation \
		$(@D)/mediad.c -o $(@D)/nuubos-mediad -lcjson
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror \
		$(@D)/playerctl.c -o $(@D)/nuubos-playerctl
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-format-truncation \
		$(@D)/jellyfinctl.c -o $(@D)/nuubos-jellyfinctl -lcjson -lcurl
endef

define NUUBOS_MEDIA_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/nuubos-mediad $(TARGET_DIR)/usr/sbin/nuubos-mediad
	$(INSTALL) -D -m 0755 $(@D)/nuubos-playerctl $(TARGET_DIR)/usr/bin/nuubos-playerctl
	$(INSTALL) -D -m 0755 $(@D)/nuubos-jellyfinctl $(TARGET_DIR)/usr/bin/nuubos-jellyfinctl
	$(INSTALL) -D -m 0755 $(@D)/S49nuubos-mediad $(TARGET_DIR)/etc/init.d/S49nuubos-mediad
	$(INSTALL) -D -m 0755 $(@D)/56-media \
		$(TARGET_DIR)/usr/lib/nuubos/lifecycle-hooks/pre-power.d/56-media
	$(INSTALL) -D -m 0644 $(@D)/app/media.app $(TARGET_DIR)/usr/share/nuubos/applications/media.app
	$(INSTALL) -D -m 0644 $(@D)/app/media.png $(TARGET_DIR)/usr/share/nuubos/applications/icons/media.png
	$(INSTALL) -D -m 0644 $(@D)/app/ATTRIBUTION.txt \
		$(TARGET_DIR)/usr/share/nuubos/applications/icons/ATTRIBUTION-media.txt
endef

$(eval $(generic-package))
