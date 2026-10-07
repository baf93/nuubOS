################################################################################
#
# nuubos-remote
#
################################################################################

NUUBOS_REMOTE_VERSION = 0.1.0
NUUBOS_REMOTE_SITE = $(BR2_EXTERNAL_NUUBOS_PATH)/package/nuubos/nuubos-remote/src
NUUBOS_REMOTE_SITE_METHOD = local
NUUBOS_REMOTE_LICENSE = MIT
NUUBOS_REMOTE_LICENSE_FILES = LICENSE

define NUUBOS_REMOTE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/nuubos-remotectl $(TARGET_DIR)/usr/sbin/nuubos-remotectl
	$(INSTALL) -D -m 0644 $(@D)/ksmbd.conf $(TARGET_DIR)/etc/nuubos/ksmbd.conf
	$(INSTALL) -D -m 0644 $(@D)/web/lighttpd.conf $(TARGET_DIR)/etc/nuubos/web/lighttpd.conf
	$(INSTALL) -D -m 0755 $(@D)/web/api.cgi $(TARGET_DIR)/usr/lib/nuubos/web/api.cgi
	$(INSTALL) -D -m 0644 $(@D)/web/www/index.html $(TARGET_DIR)/usr/share/nuubos/web/index.html
	$(INSTALL) -D -m 0644 $(@D)/web/www/app.js $(TARGET_DIR)/usr/share/nuubos/web/app.js
	$(INSTALL) -D -m 0644 $(@D)/web/www/style.css $(TARGET_DIR)/usr/share/nuubos/web/style.css
	# lighttpd's own example init script is not used: nuubos-remotectl
	# starts it only when Web administration is enabled.
	rm -f $(TARGET_DIR)/etc/init.d/S50lighttpd
endef

$(eval $(generic-package))
