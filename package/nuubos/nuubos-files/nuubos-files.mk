################################################################################
#
# nuubos-files
#
################################################################################

NUUBOS_FILES_VERSION = 0.1.0
NUUBOS_FILES_SITE = $(BR2_EXTERNAL_NUUBOS_PATH)/package/nuubos/nuubos-files/src
NUUBOS_FILES_SITE_METHOD = local
NUUBOS_FILES_LICENSE = GPL-3.0+, CC-BY-4.0 (Files icon)
NUUBOS_FILES_LICENSE_FILES = LICENSE app/ATTRIBUTION.txt app/LICENSE-CC-BY-4.0.txt
# libsmb2: the shared folders of a server for Add Network Location.
NUUBOS_FILES_DEPENDENCIES = libsmb2

define NUUBOS_FILES_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror \
		$(@D)/filesctl.c -o $(@D)/nuubos-filesctl
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror \
		$(@D)/smb-browse.c -o $(@D)/nuubos-smb-browse -lsmb2
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror \
		$(@D)/data-move.c -o $(@D)/nuubos-data-move
endef

define NUUBOS_FILES_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/nuubos-filesctl $(TARGET_DIR)/usr/bin/nuubos-filesctl
	$(INSTALL) -D -m 0755 $(@D)/nuubos-mediactl $(TARGET_DIR)/usr/sbin/nuubos-mediactl
	$(INSTALL) -D -m 0755 $(@D)/nuubos-sharesctl $(TARGET_DIR)/usr/sbin/nuubos-sharesctl
	$(INSTALL) -D -m 0755 $(@D)/nuubos-smb-browse $(TARGET_DIR)/usr/libexec/nuubos-smb-browse
	$(INSTALL) -D -m 0755 $(@D)/nuubos-data-move $(TARGET_DIR)/usr/lib/nuubos/nuubos-data-move
	$(INSTALL) -D -m 0644 $(@D)/72-nuubos-removable.rules \
		$(TARGET_DIR)/lib/udev/rules.d/72-nuubos-removable.rules
	for j in file-copy file-move; do \
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
