################################################################################
#
# nuubui-home
#
################################################################################

NUUBUI_HOME_VERSION = 0.1.0
NUUBUI_HOME_SITE = $(BR2_EXTERNAL_NUUBOS_PATH)/package/nuubos/nuubui-home/src
NUUBUI_HOME_SITE_METHOD = local
NUUBUI_HOME_LICENSE = MIT, CC-BY-4.0
NUUBUI_HOME_LICENSE_FILES = LICENSE ui/assets/icons/LICENSE.txt ui/assets/icons/ATTRIBUTION.txt

$(eval $(cargo-package))
