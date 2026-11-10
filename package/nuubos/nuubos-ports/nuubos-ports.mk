################################################################################
#
# nuubos-ports
#
################################################################################

NUUBOS_PORTS_VERSION = 0.1.0
NUUBOS_PORTS_SITE = $(BR2_EXTERNAL_NUUBOS_PATH)/package/nuubos/nuubos-ports/src
NUUBOS_PORTS_SITE_METHOD = local
NUUBOS_PORTS_LICENSE = GPL-3.0+
NUUBOS_PORTS_LICENSE_FILES = LICENSE
NUUBOS_PORTS_DEPENDENCIES = bubblewrap python3 sdl2 sdl2_image sdl2_ttf sdl2_mixer sdl2_gfx openal libvorbis \
	libtheora libcurl openssl

define NUUBOS_PORTS_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-format-truncation \
		$(@D)/portsd.c -o $(@D)/nuubos-portsd
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror \
		$(@D)/portsctl.c -o $(@D)/nuubos-portsctl
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-format-truncation \
		$(@D)/ports-mount.c -o $(@D)/nuubos-ports-mount
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-format-truncation \
		$(@D)/portmaster-get.c -o $(@D)/nuubos-portmaster-get -lcurl -lcrypto
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-format-truncation \
		$(@D)/ports-move.c -o $(@D)/nuubos-ports-move
endef

# Port scripts expect /roms/ports (PortMaster's default layout); USERDATA
# holds the ROM folders on nuubOS.
define NUUBOS_PORTS_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/nuubos-portsd $(TARGET_DIR)/usr/sbin/nuubos-portsd
	$(INSTALL) -D -m 0755 $(@D)/nuubos-portsctl $(TARGET_DIR)/usr/bin/nuubos-portsctl
	$(INSTALL) -D -m 0755 $(@D)/nuubos-portmaster-get \
		$(TARGET_DIR)/usr/lib/nuubos/nuubos-portmaster-get
	$(INSTALL) -D -m 0755 $(@D)/nuubos-ports-move $(TARGET_DIR)/usr/lib/nuubos/nuubos-ports-move
	$(INSTALL) -D -m 0755 $(@D)/nuubos-ports-mount $(TARGET_DIR)/usr/lib/nuubos/ports/bin/mount
	ln -sfn mount $(TARGET_DIR)/usr/lib/nuubos/ports/bin/umount
	$(INSTALL) -D -m 0644 $(@D)/portmaster/mod_nuubOS.txt \
		$(TARGET_DIR)/usr/share/nuubos/ports/mod_nuubOS.txt
	$(INSTALL) -D -m 0755 $(@D)/S49nuubos-portsd $(TARGET_DIR)/etc/init.d/S49nuubos-portsd
	$(INSTALL) -D -m 0755 $(@D)/57-ports \
		$(TARGET_DIR)/usr/lib/nuubos/lifecycle-hooks/pre-power.d/57-ports
	$(INSTALL) -D -m 0644 $(@D)/app/portmaster.app \
		$(TARGET_DIR)/usr/share/nuubos/applications/portmaster.app
	ln -sfn /userdata/roms $(TARGET_DIR)/roms
	# PortMaster's LOVE 11.5 runtime links libtheoradec.so.1 (libtheora
	# 1.1); 1.2 renamed it .so.2 and keeps every th_* symbol it uses
	# under the same libtheoradec_1.0 version.
	ln -sfn libtheoradec.so.2 $(TARGET_DIR)/usr/lib/libtheoradec.so.1
endef

$(eval $(generic-package))
