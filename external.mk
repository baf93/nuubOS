include $(sort $(wildcard $(BR2_EXTERNAL_NUUBOS_PATH)/package/nuubos/*/*.mk))
include $(BR2_EXTERNAL_NUUBOS_PATH)/package/libretro/libretro.mk
include $(sort $(wildcard $(BR2_EXTERNAL_NUUBOS_PATH)/package/libretro/*/*.mk))
include $(sort $(wildcard $(BR2_EXTERNAL_NUUBOS_PATH)/package/streaming/*/*.mk))
include $(sort $(wildcard $(BR2_EXTERNAL_NUUBOS_PATH)/package/thirdparty/*/*.mk))

# Third-party notices (LICENSING.md): the crates linked into a cargo package
# or the Go modules built into a golang package, with their license texts, in
# /usr/share/licenses/<pkg>/THIRD-PARTY-NOTICES.txt. Buildroot's legal-info
# archives the vendored sources but does not list their licenses.
# $(1): package name, $(2): its build directory.
define NUUBOS_CARGO_NOTICES
	cd $(2) && $(TARGET_MAKE_ENV) $(PKG_CARGO_ENV) $(HOST_DIR)/bin/cargo metadata --offline --locked \
		--format-version 1 --filter-platform $(RUSTC_TARGET_NAME) > $(2)/.nuubos-cargo-metadata.json
	mkdir -p $(TARGET_DIR)/usr/share/licenses/$(1)
	$(HOST_DIR)/bin/python3 $(BR2_EXTERNAL_NUUBOS_PATH)/scripts/third-party-notices.py cargo \
		$(2)/.nuubos-cargo-metadata.json $(1) $(TARGET_DIR)/usr/share/licenses/$(1)/THIRD-PARTY-NOTICES.txt
endef
define NUUBOS_GO_NOTICES
	mkdir -p $(TARGET_DIR)/usr/share/licenses/$(1)
	$(HOST_DIR)/bin/python3 $(BR2_EXTERNAL_NUUBOS_PATH)/scripts/third-party-notices.py go \
		$(2)/vendor $(1) $(TARGET_DIR)/usr/share/licenses/$(1)/THIRD-PARTY-NOTICES.txt
endef

# FFmpeg is built for the VPU decoders only (V4L2 request API, patches in
# patches/packages/ffmpeg): it needs libudev, which ffmpeg.mk does not
# order before it.
$(FFMPEG_TARGET_CONFIGURE): | udev

# PipeWire would build its FFmpeg SPA plugin and pw-cat against it; nuubOS
# uses neither and FFmpeg has no libavformat here. meson keeps the last -D.
PIPEWIRE_CONF_OPTS += -Dffmpeg=disabled -Dpw-cat-ffmpeg=disabled

# libvpx (screen casting, VP8 encoder): Buildroot configures
# --target=generic-gnu, i.e. plain C without NEON, several times slower on
# the Cortex-A53. libvpx keeps the first --target it sees, but a preset
# "toolchain" variable wins over it.
LIBVPX_CONF_ENV += toolchain=arm64-linux-gcc
LIBVPX_CONF_OPTS += --enable-realtime-only

# libplacebo (mpv, EPIC-028): no Vulkan/GLSL compiler on H700; mpv presents
# Cedrus frames through dmabuf-wayland and falls back to GLES.
LIBPLACEBO_CONF_OPTS += -Dvulkan=disabled -Dd3d11=disabled -Dshaderc=disabled -Dglslang=disabled -Dlcms=disabled -Ddemos=false -Dtests=false -Dxxhash=disabled

# Web Mode (EPIC-030): Cog's remote control lives on the system bus
# (nuubos-webd runs cogctl as root); an empty owner would install an
# invalid D-Bus policy (user="").
COG_CONF_OPTS += -Dcog_dbus_system_owner=root

# License metadata for legal-info (LICENSING.md). DejaVu: Buildroot leaves it
# "unknown". linux-firmware: nuubOS ships only the Realtek rtw88 Wi-Fi and
# rtl_bt Bluetooth firmware, binary-only but redistributable (WHENCE).
DEJAVU_LICENSE = Bitstream-Vera, Public-Domain (DejaVu changes), Arev fonts license
LINUX_FIRMWARE_LICENSE = Redistributable binary firmware (LICENCE.rtlwifi_firmware.txt)

# Mesa (Panfrost, the GL path of every core and of the UI) at -O3:
# Buildroot's -O2 (BR2_OPTIMIZE_2) otherwise overrides meson's buildtype
# (docs/dev/perf-power.md 5.1). The later -O3 wins.
# LTO was dropped for Mesa 26.2.7: upstream now rejects it at configure time
# unless -Dallow-broken-lto=true, because it causes random, unreproducible
# bugs. On 25.0.6 its measured gain was marginal (85/87 -> 84/84 % on the
# busiest thread); with the Panfrost faults fixed, one less variable is worth
# more. Re-add -Dallow-broken-lto=true only as a measured experiment.
MESA3D_CFLAGS += -O3
MESA3D_CXXFLAGS += -O3
