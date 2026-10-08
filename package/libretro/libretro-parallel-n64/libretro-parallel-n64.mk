################################################################################
#
# libretro-parallel-n64
#
################################################################################

# ParaLLEl N64 (Nintendo 64): lighter than Mupen64Plus-Next on Mali-G31
# (gln64/Glide64/Rice renderers on GLES3). Left out: the Vulkan ParaLLEl
# RDP/RSP and the bundled GLideN64 (desktop GL headers; Mupen64Plus-Next
# already provides GLideN64). Pinned upstream commit; part of the default
# H700 core matrix (package/libretro/README.md).
LIBRETRO_PARALLEL_N64_VERSION = 862071a8b786224905a763e8b15bdcaf75739341
LIBRETRO_PARALLEL_N64_SITE = $(call github,libretro,parallel-n64,$(LIBRETRO_PARALLEL_N64_VERSION))
LIBRETRO_PARALLEL_N64_LICENSE = GPL-2.0
LIBRETRO_PARALLEL_N64_LICENSE_FILES = mupen64plus-core/LICENSES
LIBRETRO_PARALLEL_N64_DEPENDENCIES = libgles

define LIBRETRO_PARALLEL_N64_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(LIBRETRO_CORE_ENV) $(MAKE) -C $(@D) -f Makefile \
		$(LIBRETRO_CORE_MAKE_OPTS) FORCE_GLES=1 GLES3=1 ARCH=aarch64 \
		HAVE_PARALLEL=0 HAVE_PARALLEL_RSP=0 HAVE_GLIDEN64=0 GIT_VERSION=" $(LIBRETRO_PARALLEL_N64_VERSION)"
endef

define LIBRETRO_PARALLEL_N64_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/parallel_n64_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/parallel_n64_libretro.so
endef

$(eval $(generic-package))
