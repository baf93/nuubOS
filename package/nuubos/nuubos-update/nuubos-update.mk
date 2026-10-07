################################################################################
#
# nuubos-update
#
################################################################################

NUUBOS_UPDATE_VERSION = 0.1.0
NUUBOS_UPDATE_SITE = $(BR2_EXTERNAL_NUUBOS_PATH)/package/nuubos/nuubos-update/src
NUUBOS_UPDATE_SITE_METHOD = local
NUUBOS_UPDATE_LICENSE = MIT
NUUBOS_UPDATE_LICENSE_FILES = LICENSE
NUUBOS_UPDATE_DEPENDENCIES = libcurl libsodium

# Public half of the nuubOS release key (64 hex characters). The private
# key never enters the repository; without this file updates are
# reported unavailable.
NUUBOS_UPDATE_PUBKEY = $(shell tr -d ' \n' < $(BR2_EXTERNAL_NUUBOS_PATH)/board/nuubos/common/ota/update-key.pub 2>/dev/null)

define NUUBOS_UPDATE_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-format-truncation \
		-DUPDATE_PUBKEY_HEX='"$(NUUBOS_UPDATE_PUBKEY)"' \
		$(@D)/updatectl.c -o $(@D)/nuubos-updatectl -lcurl -lsodium
endef

define NUUBOS_UPDATE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/nuubos-updatectl $(TARGET_DIR)/usr/bin/nuubos-updatectl
	$(INSTALL) -D -m 0755 $(@D)/nuubos-update-apply $(TARGET_DIR)/usr/sbin/nuubos-update-apply
	$(INSTALL) -D -m 0755 $(@D)/90-update \
		$(TARGET_DIR)/usr/lib/nuubos/lifecycle-hooks/pre-power.d/90-update
	$(INSTALL) -D -m 0644 $(@D)/update-download.job \
		$(TARGET_DIR)/usr/share/nuubos/jobs/update-download.job
endef

$(eval $(generic-package))
