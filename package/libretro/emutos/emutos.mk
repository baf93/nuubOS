################################################################################
#
# emutos
#
################################################################################

# EmuTOS: free GPL replacement of the Atari ST TOS ROM, so the Hatari core
# runs without a proprietary TOS image. Installed as the system tos.img
# (nuubos-emud copies it into /userdata/bios when the user has none).
EMUTOS_VERSION = 1.4
EMUTOS_SITE = https://downloads.sourceforge.net/project/emutos/emutos/$(EMUTOS_VERSION)
EMUTOS_SOURCE = emutos-256k-$(EMUTOS_VERSION).zip
EMUTOS_LICENSE = GPL-2.0+
EMUTOS_LICENSE_FILES = doc/license.txt

define EMUTOS_EXTRACT_CMDS
	$(UNZIP) -q -o $(EMUTOS_DL_DIR)/$(EMUTOS_SOURCE) -d $(@D)
	mv $(@D)/emutos-256k-$(EMUTOS_VERSION)/* $(@D)/
endef

define EMUTOS_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(@D)/etos256us.img \
		$(TARGET_DIR)/usr/share/nuubos/emulation/system/tos.img
endef

$(eval $(generic-package))
