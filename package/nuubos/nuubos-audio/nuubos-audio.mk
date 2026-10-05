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
NUUBOS_AUDIO_DEPENDENCIES = alsa-lib alsa-utils dbus mpg123 pipewire wireplumber nuubos-notify

NUUBOS_AUDIO_DBUS_CFLAGS = \
	-I$(STAGING_DIR)/usr/include/dbus-1.0 \
	-I$(STAGING_DIR)/usr/lib/dbus-1.0/include

define NUUBOS_AUDIO_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		$(NUUBOS_AUDIO_DBUS_CFLAGS) \
		-isystem $(STAGING_DIR)/usr/include/pipewire-0.3 \
		-isystem $(STAGING_DIR)/usr/include/spa-0.2 -D_REENTRANT \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror \
		$(@D)/audiod.c \
		-ldbus-1 -lasound -lpipewire-0.3 \
		-o $(@D)/nuubos-audiod
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror \
		$(@D)/audioctl.c \
		-o $(@D)/nuubos-audioctl
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror \
		$(@D)/audio-rate-probe.c \
		-lasound \
		-o $(@D)/nuubos-audio-rate-probe
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror \
		$(@D)/audio-wav-player.c \
		-lasound \
		-o $(@D)/nuubos-audio-wav-player
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror \
		$(@D)/audio-mp3-player.c \
		-lmpg123 -lasound \
		-o $(@D)/nuubos-audio-mp3-player
endef

define NUUBOS_AUDIO_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/nuubos-audiod \
		$(TARGET_DIR)/usr/sbin/nuubos-audiod
	$(INSTALL) -D -m 0755 $(@D)/nuubos-audioctl \
		$(TARGET_DIR)/usr/bin/nuubos-audioctl
	$(INSTALL) -D -m 0755 $(@D)/S43nuubos-audiod \
		$(TARGET_DIR)/etc/init.d/S43nuubos-audiod
	$(INSTALL) -D -m 0755 $(@D)/nuubos-audio-rate-probe \
		$(TARGET_DIR)/usr/bin/nuubos-audio-rate-probe
	$(INSTALL) -D -m 0644 $(@D)/org.nuubOS.Audio.conf \
		$(TARGET_DIR)/usr/share/dbus-1/system.d/org.nuubOS.Audio.conf
	$(INSTALL) -D -m 0644 $(@D)/92-nuubos-wireplumber-bluez.conf \
		$(TARGET_DIR)/etc/dbus-1/system.d/92-nuubos-wireplumber-bluez.conf
	$(INSTALL) -D -m 0755 $(@D)/nuubos-pipewire-candidate \
		$(TARGET_DIR)/usr/bin/nuubos-pipewire-candidate
	$(INSTALL) -D -m 0755 $(@D)/nuubos-audio-test-tone \
		$(TARGET_DIR)/usr/bin/nuubos-audio-test-tone
	$(INSTALL) -D -m 0644 $(@D)/assets/nuubos-test-jingle.wav \
		$(TARGET_DIR)/usr/share/nuubos/audio/nuubos-test-jingle.wav
	$(INSTALL) -D -m 0755 $(@D)/nuubos-audio-wav-player \
		$(TARGET_DIR)/usr/bin/nuubos-audio-wav-player
	$(INSTALL) -D -m 0755 $(@D)/nuubos-audio-mp3-player \
		$(TARGET_DIR)/usr/bin/nuubos-audio-mp3-player
	$(INSTALL) -D -m 0644 $(@D)/assets/system/boot.wav \
		$(TARGET_DIR)/usr/share/nuubos/audio/system/boot.wav
	$(INSTALL) -D -m 0644 $(@D)/assets/system/poweroff.wav \
		$(TARGET_DIR)/usr/share/nuubos/audio/system/poweroff.wav
	$(INSTALL) -D -m 0644 $(@D)/assets/system/restart.wav \
		$(TARGET_DIR)/usr/share/nuubos/audio/system/restart.wav
	$(INSTALL) -D -m 0644 $(@D)/assets/system/select.wav \
		$(TARGET_DIR)/usr/share/nuubos/audio/system/select.wav
	$(INSTALL) -D -m 0644 $(@D)/assets/system/back.wav \
		$(TARGET_DIR)/usr/share/nuubos/audio/system/back.wav
	$(INSTALL) -D -m 0644 $(@D)/assets/system/navigation.wav \
		$(TARGET_DIR)/usr/share/nuubos/audio/system/navigation.wav
	$(INSTALL) -D -m 0644 $(@D)/assets/system/quick-settings.wav \
		$(TARGET_DIR)/usr/share/nuubos/audio/system/quick-settings.wav
	$(INSTALL) -D -m 0644 $(@D)/nuubos-alsa.conf \
		$(TARGET_DIR)/usr/share/nuubos/audio/alsa.conf
	$(INSTALL) -D -m 0755 $(@D)/nuubos-pipewire-runtime \
		$(TARGET_DIR)/usr/bin/nuubos-pipewire-runtime
	$(INSTALL) -D -m 0755 $(@D)/nuubos-pw-route \
		$(TARGET_DIR)/usr/bin/nuubos-pw-route
	$(INSTALL) -D -m 0755 $(@D)/nuubos-pw-app-volume \
		$(TARGET_DIR)/usr/bin/nuubos-pw-app-volume
	$(INSTALL) -D -m 0755 $(@D)/S41nuubos-pipewire \
		$(TARGET_DIR)/etc/init.d/S41nuubos-pipewire
	$(INSTALL) -D -m 0644 $(@D)/90-nuubos-wireplumber.conf \
		$(TARGET_DIR)/etc/wireplumber/wireplumber.conf.d/90-nuubos.conf
	$(INSTALL) -D -m 0644 $(@D)/99-nuubos-pipewire-default.conf \
		$(TARGET_DIR)/etc/alsa/conf.d/99-nuubos-pipewire-default.conf
	$(INSTALL) -D -m 0644 $(@D)/90-nuubos-pipewire-client.conf \
		$(TARGET_DIR)/etc/pipewire/client.conf.d/90-nuubos-remote.conf
	$(INSTALL) -D -m 0644 $(@D)/91-nuubos-analog-sink.conf \
		$(TARGET_DIR)/etc/pipewire/pipewire.conf.d/91-nuubos-analog-sink.conf
	$(INSTALL) -D -m 0644 $(@D)/93-nuubos-stream-policy.conf \
		$(TARGET_DIR)/etc/wireplumber/wireplumber.conf.d/93-nuubos-stream-policy.conf
	$(INSTALL) -D -m 0644 $(@D)/nuubos-asound.conf \
		$(TARGET_DIR)/etc/asound.conf
endef

$(eval $(generic-package))
