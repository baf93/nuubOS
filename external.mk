include $(sort $(wildcard $(BR2_EXTERNAL_NUUBOS_PATH)/package/nuubos/*/*.mk))
include $(BR2_EXTERNAL_NUUBOS_PATH)/package/libretro/libretro.mk
include $(sort $(wildcard $(BR2_EXTERNAL_NUUBOS_PATH)/package/libretro/*/*.mk))
include $(sort $(wildcard $(BR2_EXTERNAL_NUUBOS_PATH)/package/streaming/*/*.mk))
include $(sort $(wildcard $(BR2_EXTERNAL_NUUBOS_PATH)/package/thirdparty/*/*.mk))

# FFmpeg is built for the VPU decoders only (V4L2 request API, patches in
# patches/packages/ffmpeg): it needs libudev, which ffmpeg.mk does not
# order before it.
$(FFMPEG_TARGET_CONFIGURE): | udev

# PipeWire would build its FFmpeg SPA plugin and pw-cat against it; nuubOS
# uses neither and FFmpeg has no libavformat here. meson keeps the last -D.
PIPEWIRE_CONF_OPTS += -Dffmpeg=disabled -Dpw-cat-ffmpeg=disabled

# libplacebo (mpv, EPIC-028): no Vulkan/GLSL compiler on H700; mpv presents
# Cedrus frames through dmabuf-wayland and falls back to GLES.
LIBPLACEBO_CONF_OPTS += -Dvulkan=disabled -Dd3d11=disabled -Dshaderc=disabled -Dglslang=disabled -Dlcms=disabled -Ddemos=false -Dtests=false -Dxxhash=disabled

# Web Mode (EPIC-030): Cog's remote control lives on the system bus
# (nuubos-webd runs cogctl as root); an empty owner would install an
# invalid D-Bus policy (user="").
COG_CONF_OPTS += -Dcog_dbus_system_owner=root
