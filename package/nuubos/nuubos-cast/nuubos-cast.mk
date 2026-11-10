################################################################################
#
# nuubos-cast
#
################################################################################

NUUBOS_CAST_VERSION = 0.1.0
NUUBOS_CAST_SITE = $(BR2_EXTERNAL_NUUBOS_PATH)/package/nuubos/nuubos-cast/src
NUUBOS_CAST_SITE_METHOD = local
NUUBOS_CAST_LICENSE = GPL-3.0+, BSD-2-Clause (OpenH264 API headers)
NUUBOS_CAST_LICENSE_FILES = LICENSE third_party/openh264/LICENSE
NUUBOS_CAST_DEPENDENCIES = nuubos-notify cjson libcurl openssl libvpx opus ffmpeg pipewire \
	wayland host-wayland

define NUUBOS_CAST_BUILD_CMDS
	$(HOST_DIR)/bin/wayland-scanner client-header \
		$(@D)/protocols/wlr-screencopy-unstable-v1.xml \
		$(@D)/session/wlr-screencopy-unstable-v1-client-protocol.h
	$(HOST_DIR)/bin/wayland-scanner private-code \
		$(@D)/protocols/wlr-screencopy-unstable-v1.xml \
		$(@D)/wlr-screencopy-unstable-v1-protocol.c
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-format-truncation \
		-I$(@D) -I$(@D)/session -I$(STAGING_DIR)/usr/include/pipewire-0.3 \
		-I$(STAGING_DIR)/usr/include/spa-0.2 \
		$(wildcard $(@D)/session/*.c) $(@D)/wlr-screencopy-unstable-v1-protocol.c \
		-o $(@D)/nuubos-cast-session \
		-lwayland-client -lpipewire-0.3 -lvpx -lopus -lavformat -lavcodec -lswscale -lavutil \
		-lcjson -lssl -lcrypto -ldl -lpthread -lm
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-format-truncation \
		$(@D)/castd.c -o $(@D)/nuubos-castd
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror \
		$(@D)/castctl.c -o $(@D)/nuubos-castctl
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-format-truncation \
		$(@D)/openh264-get.c -o $(@D)/nuubos-openh264-get -lcurl -lcrypto
endef

define NUUBOS_CAST_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/nuubos-castd $(TARGET_DIR)/usr/sbin/nuubos-castd
	$(INSTALL) -D -m 0755 $(@D)/nuubos-castctl $(TARGET_DIR)/usr/bin/nuubos-castctl
	$(INSTALL) -D -m 0755 $(@D)/nuubos-cast-session $(TARGET_DIR)/usr/lib/nuubos/nuubos-cast-session
	$(INSTALL) -D -m 0755 $(@D)/nuubos-openh264-get $(TARGET_DIR)/usr/lib/nuubos/nuubos-openh264-get
	$(INSTALL) -D -m 0755 $(@D)/S49nuubos-castd $(TARGET_DIR)/etc/init.d/S49nuubos-castd
	$(INSTALL) -D -m 0755 $(@D)/58-cast \
		$(TARGET_DIR)/usr/lib/nuubos/lifecycle-hooks/pre-power.d/58-cast
endef

$(eval $(generic-package))
