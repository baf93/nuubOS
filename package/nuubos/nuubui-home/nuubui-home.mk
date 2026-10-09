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

# i-slint-core with the nuubOS patches (patches/packages/i-slint-core), used
# through [patch.crates-io] in Cargo.toml: the pristine crate comes from the
# offline cargo cache.
NUUBUI_HOME_SLINT_CORE_CRATE = $(firstword $(wildcard $(DL_DIR)/br-cargo-home/registry/cache/*/i-slint-core-1.18.1.crate))
define NUUBUI_HOME_VENDOR_SLINT_CORE
	test -f "$(NUUBUI_HOME_SLINT_CORE_CRATE)"
	rm -rf $(@D)/vendor/i-slint-core
	mkdir -p $(@D)/vendor/i-slint-core
	tar -xzf $(NUUBUI_HOME_SLINT_CORE_CRATE) -C $(@D)/vendor/i-slint-core --strip-components=1
	$(APPLY_PATCHES) $(@D)/vendor/i-slint-core $(BR2_EXTERNAL_NUUBOS_PATH)/patches/packages/i-slint-core \*.patch
endef
NUUBUI_HOME_POST_RSYNC_HOOKS += NUUBUI_HOME_VENDOR_SLINT_CORE

$(eval $(cargo-package))
