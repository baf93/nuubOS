################################################################################
#
# libretro-mame
#
################################################################################

# Current MAME libretro core, built with only the drivers of the computers
# and consoles nuubOS has no dedicated core for (Apple II, TRS-80 CoCo,
# Dragon, Gamate, Game.com, Sord M5, Super Vision 8000, TI-99/4A, TRS-80,
# V.Smile, Super Cassette Vision, Oric, FM Towns). Arcade games use FBNeo
# and MAME 2003-Plus. Every machine needs its own system ROMs from the user.
LIBRETRO_MAME_VERSION = 9069f39340f2d2b1795df8e71bb1d3d0fbc76598
LIBRETRO_MAME_SITE = $(call github,libretro,mame,$(LIBRETRO_MAME_VERSION))
LIBRETRO_MAME_LICENSE = GPL-2.0+
LIBRETRO_MAME_LICENSE_FILES = COPYING

LIBRETRO_MAME_DRIVERS = \
	apple/apple2.cpp apple/apple2e.cpp trs/coco12.cpp trs/coco3.cpp \
	trs/dragon.cpp bitcorp/gamate.cpp tiger/gamecom.cpp sord/m5.cpp \
	bandai/sv8000.cpp ti/ti99_4x.cpp trs/trs80.cpp vtech/vsmile.cpp \
	epoch/scv.cpp tangerine/oric.cpp fujitsu/fmtowns.cpp

LIBRETRO_MAME_SOURCES = \
	$(subst $(space),$(comma),$(strip $(addprefix src/mame/,$(LIBRETRO_MAME_DRIVERS))))

LIBRETRO_MAME_MAKE_OPTS = \
	OSD=retro TARGETOS=linux PLATFORM=arm64 PTR64=1 CROSS_BUILD=1 \
	ARCHITECTURE= NOASM=1 CONFIG=libretro \
	OVERRIDE_CC="$(TARGET_CC)" OVERRIDE_CXX="$(TARGET_CXX)" \
	OVERRIDE_LD="$(TARGET_CXX)" OVERRIDE_AR="$(TARGET_AR)" \
	REGENIE=1 NOWERROR=1 NO_USE_MIDI=1 NO_USE_PORTAUDIO=1 \
	NO_OPENGL=1 USE_QTDEBUG=0 DONT_USE_NETWORK=1 \
	PYTHON_EXECUTABLE=python3 SUBTARGET=nuubos \
	SOURCES=$(LIBRETRO_MAME_SOURCES)

define LIBRETRO_MAME_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) -f makefile $(LIBRETRO_MAME_MAKE_OPTS)
endef

define LIBRETRO_MAME_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $$(ls $(@D)/*_libretro.so | head -1) \
		$(TARGET_DIR)/usr/lib/libretro/mame_libretro.so
endef

$(eval $(generic-package))
