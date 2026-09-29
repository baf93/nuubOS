################################################################################
# nuubos-sftp-server
################################################################################

NUUBOS_SFTP_SERVER_VERSION = 0.1.0
NUUBOS_SFTP_SERVER_SITE = $(BR2_EXTERNAL_NUUBOS_PATH)/package/nuubos/nuubos-sftp-server/src
NUUBOS_SFTP_SERVER_SITE_METHOD = local
NUUBOS_SFTP_SERVER_LICENSE = MIT
NUUBOS_SFTP_SERVER_LICENSE_FILES = LICENSE
NUUBOS_SFTP_SERVER_DEPENDENCIES = openssh

# OpenSSH's Buildroot package builds the subsystem helper even when its
# client/server install options are disabled. Install only that helper so
# Dropbear remains the sole SSH daemon on nuubOS.
define NUUBOS_SFTP_SERVER_BUILD_CMDS
	test -x $(OPENSSH_DIR)/sftp-server
endef

define NUUBOS_SFTP_SERVER_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(OPENSSH_DIR)/sftp-server $(TARGET_DIR)/usr/libexec/sftp-server
	$(RM) -rf $(TARGET_DIR)/etc/ssh
endef

$(eval $(generic-package))
