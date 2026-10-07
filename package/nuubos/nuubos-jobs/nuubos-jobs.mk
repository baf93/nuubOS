################################################################################
#
# nuubos-jobs
#
################################################################################

NUUBOS_JOBS_VERSION = 0.1.0
NUUBOS_JOBS_SITE = $(BR2_EXTERNAL_NUUBOS_PATH)/package/nuubos/nuubos-jobs/src
NUUBOS_JOBS_SITE_METHOD = local
NUUBOS_JOBS_LICENSE = MIT
NUUBOS_JOBS_LICENSE_FILES = LICENSE
NUUBOS_JOBS_DEPENDENCIES = nuubos-notify

define NUUBOS_JOBS_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror \
		$(@D)/jobd.c -o $(@D)/nuubos-jobd
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror \
		$(@D)/jobctl.c -o $(@D)/nuubos-jobctl
endef

define NUUBOS_JOBS_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/nuubos-jobd $(TARGET_DIR)/usr/sbin/nuubos-jobd
	$(INSTALL) -D -m 0755 $(@D)/nuubos-jobctl $(TARGET_DIR)/usr/bin/nuubos-jobctl
	$(INSTALL) -D -m 0755 $(@D)/S39nuubos-jobd $(TARGET_DIR)/etc/init.d/S39nuubos-jobd
	$(INSTALL) -d -m 0755 $(TARGET_DIR)/usr/share/nuubos/jobs
endef

$(eval $(generic-package))
