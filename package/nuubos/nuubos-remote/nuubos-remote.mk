################################################################################
#
# nuubos-remote
#
################################################################################

NUUBOS_REMOTE_VERSION = 0.2.0
NUUBOS_REMOTE_SITE = $(BR2_EXTERNAL_NUUBOS_PATH)/package/nuubos/nuubos-remote/src
NUUBOS_REMOTE_SITE_METHOD = local
NUUBOS_REMOTE_LICENSE = GPL-3.0+, CC-BY-4.0 (icons)
NUUBOS_REMOTE_LICENSE_FILES = LICENSE web/www/icons/LICENSE.txt
NUUBOS_REMOTE_DEPENDENCIES = openssl

# The per-device TLS certificate helper (libcrypto).
define NUUBOS_REMOTE_BUILD_CERT_HELPER
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -Wall -Wextra -o $(@D)/nuubos-web-cert \
		$(@D)/nuubos-web-cert.c -lcrypto
endef
NUUBOS_REMOTE_POST_BUILD_HOOKS += NUUBOS_REMOTE_BUILD_CERT_HELPER

define NUUBOS_REMOTE_INSTALL_FILES
	mv -f $(TARGET_DIR)/usr/bin/nuubos-admind $(TARGET_DIR)/usr/sbin/nuubos-admind
	$(INSTALL) -D -m 0755 $(@D)/nuubos-web-cert $(TARGET_DIR)/usr/sbin/nuubos-web-cert
	$(INSTALL) -D -m 0755 $(@D)/nuubos-remotectl $(TARGET_DIR)/usr/sbin/nuubos-remotectl
	$(INSTALL) -D -m 0644 $(@D)/ksmbd.conf $(TARGET_DIR)/etc/nuubos/ksmbd.conf
	$(INSTALL) -D -m 0644 $(@D)/web/lighttpd.conf $(TARGET_DIR)/etc/nuubos/web/lighttpd.conf
	# The 0.1 CGI interface and its static files are replaced as a whole.
	rm -rf $(TARGET_DIR)/usr/share/nuubos/web $(TARGET_DIR)/usr/lib/nuubos/web
	cd $(@D)/web/www && find . -type f -exec $(INSTALL) -D -m 0644 {} $(TARGET_DIR)/usr/share/nuubos/web/{} \;
	# lighttpd's own example init script is not used: nuubos-remotectl
	# starts it only when Web administration is enabled.
	rm -f $(TARGET_DIR)/etc/init.d/S50lighttpd
endef
NUUBOS_REMOTE_POST_INSTALL_TARGET_HOOKS += NUUBOS_REMOTE_INSTALL_FILES

NUUBOS_REMOTE_DEPENDENCIES += host-python3
define NUUBOS_REMOTE_INSTALL_NOTICES
	$(call NUUBOS_CARGO_NOTICES,nuubos-remote,$(@D))
endef
NUUBOS_REMOTE_POST_INSTALL_TARGET_HOOKS += NUUBOS_REMOTE_INSTALL_NOTICES

$(eval $(cargo-package))
