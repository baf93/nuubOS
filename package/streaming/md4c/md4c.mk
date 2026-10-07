################################################################################
#
# md4c
#
################################################################################

MD4C_VERSION = 0.5.2
MD4C_SITE = https://github.com/mity/md4c/archive/release-$(MD4C_VERSION)
MD4C_SOURCE = md4c-$(MD4C_VERSION).tar.gz
MD4C_LICENSE = MIT
MD4C_LICENSE_FILES = LICENSE.md
MD4C_INSTALL_STAGING = YES
MD4C_CONF_OPTS = -DBUILD_MD2HTML_EXECUTABLE=OFF

$(eval $(cmake-package))
