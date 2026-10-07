################################################################################
#
# nuubos-themes
#
################################################################################

NUUBOS_THEMES_VERSION = 0.1.0
NUUBOS_THEMES_SITE = $(BR2_EXTERNAL_NUUBOS_PATH)/package/nuubos/nuubos-themes/src
NUUBOS_THEMES_SITE_METHOD = local
NUUBOS_THEMES_LICENSE = MIT
NUUBOS_THEMES_LICENSE_FILES = LICENSE

define NUUBOS_THEMES_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/nuubos-themectl $(TARGET_DIR)/usr/bin/nuubos-themectl
	for t in nuubos ember mint graphite; do \
		$(INSTALL) -D -m 0644 $(@D)/themes/$$t/theme.conf \
			$(TARGET_DIR)/usr/share/nuubos/themes/$$t/theme.conf || exit 1; \
	done
endef

$(eval $(generic-package))
