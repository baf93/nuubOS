################################################################################
#
# libretro-bluemsx
#
################################################################################

# blueMSX (MSX/MSX2/MSX2+/turboR, SVI, ColecoVision), with C-BIOS. Pinned upstream commit; part of the
# default H700 core matrix (package/libretro/README.md).
LIBRETRO_BLUEMSX_VERSION = e3086eb5d36d77fa11704cf53dc176686e70127d
LIBRETRO_BLUEMSX_SITE = $(call github,libretro,blueMSX-libretro,$(LIBRETRO_BLUEMSX_VERSION))
LIBRETRO_BLUEMSX_LICENSE = GPL-2.0
LIBRETRO_BLUEMSX_LICENSE_FILES = license.txt

define LIBRETRO_BLUEMSX_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(LIBRETRO_CORE_ENV) $(MAKE) -C $(@D) -f Makefile.libretro \
		$(LIBRETRO_CORE_MAKE_OPTS) LD="$(TARGET_CXX)"
endef

# System files: the Databases and only machines without proprietary ROMs.
# The upstream MSX/ColecoVision BIOS images have no clear redistribution
# rights and are not shipped; the free C-BIOS machines keep their upstream
# names, which their config.ini ROM paths refer to: when the machine the
# core picks automatically (MSX2+, ...) is missing, the core falls back to
# "MSX2+ - C-BIOS" and the others. Users can add their own BIOS machines in
# /userdata/bios/Machines.
LIBRETRO_BLUEMSX_SYSTEM = $(TARGET_DIR)/usr/share/nuubos/emulation/system

define LIBRETRO_BLUEMSX_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/bluemsx_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/bluemsx_libretro.so
	rm -rf $(LIBRETRO_BLUEMSX_SYSTEM)/Databases $(LIBRETRO_BLUEMSX_SYSTEM)/Machines
	mkdir -p $(LIBRETRO_BLUEMSX_SYSTEM)/Machines
	cp -a $(@D)/system/bluemsx/Databases $(LIBRETRO_BLUEMSX_SYSTEM)/
	cd $(@D)/system/bluemsx/Machines && for m in *; do \
		[ -d "$$m" ] || continue; \
		case "$$m" in *" - C-BIOS") ;; \
		*) ls "$$m" | grep -qiE '\.(rom|bin)$$' && continue ;; esac; \
		cp -a "$$m" $(LIBRETRO_BLUEMSX_SYSTEM)/Machines/; \
	done
endef

$(eval $(generic-package))
