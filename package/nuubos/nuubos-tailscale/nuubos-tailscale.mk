################################################################################
#
# nuubos-tailscale
#
################################################################################

NUUBOS_TAILSCALE_VERSION = 0.1.0
NUUBOS_TAILSCALE_SITE = $(BR2_EXTERNAL_NUUBOS_PATH)/package/nuubos/nuubos-tailscale/src
NUUBOS_TAILSCALE_SITE_METHOD = local
NUUBOS_TAILSCALE_LICENSE = GPL-3.0+
NUUBOS_TAILSCALE_LICENSE_FILES = LICENSE
NUUBOS_TAILSCALE_DEPENDENCIES = cjson libcurl openssl zlib

define NUUBOS_TAILSCALE_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-format-truncation \
		$(@D)/tsd.c -o $(@D)/nuubos-tsd -lcjson -lcurl -lpthread
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror \
		$(@D)/tsctl.c -o $(@D)/nuubos-tsctl
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-format-truncation \
		$(@D)/tailscale-get.c -o $(@D)/nuubos-tailscale-get -lcjson -lcurl -lcrypto -lz
endef

define NUUBOS_TAILSCALE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/nuubos-tsd $(TARGET_DIR)/usr/sbin/nuubos-tsd
	$(INSTALL) -D -m 0755 $(@D)/nuubos-tsctl $(TARGET_DIR)/usr/bin/nuubos-tsctl
	$(INSTALL) -D -m 0755 $(@D)/nuubos-tailscale-get $(TARGET_DIR)/usr/lib/nuubos/nuubos-tailscale-get
	$(INSTALL) -D -m 0755 $(@D)/S49nuubos-tsd $(TARGET_DIR)/etc/init.d/S49nuubos-tsd
endef

$(eval $(generic-package))
