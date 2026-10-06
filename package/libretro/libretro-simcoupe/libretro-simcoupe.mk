################################################################################
#
# libretro-simcoupe
#
################################################################################

# SimCoupe (SAM Coupé). Pinned upstream commit; part of the
# default H700 core matrix (package/libretro/README.md).
LIBRETRO_SIMCOUPE_VERSION = c28046241ac6a4d79e55326b6e354dc02f92fa34
LIBRETRO_SIMCOUPE_SITE = $(call github,libretro,libretro-simcoupe,$(LIBRETRO_SIMCOUPE_VERSION))
LIBRETRO_SIMCOUPE_LICENSE = GPL-2.0
LIBRETRO_SIMCOUPE_LICENSE_FILES = SimCoupe/License.txt

# Upstream tarball ships prebuilt x86-64 objects: drop them.
define LIBRETRO_SIMCOUPE_REMOVE_PREBUILT
	find $(@D) -name '*.o' -delete
endef
LIBRETRO_SIMCOUPE_POST_EXTRACT_HOOKS += LIBRETRO_SIMCOUPE_REMOVE_PREBUILT

define LIBRETRO_SIMCOUPE_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(LIBRETRO_CORE_ENV) $(MAKE) -C $(@D) -f Makefile \
		$(LIBRETRO_CORE_MAKE_OPTS) LD="$(TARGET_CXX)" CC="$(TARGET_CC)" CXX="$(TARGET_CXX)"
endef

define LIBRETRO_SIMCOUPE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/libretro-simcp.so \
		$(TARGET_DIR)/usr/lib/libretro/simcp_libretro.so
endef

$(eval $(generic-package))
