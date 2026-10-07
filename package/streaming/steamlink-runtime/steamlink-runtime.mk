################################################################################
#
# steamlink-runtime
#
################################################################################

# The Valve Steam Link application (downloaded on the device, never part of
# the image) bundles Qt 5.14.1 without a Wayland platform plugin. This
# package builds that plugin from the same Qt release: qtbase 5.14.1 is
# configured like Valve's build (OpenGL ES 2, EGL, Vulkan, D-Bus, GLib) only
# as a build-time SDK, and qtwayland 5.14.1 is built against it. Only
# libQt5WaylandClient and the client plugins Steam Link needs are installed;
# at run time they load Valve's own Qt libraries.
STEAMLINK_RUNTIME_VERSION = 5.14.1
STEAMLINK_RUNTIME_SITE = https://download.qt.io/archive/qt/5.14/$(STEAMLINK_RUNTIME_VERSION)/submodules
STEAMLINK_RUNTIME_SOURCE = qtwayland-everywhere-src-$(STEAMLINK_RUNTIME_VERSION).tar.xz
STEAMLINK_RUNTIME_QTBASE_SOURCE = qtbase-everywhere-src-$(STEAMLINK_RUNTIME_VERSION).tar.xz
# Build time only (Qt's Vulkan support changes QPlatformIntegration's vtable,
# so it must match Valve's build): headers contemporary with Qt 5.14.
STEAMLINK_RUNTIME_VULKAN_VERSION = 1.2.131
STEAMLINK_RUNTIME_VULKAN_SOURCE = Vulkan-Headers-$(STEAMLINK_RUNTIME_VULKAN_VERSION).tar.gz
STEAMLINK_RUNTIME_EXTRA_DOWNLOADS = \
	$(STEAMLINK_RUNTIME_QTBASE_SOURCE) \
	https://github.com/KhronosGroup/Vulkan-Headers/archive/v$(STEAMLINK_RUNTIME_VULKAN_VERSION)/$(STEAMLINK_RUNTIME_VULKAN_SOURCE)
STEAMLINK_RUNTIME_LICENSE = LGPL-3.0 or GPL-2.0+ or GPL-3.0 (Qt Wayland)
STEAMLINK_RUNTIME_LICENSE_FILES = LICENSE.LGPL3 LICENSE.GPL2 LICENSE.GPL3
STEAMLINK_RUNTIME_DEPENDENCIES = host-pkgconf double-conversion ffmpeg libdrm \
	libepoxy libkrb5 md4c zstd dbus fontconfig freetype harfbuzz libegl \
	libgles libglib2 libpng libxkbcommon wayland zlib

STEAMLINK_RUNTIME_QT = $(@D)/qtbase
STEAMLINK_RUNTIME_SDK = $(@D)/sdk
STEAMLINK_RUNTIME_HOST_SDK = $(@D)/host-sdk
STEAMLINK_RUNTIME_PREFIX = /usr/lib/steamlink-runtime/qt5
STEAMLINK_RUNTIME_ENV = \
	PATH=$(BR_PATH) \
	PKG_CONFIG=$(PKG_CONFIG_HOST_BINARY) \
	PKG_CONFIG_SYSROOT_DIR=$(STAGING_DIR) \
	PKG_CONFIG_LIBDIR=$(STAGING_DIR)/usr/lib/pkgconfig:$(STAGING_DIR)/usr/share/pkgconfig

define STEAMLINK_RUNTIME_EXTRACT_EXTRA
	mkdir -p $(STEAMLINK_RUNTIME_QT) $(@D)/vulkan-headers
	$(call suitable-extractor,$(STEAMLINK_RUNTIME_QTBASE_SOURCE)) \
		$(STEAMLINK_RUNTIME_DL_DIR)/$(STEAMLINK_RUNTIME_QTBASE_SOURCE) | \
		$(TAR) --strip-components=1 -C $(STEAMLINK_RUNTIME_QT) $(TAR_OPTIONS) -
	$(call suitable-extractor,$(STEAMLINK_RUNTIME_VULKAN_SOURCE)) \
		$(STEAMLINK_RUNTIME_DL_DIR)/$(STEAMLINK_RUNTIME_VULKAN_SOURCE) | \
		$(TAR) --strip-components=1 -C $(@D)/vulkan-headers $(TAR_OPTIONS) -
	$(APPLY_PATCHES) $(STEAMLINK_RUNTIME_QT) $(STEAMLINK_RUNTIME_PKGDIR)/qtbase \*.patch
endef
STEAMLINK_RUNTIME_POST_EXTRACT_HOOKS += STEAMLINK_RUNTIME_EXTRACT_EXTRA

# Buildroot cross "device" for qmake (as qt5base does).
define STEAMLINK_RUNTIME_DEVICE_SPEC
	mkdir -p $(STEAMLINK_RUNTIME_QT)/mkspecs/devices/linux-buildroot-g++
	printf '%s\n' \
		'include(../common/linux_device_pre.conf)' \
		'QMAKE_CFLAGS += $(TARGET_CFLAGS) -I$(@D)/vulkan-headers/include' \
		'QMAKE_CXXFLAGS += $(TARGET_CXXFLAGS) -I$(@D)/vulkan-headers/include' \
		'QMAKE_CFLAGS_OPTIMIZE =' \
		'QMAKE_CFLAGS_OPTIMIZE_FULL =' \
		'QMAKE_CFLAGS_RELEASE =' \
		'QMAKE_CXXFLAGS_RELEASE =' \
		'QMAKE_LIBS += -lrt -lpthread -ldl' \
		'QMAKE_CFLAGS_ISYSTEM =' \
		'CONFIG += nostrip' \
		'include(../common/linux_device_post.conf)' \
		'load(qt_config)' \
		> $(STEAMLINK_RUNTIME_QT)/mkspecs/devices/linux-buildroot-g++/qmake.conf
	echo '#include "../../linux-g++/qplatformdefs.h"' \
		> $(STEAMLINK_RUNTIME_QT)/mkspecs/devices/linux-buildroot-g++/qplatformdefs.h
endef

define STEAMLINK_RUNTIME_CONFIGURE_CMDS
	$(STEAMLINK_RUNTIME_DEVICE_SPEC)
	cd $(STEAMLINK_RUNTIME_QT) && $(STEAMLINK_RUNTIME_ENV) ./configure \
		-prefix $(STEAMLINK_RUNTIME_PREFIX) \
		-extprefix $(STEAMLINK_RUNTIME_SDK) \
		-hostprefix $(STEAMLINK_RUNTIME_HOST_SDK) \
		-sysroot $(STAGING_DIR) \
		-device linux-buildroot-g++ \
		-device-option CROSS_COMPILE="$(TARGET_CROSS)" \
		-opensource -confirm-license -release -shared -no-pch -no-rpath \
		-opengl es2 -egl -feature-vulkan -no-xcb -no-eglfs -no-linuxfb \
		-no-kms -no-gbm -no-libinput -no-tslib -no-evdev -no-mtdev -no-icu \
		-glib -dbus-linked -system-zlib -system-libpng -no-libjpeg \
		-system-harfbuzz -system-freetype -fontconfig \
		-system-doubleconversion -zstd -no-openssl -no-widgets \
		-no-sql-sqlite -no-cups -nomake examples -nomake tests \
		-make libs -make tools
endef

define STEAMLINK_RUNTIME_BUILD_CMDS
	$(STEAMLINK_RUNTIME_ENV) $(MAKE) -C $(STEAMLINK_RUNTIME_QT)
	$(STEAMLINK_RUNTIME_ENV) $(MAKE) -C $(STEAMLINK_RUNTIME_QT) install
	mkdir -p $(@D)/build
	cd $(@D)/build && $(STEAMLINK_RUNTIME_ENV) $(STEAMLINK_RUNTIME_HOST_SDK)/bin/qmake $(@D) \
		-- -no-feature-wayland-server
	$(STEAMLINK_RUNTIME_ENV) $(MAKE) -C $(@D)/build
	$(STEAMLINK_RUNTIME_ENV) $(MAKE) -C $(@D)/build install INSTALL_ROOT=$(@D)/install
endef

STEAMLINK_RUNTIME_PLUGINS = \
	platforms/libqwayland-egl.so \
	wayland-graphics-integration-client/libqt-plugin-wayland-egl.so \
	wayland-shell-integration/libxdg-shell.so

define STEAMLINK_RUNTIME_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 \
		$(@D)/install$(STEAMLINK_RUNTIME_SDK)/lib/libQt5WaylandClient.so.$(STEAMLINK_RUNTIME_VERSION) \
		$(TARGET_DIR)$(STEAMLINK_RUNTIME_PREFIX)/lib/libQt5WaylandClient.so.5
	$(foreach p,$(STEAMLINK_RUNTIME_PLUGINS),
		$(INSTALL) -D -m 0755 $(@D)/install$(STEAMLINK_RUNTIME_SDK)/plugins/$(p) \
			$(TARGET_DIR)$(STEAMLINK_RUNTIME_PREFIX)/plugins/$(p)
	)
endef

$(eval $(generic-package))
