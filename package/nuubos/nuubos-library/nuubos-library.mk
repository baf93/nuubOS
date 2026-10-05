################################################################################
#
# nuubos-library
#
################################################################################

NUUBOS_LIBRARY_VERSION = 0.1.0
NUUBOS_LIBRARY_SITE = $(BR2_EXTERNAL_NUUBOS_PATH)/package/nuubos/nuubos-library/src
NUUBOS_LIBRARY_SITE_METHOD = local
NUUBOS_LIBRARY_LICENSE = MIT, CC-BY-4.0 (system icons)
NUUBOS_LIBRARY_LICENSE_FILES = LICENSE systems/ATTRIBUTION.txt systems/LICENSE-CC-BY-4.0.txt

define NUUBOS_LIBRARY_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror \
		$(@D)/libraryd.c -o $(@D)/nuubos-libraryd -lpthread
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror \
		$(@D)/libraryctl.c -o $(@D)/nuubos-libraryctl
endef

define NUUBOS_LIBRARY_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/nuubos-libraryd \
		$(TARGET_DIR)/usr/sbin/nuubos-libraryd
	$(INSTALL) -D -m 0755 $(@D)/nuubos-libraryctl \
		$(TARGET_DIR)/usr/bin/nuubos-libraryctl
	$(INSTALL) -D -m 0755 $(@D)/S47nuubos-library \
		$(TARGET_DIR)/etc/init.d/S47nuubos-library
	$(INSTALL) -D -m 0644 $(@D)/systems.conf \
		$(TARGET_DIR)/usr/share/nuubos/systems.conf
	$(INSTALL) -d -m 0755 $(TARGET_DIR)/usr/share/nuubos/applications
	$(INSTALL) -d -m 0755 $(TARGET_DIR)/usr/share/nuubos/systems
	$(INSTALL) -m 0644 $(@D)/systems/*.png $(@D)/systems/ATTRIBUTION.txt \
		$(@D)/systems/LICENSE-CC-BY-4.0.txt $(TARGET_DIR)/usr/share/nuubos/systems/
endef

$(eval $(generic-package))
