################################################################################
#
# retroarch
#
################################################################################

RETROARCH_VERSION = 1.22.2
RETROARCH_SITE = $(call github,libretro,RetroArch,v$(RETROARCH_VERSION))
RETROARCH_LICENSE = GPL-3.0+
RETROARCH_LICENSE_FILES = COPYING
RETROARCH_DEPENDENCIES = \
	host-pkgconf host-wayland alsa-lib freetype libegl libgles \
	libxkbcommon udev wayland wayland-protocols zlib

# Only what nuubOS uses: Wayland/EGL/GLES on Panfrost, udev joypads (the
# nuubOS virtual gamepads), ALSA (PipeWire's ALSA plugin) and native
# PipeWire audio, stdin commands from nuubos-emud. Networking stays for the
# Online Updater (cores), RetroAchievements and netplay; the UDP network
# command interface is never enabled at runtime.
RETROARCH_CONF_OPTS = \
	--prefix=/usr \
	--disable-x11 --disable-xrandr --disable-xinerama --disable-xscrnsaver \
	--disable-xi2 --disable-xvideo --disable-xshm \
	--enable-wayland --disable-libdecor \
	--enable-egl --enable-opengles --enable-opengles3 --enable-opengles3_1 \
	--disable-opengl1 --disable-vulkan --disable-kms --disable-vg \
	--disable-cg --disable-slang --disable-glslang --disable-spirv_cross \
	--disable-sdl --disable-sdl2 --disable-qt --disable-ffmpeg --disable-mpv \
	--enable-udev --disable-libusb --disable-parport --disable-blissbox \
	--enable-alsa --disable-tinyalsa --disable-pulse --disable-jack \
	--disable-oss --disable-rsound --disable-roar --disable-al \
	--disable-audioio \
	--enable-freetype --enable-zlib --disable-builtinzlib \
	--enable-threads --enable-networking --disable-discord \
	--disable-v4l2 --disable-caca --disable-sixel --disable-videoprocessor \
	--disable-videocore --disable-cdrom --disable-systemd --disable-dbus \
	--disable-update_assets

ifeq ($(BR2_PACKAGE_PIPEWIRE),y)
RETROARCH_DEPENDENCIES += pipewire
RETROARCH_CONF_OPTS += --enable-pipewire
else
RETROARCH_CONF_OPTS += --disable-pipewire
endif

define RETROARCH_CONFIGURE_CMDS
	(cd $(@D) && rm -f config.cache && \
		$(TARGET_MAKE_ENV) $(TARGET_CONFIGURE_OPTS) \
		CROSS_COMPILE="$(TARGET_CROSS)" \
		PKG_CONF_PATH="$(PKG_CONFIG_HOST_BINARY)" \
		./configure $(RETROARCH_CONF_OPTS))
endef

define RETROARCH_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(TARGET_CONFIGURE_OPTS) $(MAKE) -C $(@D) \
		CXX="$(TARGET_CXX)" LD="$(TARGET_CXX)"
endef

define RETROARCH_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/retroarch $(TARGET_DIR)/usr/bin/retroarch
endef

$(eval $(generic-package))
