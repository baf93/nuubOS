################################################################################
#
# nuubos-recovery
#
################################################################################

NUUBOS_RECOVERY_VERSION = 0.1.0
NUUBOS_RECOVERY_SITE = $(BR2_EXTERNAL_NUUBOS_PATH)/package/nuubos/nuubos-recovery/src
NUUBOS_RECOVERY_SITE_METHOD = local
NUUBOS_RECOVERY_LICENSE = MIT
NUUBOS_RECOVERY_LICENSE_FILES = LICENSE

define NUUBOS_RECOVERY_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/nuubos-diag $(TARGET_DIR)/usr/bin/nuubos-diag
	$(INSTALL) -D -m 0755 $(@D)/nuubos-recoveryctl $(TARGET_DIR)/usr/sbin/nuubos-recoveryctl
	$(INSTALL) -D -m 0755 $(@D)/nuubos-ui-supervisor $(TARGET_DIR)/usr/bin/nuubos-ui-supervisor
	$(INSTALL) -D -m 0644 $(@D)/support-bundle.job $(TARGET_DIR)/usr/share/nuubos/jobs/support-bundle.job
endef

$(eval $(generic-package))
