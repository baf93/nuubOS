################################################################################
#
# nuubOS Linux integration
#
################################################################################

define NUUBOS_LINUX_GENERATE_PANEL_FIRMWARE
	rm -rf $(LINUX_DIR)/firmware/panels
	mkdir -p $(LINUX_DIR)/firmware/panels
	$(BR2_EXTERNAL_NUUBOS_PATH)/scripts/generate-panel-firmware.py \
		--presets $(BR2_EXTERNAL_NUUBOS_PATH)/board/nuubos/platform/h700/panel-firmware/presets \
		--output $(LINUX_DIR)/firmware/panels
endef

LINUX_POST_PATCH_HOOKS += NUUBOS_LINUX_GENERATE_PANEL_FIRMWARE
