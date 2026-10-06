################################################################################
#
# libretro-picodrive
#
################################################################################

# PicoDrive (Sega 32X), ARM64 dynarec. Pinned upstream commit; part of the
# default H700 core matrix (package/libretro/README.md).
LIBRETRO_PICODRIVE_VERSION = 1890c2932234c9d30f4cd3851d02228baae8f09e
LIBRETRO_PICODRIVE_SITE = https://github.com/libretro/picodrive.git
LIBRETRO_PICODRIVE_SITE_METHOD = git
LIBRETRO_PICODRIVE_GIT_SUBMODULES = YES
LIBRETRO_PICODRIVE_LICENSE = PicoDrive (non-commercial)
LIBRETRO_PICODRIVE_LICENSE_FILES = COPYING

# Buildroot's _LARGEFILE64_SOURCE makes dr_mp3 call fopen64() directly,
# bypassing the libretro VFS that replaces fopen() (a FILE * ends up used as
# an RFILE *): build without it, off_t is 64-bit on aarch64 anyway.
define LIBRETRO_PICODRIVE_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(LIBRETRO_CORE_ENV) \
		CFLAGS="$(TARGET_CFLAGS) $(LIBRETRO_CORE_CFLAGS) -U_LARGEFILE64_SOURCE" \
		$(MAKE) -C $(@D) -f Makefile.libretro \
		$(LIBRETRO_CORE_MAKE_OPTS) LD="$(TARGET_CC)"
endef

define LIBRETRO_PICODRIVE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/picodrive_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/picodrive_libretro.so
endef

$(eval $(generic-package))
