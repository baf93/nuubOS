################################################################################
#
# libretro-tic80
#
################################################################################

# TIC-80 fantasy console (libretro core only). Part of the default H700 core
# matrix (package/libretro/README.md).
LIBRETRO_TIC80_VERSION = 14fd99edc29962778bc88d81773288ffc6e8aa53
LIBRETRO_TIC80_SITE = https://github.com/libretro/TIC-80.git
LIBRETRO_TIC80_SITE_METHOD = git
LIBRETRO_TIC80_GIT_SUBMODULES = YES
LIBRETRO_TIC80_LICENSE = MIT
LIBRETRO_TIC80_LICENSE_FILES = LICENSE
LIBRETRO_TIC80_SUPPORTS_IN_SOURCE_BUILD = NO
# The libretro repository carries upstream TIC-80 as the core submodule.
LIBRETRO_TIC80_SUBDIR = core
# Same feature set as the libretro buildbot, without the languages that do
# not cross-build reliably (Ruby, YueScript, Scheme).
LIBRETRO_TIC80_CONF_OPTS = \
	-DBUILD_LIBRETRO=ON -D__LIBRETRO__=ON -DBUILD_PLAYER=OFF -DBUILD_PRO=OFF \
	-DBUILD_SDL=OFF -DBUILD_TOOLS=OFF -DBUILD_TOUCH_INPUT=OFF -DBUILD_STATIC=ON \
	-DBUILD_DEMO_CARTS=OFF -DBUILD_WITH_ALL=OFF -DBUILD_WITH_LUA=ON \
	-DBUILD_WITH_MOON=ON -DBUILD_WITH_FENNEL=ON -DBUILD_WITH_WREN=ON \
	-DBUILD_WITH_WASM=ON -DBUILD_WITH_SQUIRREL=ON -DBUILD_WITH_PYTHON=ON \
	-DBUILD_WITH_JS=ON -DBUILD_WITH_JANET=ON -DBUILD_WITH_RUBY=OFF \
	-DBUILD_WITH_YUE=OFF -DBUILD_WITH_SCHEME=OFF \
	-DCMAKE_C_FLAGS="$(TARGET_CFLAGS) $(LIBRETRO_CORE_CFLAGS)"

define LIBRETRO_TIC80_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $$(find $(LIBRETRO_TIC80_BUILDDIR) -name tic80_libretro.so | head -1) \
		$(TARGET_DIR)/usr/lib/libretro/tic80_libretro.so
endef

$(eval $(cmake-package))
