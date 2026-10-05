################################################################################
# nuubos-users
################################################################################
NUUBOS_USERS_VERSION = 0.1.0
NUUBOS_USERS_SITE = $(BR2_EXTERNAL_NUUBOS_PATH)/package/nuubos/nuubos-users/src
NUUBOS_USERS_SITE_METHOD = local
NUUBOS_USERS_LICENSE = MIT, CC-BY-4.0 (avatars 01-16), CC0-1.0 (avatars 17-24)
NUUBOS_USERS_LICENSE_FILES = LICENSE avatars/LICENSE-DiceBear.txt

define NUUBOS_USERS_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -std=c11 -D_GNU_SOURCE -D_XOPEN_SOURCE=700 -Wall -Wextra -Werror $(@D)/usersd.c -o $(@D)/nuubos-usersd
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror $(@D)/usersctl.c -o $(@D)/nuubos-usersctl
endef

define NUUBOS_USERS_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/nuubos-usersd $(TARGET_DIR)/usr/sbin/nuubos-usersd
	$(INSTALL) -D -m 0755 $(@D)/nuubos-usersctl $(TARGET_DIR)/usr/bin/nuubos-usersctl
	$(INSTALL) -D -m 0755 $(@D)/S46nuubos-users $(TARGET_DIR)/etc/init.d/S46nuubos-users
	$(INSTALL) -d -m 0755 $(TARGET_DIR)/usr/share/nuubos/avatars
	cp -a $(@D)/avatars/*.png $(TARGET_DIR)/usr/share/nuubos/avatars/
	$(INSTALL) -D -m 0644 $(@D)/avatars/README.txt $(TARGET_DIR)/usr/share/nuubos/avatars/README.txt
	$(INSTALL) -D -m 0644 $(@D)/avatars/LICENSE-DiceBear.txt $(TARGET_DIR)/usr/share/nuubos/avatars/LICENSE-DiceBear.txt
endef

$(eval $(generic-package))
