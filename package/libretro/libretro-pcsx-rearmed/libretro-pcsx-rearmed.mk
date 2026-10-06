################################################################################
#
# libretro-pcsx-rearmed
#
################################################################################

# PCSX ReARMed (PlayStation), ARM64 dynarec + NEON GPU. Pinned upstream commit; part of the
# default H700 core matrix (package/libretro/README.md).
LIBRETRO_PCSX_REARMED_VERSION = c8816799b50388e61cfe237fe2cdbb7d8175f20a
LIBRETRO_PCSX_REARMED_SITE = https://github.com/libretro/pcsx_rearmed.git
LIBRETRO_PCSX_REARMED_SITE_METHOD = git
LIBRETRO_PCSX_REARMED_GIT_SUBMODULES = YES
LIBRETRO_PCSX_REARMED_LICENSE = GPL-2.0
LIBRETRO_PCSX_REARMED_LICENSE_FILES = COPYING

# No physical CD-ROM drives on a handheld; the upstream physical CD code
# also references dir_list_new() without building it (the core then fails
# to load with an undefined symbol).
define LIBRETRO_PCSX_REARMED_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(LIBRETRO_CORE_ENV) $(MAKE) -C $(@D) -f Makefile.libretro \
		$(LIBRETRO_CORE_MAKE_OPTS) LD="$(TARGET_CC)" HAVE_PHYSICAL_CDROM=0
endef

define LIBRETRO_PCSX_REARMED_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/pcsx_rearmed_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/pcsx_rearmed_libretro.so
endef

$(eval $(generic-package))
