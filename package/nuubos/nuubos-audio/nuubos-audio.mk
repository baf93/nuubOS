################################################################################
#
# nuubos-audio
#
################################################################################

NUUBOS_AUDIO_VERSION = 1.0
NUUBOS_AUDIO_SITE = $(BR2_EXTERNAL_NUUBOS_PATH)/package/nuubos/nuubos-audio/src
NUUBOS_AUDIO_SITE_METHOD = local
NUUBOS_AUDIO_LICENSE = MIT
NUUBOS_AUDIO_LICENSE_FILES = LICENSE
NUUBOS_AUDIO_DEPENDENCIES = alsa-lib dbus bluez-alsa

NUUBOS_AUDIO_DBUS_CFLAGS = \
	-I$(STAGING_DIR)/usr/include/dbus-1.0 \
	-I$(STAGING_DIR)/usr/lib/dbus-1.0/include

define NUUBOS_AUDIO_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		$(NUUBOS_AUDIO_DBUS_CFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror \
		$(@D)/audiod.c \
		-ldbus-1 -lasound \
		-o $(@D)/nuubos-audiod
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror \
		$(@D)/audioctl.c \
		-o $(@D)/nuubos-audioctl
endef

define NUUBOS_AUDIO_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/nuubos-audiod \
		$(TARGET_DIR)/usr/sbin/nuubos-audiod
	$(INSTALL) -D -m 0755 $(@D)/nuubos-audioctl \
		$(TARGET_DIR)/usr/bin/nuubos-audioctl
	$(INSTALL) -D -m 0755 $(@D)/S43nuubos-audiod \
		$(TARGET_DIR)/etc/init.d/S43nuubos-audiod
endef

$(eval $(generic-package))
