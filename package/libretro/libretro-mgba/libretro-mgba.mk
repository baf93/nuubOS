################################################################################
#
# libretro-mgba
#
################################################################################

# mGBA (Game Boy Advance). Pinned upstream commit; part of the
# default H700 core matrix (package/libretro/README.md). The libretro core is a
# CMake target of the mGBA tree; only the core is built, without optional
# dependencies, frontends, OpenGL renderers or scripting.
LIBRETRO_MGBA_VERSION = 7a12d6d4b9acb14c0ae62c9166b6a2f3d08007f6
LIBRETRO_MGBA_SITE = $(call github,libretro,mgba,$(LIBRETRO_MGBA_VERSION))
LIBRETRO_MGBA_LICENSE = MPL-2.0
LIBRETRO_MGBA_LICENSE_FILES = LICENSE
LIBRETRO_MGBA_SUPPORTS_IN_SOURCE_BUILD = NO
LIBRETRO_MGBA_CONF_OPTS = \
	-DBUILD_LIBRETRO=ON -DSKIP_LIBRARY=ON -DDISABLE_DEPS=ON \
	-DBUILD_QT=OFF -DBUILD_SDL=OFF -DBUILD_GL=OFF -DBUILD_GLES2=OFF \
	-DBUILD_GLES3=OFF -DUSE_EPOXY=OFF -DUSE_DISCORD_RPC=OFF \
	-DENABLE_SCRIPTING=OFF -DUSE_FFMPEG=OFF -DUSE_EDITLINE=OFF \
	-DBUILD_SHARED=OFF -DBUILD_STATIC=OFF -DM_CORE_GB=ON -DM_CORE_GBA=ON

define LIBRETRO_MGBA_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(LIBRETRO_MGBA_BUILDDIR)/mgba_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/mgba_libretro.so
endef

$(eval $(cmake-package))
