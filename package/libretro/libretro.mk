################################################################################
#
# Common settings for the libretro core packages
#
################################################################################

# libretro core Makefiles append their own flags to CC/CFLAGS taken from the
# environment. GCC 15 defaults to C23 and turns several old warnings into
# errors (pointer/int conversions, implicit declarations) that much older
# emulator C code still has, so C is pinned to gnu11 with those kept as
# warnings.
LIBRETRO_CORE_CFLAGS = \
	-std=gnu11 \
	-Wno-error=incompatible-pointer-types \
	-Wno-error=int-conversion \
	-Wno-error=implicit-function-declaration \
	-Wno-error=implicit-int

LIBRETRO_CORE_ENV = \
	$(TARGET_CONFIGURE_OPTS) \
	CFLAGS="$(TARGET_CFLAGS) $(LIBRETRO_CORE_CFLAGS)" \
	CXXFLAGS="$(TARGET_CXXFLAGS)" \
	LDFLAGS="$(TARGET_LDFLAGS) -lm"

LIBRETRO_CORE_MAKE_OPTS = platform=unix
