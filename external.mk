include $(sort $(wildcard $(BR2_EXTERNAL_NUUBOS_PATH)/package/nuubos/*/*.mk))
include $(BR2_EXTERNAL_NUUBOS_PATH)/package/libretro/libretro.mk
include $(sort $(wildcard $(BR2_EXTERNAL_NUUBOS_PATH)/package/libretro/*/*.mk))
include $(sort $(wildcard $(BR2_EXTERNAL_NUUBOS_PATH)/package/streaming/*/*.mk))

# FFmpeg is built for the VPU decoders only (V4L2 request API, patches in
# patches/packages/ffmpeg): it needs libudev, which ffmpeg.mk does not
# order before it.
$(FFMPEG_TARGET_CONFIGURE): | udev

# PipeWire would build its FFmpeg SPA plugin and pw-cat against it; nuubOS
# uses neither and FFmpeg has no libavformat here. meson keeps the last -D.
PIPEWIRE_CONF_OPTS += -Dffmpeg=disabled -Dpw-cat-ffmpeg=disabled
