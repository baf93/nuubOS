################################################################################
#
# libretro-np2kai
#
################################################################################

# Neko Project II kai (NEC PC-9801). Pinned upstream commit; part of the
# default H700 core matrix (package/libretro/README.md).
LIBRETRO_NP2KAI_VERSION = 1ea561cd7fd62b33f4e7b3f1c2e30fc3e6089f28
LIBRETRO_NP2KAI_SITE = $(call github,libretro,NP2kai,$(LIBRETRO_NP2KAI_VERSION))
LIBRETRO_NP2KAI_LICENSE = MIT
LIBRETRO_NP2KAI_LICENSE_FILES = LICENSE

define LIBRETRO_NP2KAI_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(LIBRETRO_CORE_ENV) $(MAKE) -C $(@D)/sdl -f Makefile.libretro \
		$(LIBRETRO_CORE_MAKE_OPTS) LD="$(TARGET_CXX)"
endef

define LIBRETRO_NP2KAI_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/sdl/np2kai_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/np2kai_libretro.so
endef

$(eval $(generic-package))
