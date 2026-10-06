################################################################################
#
# libretro-scummvm
#
################################################################################

# ScummVM (point-and-click adventures) libretro core, "lite" engine set
# (SCUMM, Sky, Queen, Lure, Sword1/2, Kyra, SCI, AGI, Gob, Tinsel... the
# classic 2D engines; no 3D engines, no desktop OpenGL). Part of the default
# H700 core matrix (package/libretro/README.md).
LIBRETRO_SCUMMVM_VERSION = fcbce3ae815269dacdc309092bc92ccc6d3e13bb
LIBRETRO_SCUMMVM_SITE = $(call github,libretro,scummvm,$(LIBRETRO_SCUMMVM_VERSION))
LIBRETRO_SCUMMVM_LICENSE = GPL-3.0+
LIBRETRO_SCUMMVM_LICENSE_FILES = COPYING
# zip packs the engine data and themes (make datafiles).
LIBRETRO_SCUMMVM_DEPENDENCIES = host-zip

# Dependencies the libretro backend would otherwise git-clone at build time
# (commits from backends/platform/libretro/dependencies.mk).
LIBRETRO_SCUMMVM_DEPS_COMMIT = bab7d258c451c0e7cba4b6a79f1b062c13efff38
LIBRETRO_SCUMMVM_COMMON_COMMIT = 879c8d507b0b52e77e27d759239c2b5df1e26dfd
LIBRETRO_SCUMMVM_EXTRA_DOWNLOADS = \
	https://github.com/libretro/libretro-deps/archive/$(LIBRETRO_SCUMMVM_DEPS_COMMIT).tar.gz \
	https://github.com/libretro/libretro-common/archive/$(LIBRETRO_SCUMMVM_COMMON_COMMIT).tar.gz

LIBRETRO_SCUMMVM_LIBRETRO_DIR = $(@D)/backends/platform/libretro

define LIBRETRO_SCUMMVM_EXTRACT_DEPS
	mkdir -p $(LIBRETRO_SCUMMVM_LIBRETRO_DIR)/deps/libretro-deps \
		$(LIBRETRO_SCUMMVM_LIBRETRO_DIR)/deps/libretro-common
	$(call suitable-extractor,$(LIBRETRO_SCUMMVM_DEPS_COMMIT).tar.gz) \
		$(LIBRETRO_SCUMMVM_DL_DIR)/$(LIBRETRO_SCUMMVM_DEPS_COMMIT).tar.gz | \
		$(TAR) --strip-components=1 -C $(LIBRETRO_SCUMMVM_LIBRETRO_DIR)/deps/libretro-deps $(TAR_OPTIONS) -
	$(call suitable-extractor,$(LIBRETRO_SCUMMVM_COMMON_COMMIT).tar.gz) \
		$(LIBRETRO_SCUMMVM_DL_DIR)/$(LIBRETRO_SCUMMVM_COMMON_COMMIT).tar.gz | \
		$(TAR) --strip-components=1 -C $(LIBRETRO_SCUMMVM_LIBRETRO_DIR)/deps/libretro-common $(TAR_OPTIONS) -
endef
LIBRETRO_SCUMMVM_POST_EXTRACT_HOOKS += LIBRETRO_SCUMMVM_EXTRACT_DEPS

LIBRETRO_SCUMMVM_MAKE_OPTS = \
	platform=unix LITE=1 NO_WIP=1 DEPS_PROVIDED=1 HAVE_OPENGL=0 \
	HAVE_OPENGLES2=0 USE_CURL=0 BUILD_64BIT=1 \
	AR="$(TARGET_AR) cru" RANLIB="$(TARGET_RANLIB)" LD="$(TARGET_CXX)"

define LIBRETRO_SCUMMVM_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(LIBRETRO_CORE_ENV) $(MAKE) -C $(LIBRETRO_SCUMMVM_LIBRETRO_DIR) \
		$(LIBRETRO_SCUMMVM_MAKE_OPTS)
	$(TARGET_MAKE_ENV) $(LIBRETRO_CORE_ENV) $(MAKE1) -C $(LIBRETRO_SCUMMVM_LIBRETRO_DIR) \
		$(LIBRETRO_SCUMMVM_MAKE_OPTS) datafiles
endef

# Engine data and GUI themes live in <system>/scummvm: shipped here and
# copied into /userdata/bios by nuubos-emud when missing.
define LIBRETRO_SCUMMVM_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(LIBRETRO_SCUMMVM_LIBRETRO_DIR)/scummvm_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/scummvm_libretro.so
	rm -rf $(TARGET_DIR)/usr/share/nuubos/emulation/system/scummvm
	mkdir -p $(TARGET_DIR)/usr/share/nuubos/emulation/system
	cd $(TARGET_DIR)/usr/share/nuubos/emulation/system && \
		unzip -q -o $(LIBRETRO_SCUMMVM_LIBRETRO_DIR)/scummvm.zip
endef

$(eval $(generic-package))
