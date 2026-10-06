################################################################################
#
# libretro-vice
#
################################################################################

# VICE (Commodore 64, 128, Plus/4, VIC-20): one core per machine. Pinned upstream commit; part of the
# default H700 core matrix (package/libretro/README.md).
LIBRETRO_VICE_VERSION = f63b56688f3133a2bb17499db9eebb7ab81df5f5
LIBRETRO_VICE_SITE = $(call github,libretro,vice-libretro,$(LIBRETRO_VICE_VERSION))
LIBRETRO_VICE_LICENSE = GPL-2.0
LIBRETRO_VICE_LICENSE_FILES = COPYING

LIBRETRO_VICE_EMUTYPES = x64 x128 xplus4 xvic

# The objects depend on EMUTYPE: clean between machines.
define LIBRETRO_VICE_BUILD_CMDS
	mkdir -p $(@D)/nuubos-out
	for t in $(LIBRETRO_VICE_EMUTYPES); do \
		$(TARGET_MAKE_ENV) $(LIBRETRO_CORE_ENV) $(MAKE) -C $(@D) -f Makefile \
			$(LIBRETRO_CORE_MAKE_OPTS) LD="$(TARGET_CC)" EMUTYPE=$$t clean && \
		$(TARGET_MAKE_ENV) $(LIBRETRO_CORE_ENV) $(MAKE) -C $(@D) -f Makefile \
			$(LIBRETRO_CORE_MAKE_OPTS) LD="$(TARGET_CC)" EMUTYPE=$$t && \
		cp $(@D)/vice_$${t}_libretro.so $(@D)/nuubos-out/ || exit 1; \
	done
endef

define LIBRETRO_VICE_INSTALL_TARGET_CMDS
	for t in $(LIBRETRO_VICE_EMUTYPES); do \
		$(INSTALL) -D -m 0755 $(@D)/nuubos-out/vice_$${t}_libretro.so \
			$(TARGET_DIR)/usr/lib/libretro/vice_$${t}_libretro.so || exit 1; \
	done
endef

$(eval $(generic-package))
