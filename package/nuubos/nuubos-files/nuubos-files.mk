################################################################################
#
# nuubos-files
#
################################################################################

NUUBOS_FILES_VERSION = 0.1.0
NUUBOS_FILES_SITE = $(BR2_EXTERNAL_NUUBOS_PATH)/package/nuubos/nuubos-files/src
NUUBOS_FILES_SITE_METHOD = local
NUUBOS_FILES_LICENSE = MIT, CC-BY-4.0 (Files icon)
NUUBOS_FILES_LICENSE_FILES = LICENSE app/ATTRIBUTION.txt app/LICENSE-CC-BY-4.0.txt

define NUUBOS_FILES_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror \
		$(@D)/filesctl.c -o $(@D)/nuubos-filesctl
endef

define NUUBOS_FILES_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/nuubos-filesctl $(TARGET_DIR)/usr/bin/nuubos-filesctl
	$(INSTALL) -D -m 0755 $(@D)/nuubos-mediactl $(TARGET_DIR)/usr/sbin/nuubos-mediactl
	$(INSTALL) -D -m 0755 $(@D)/nuubos-sharesctl $(TARGET_DIR)/usr/sbin/nuubos-sharesctl
	$(INSTALL) -D -m 0755 $(@D)/nuubos-backupctl $(TARGET_DIR)/usr/bin/nuubos-backupctl
	$(INSTALL) -D -m 0644 $(@D)/72-nuubos-removable.rules \
		$(TARGET_DIR)/lib/udev/rules.d/72-nuubos-removable.rules
	for j in file-copy file-move backup-user restore-user; do \
		$(INSTALL) -D -m 0644 $(@D)/$$j.job $(TARGET_DIR)/usr/share/nuubos/jobs/$$j.job || exit 1; \
	done
	$(INSTALL) -d -m 0755 $(TARGET_DIR)/media
	$(INSTALL) -D -m 0644 $(@D)/app/files.app \
		$(TARGET_DIR)/usr/share/nuubos/applications/files.app
	$(INSTALL) -D -m 0644 $(@D)/app/files.png \
		$(TARGET_DIR)/usr/share/nuubos/applications/icons/files.png
	$(INSTALL) -D -m 0644 $(@D)/app/ATTRIBUTION.txt \
		$(TARGET_DIR)/usr/share/nuubos/applications/icons/ATTRIBUTION-files.txt
endef

$(eval $(generic-package))
