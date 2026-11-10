################################################################################
#
# nuubos-recovery
#
################################################################################

NUUBOS_RECOVERY_VERSION = 0.1.0
NUUBOS_RECOVERY_SITE = $(BR2_EXTERNAL_NUUBOS_PATH)/package/nuubos/nuubos-recovery/src
NUUBOS_RECOVERY_SITE_METHOD = local
NUUBOS_RECOVERY_LICENSE = GPL-3.0+
NUUBOS_RECOVERY_LICENSE_FILES = LICENSE

define NUUBOS_RECOVERY_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -std=c11 -D_GNU_SOURCE \
		-Wall -Wextra -Werror $(@D)/boot-stages.c -o $(@D)/nuubos-boot-stages
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -std=c11 -D_GNU_SOURCE \
		-Wall -Wextra -Werror $(@D)/dram-blob.c -o $(@D)/nuubos-dram-blob
endef

define NUUBOS_RECOVERY_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/nuubos-boot-stages $(TARGET_DIR)/usr/libexec/nuubos-boot-stages
	$(INSTALL) -D -m 0755 $(@D)/nuubos-dram-blob $(TARGET_DIR)/usr/libexec/nuubos-dram-blob
	$(INSTALL) -D -m 0755 $(@D)/S04nuubos-dram-blob $(TARGET_DIR)/etc/init.d/S04nuubos-dram-blob
	$(INSTALL) -D -m 0755 $(@D)/nuubos-diag $(TARGET_DIR)/usr/bin/nuubos-diag
	$(INSTALL) -D -m 0755 $(@D)/nuubos-diag-persist $(TARGET_DIR)/usr/libexec/nuubos-diag-persist
	$(INSTALL) -D -m 0755 $(@D)/nuubos-recoveryctl $(TARGET_DIR)/usr/sbin/nuubos-recoveryctl
	$(INSTALL) -D -m 0755 $(@D)/nuubos-ui-supervisor $(TARGET_DIR)/usr/bin/nuubos-ui-supervisor
	$(INSTALL) -D -m 0644 $(@D)/support-bundle.job $(TARGET_DIR)/usr/share/nuubos/jobs/support-bundle.job
	$(INSTALL) -D -m 0755 $(@D)/S04nuubos-diagnostics $(TARGET_DIR)/etc/init.d/S04nuubos-diagnostics
	$(INSTALL) -D -m 0755 $(@D)/S99nuubos-diagnostics $(TARGET_DIR)/etc/init.d/S99nuubos-diagnostics
	$(INSTALL) -D -m 0755 $(@D)/95-diagnostics \
		$(TARGET_DIR)/usr/lib/nuubos/lifecycle-hooks/pre-power.d/95-diagnostics
	# Replaced by the diagnostic mode (2026-10-22): gone from older targets.
	rm -f $(TARGET_DIR)/etc/init.d/S01nuubos-bootlog $(TARGET_DIR)/etc/init.d/S99nuubos-bootlog
endef

$(eval $(generic-package))
