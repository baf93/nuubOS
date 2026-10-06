################################################################################
#
# libretro-a5200
#
################################################################################

# a5200 (Atari 5200). Pinned upstream commit; part of the
# default H700 core matrix (package/libretro/README.md).
LIBRETRO_A5200_VERSION = 40c6f2f1ad4a3145b328d5baaf010fae6c7e752b
LIBRETRO_A5200_SITE = $(call github,libretro,a5200,$(LIBRETRO_A5200_VERSION))
LIBRETRO_A5200_LICENSE = GPL-2.0
LIBRETRO_A5200_LICENSE_FILES = License.txt

define LIBRETRO_A5200_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(LIBRETRO_CORE_ENV) $(MAKE) -C $(@D) -f Makefile \
		$(LIBRETRO_CORE_MAKE_OPTS) LD="$(TARGET_CC)"
endef

define LIBRETRO_A5200_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/a5200_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/a5200_libretro.so
endef

$(eval $(generic-package))
