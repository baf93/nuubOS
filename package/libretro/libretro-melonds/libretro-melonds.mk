################################################################################
#
# libretro-melonds
#
################################################################################

# melonDS (Nintendo DS), aarch64 JIT. Pinned upstream commit; part of the
# default H700 core matrix (package/libretro/README.md).
LIBRETRO_MELONDS_VERSION = 66b5d2634cd0a79030562811e6e05f5532f800ba
LIBRETRO_MELONDS_SITE = $(call github,libretro,melonDS,$(LIBRETRO_MELONDS_VERSION))
LIBRETRO_MELONDS_LICENSE = GPL-3.0
LIBRETRO_MELONDS_LICENSE_FILES = LICENSE
# core-info file to derive melonds_nuubos_libretro.info from
LIBRETRO_MELONDS_DEPENDENCIES = libretro-core-info

# Bundled as melonDS-nuubOS (melonds_nuubos): patches 0001-0027 are nuubOS
# H700 performance work (README "Patched cores"); upstream melonDS remains a
# download.
# Link-time and profile-guided optimisation (2026-10-17, user: "optimise
# melonDS as much as possible for H700"): the emulation thread is CPU-bound
# on one A53 core (2D software renderer, 3D command processing, JIT
# dispatch). Mario Kart DS on the CubeXX: 26.0 fps stock, 28-29 with LTO,
# 31.4 with LTO + PGO (menus 42 -> 54). pgo/ holds the GCC 15.3 profile
# trained on the device (docs/dev/emulation.md "melonDS PGO"): it matches
# this commit and these flags only, so regenerate it on a version or
# compiler bump.
# A function changed after the training (patch 0022, the cheats) only warns:
# GCC drops the profile of that function and keeps it for the others;
# retrain when a hot path changes.
# NUUBOS_MELONDS_PGO=generate builds the instrumented core instead (writes
# its counters to /tmp/nuubos-pgo on the device when RetroArch exits).
ifeq ($(NUUBOS_MELONDS_PGO),generate)
LIBRETRO_MELONDS_OPT_FLAGS = \
	-flto=$(PARALLEL_JOBS) \
	-fprofile-generate=/tmp/nuubos-pgo \
	-fprofile-update=prefer-atomic \
	-fprofile-prefix-path=$(@D)
else
LIBRETRO_MELONDS_OPT_FLAGS = \
	-flto=$(PARALLEL_JOBS) \
	-fprofile-use=$(LIBRETRO_MELONDS_PKGDIR)/pgo \
	-fprofile-prefix-path=$(@D) \
	-fprofile-partial-training \
	-Wno-missing-profile \
	-Wno-error=coverage-mismatch
endif

define LIBRETRO_MELONDS_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(LIBRETRO_CORE_ENV) \
		CFLAGS="$(TARGET_CFLAGS) $(LIBRETRO_CORE_CFLAGS) $(LIBRETRO_MELONDS_OPT_FLAGS)" \
		CXXFLAGS="$(TARGET_CXXFLAGS) $(LIBRETRO_MELONDS_OPT_FLAGS)" \
		LDFLAGS="$(TARGET_LDFLAGS) -lm $(LIBRETRO_MELONDS_OPT_FLAGS)" \
		$(MAKE) -C $(@D) -f Makefile \
		$(LIBRETRO_CORE_MAKE_OPTS) LD="$(TARGET_CXX)" ARCH=arm64
endef

define LIBRETRO_MELONDS_INSTALL_TARGET_CMDS
	$(call LIBRETRO_NUUBOS_CORE_INSTALL,melonds,melonds_nuubos,$(@D)/melonds_libretro.so)
endef

$(eval $(generic-package))
