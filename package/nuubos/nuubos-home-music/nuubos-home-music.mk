################################################################################
#
# nuubos-home-music
#
################################################################################

# Default Home Music: Kevin MacLeod (incompetech.com), CC BY 4.0, unmodified.
NUUBOS_HOME_MUSIC_VERSION = 2026.10
NUUBOS_HOME_MUSIC_SITE = https://incompetech.com/music/royalty-free/mp3-royaltyfree
NUUBOS_HOME_MUSIC_TRACKS = \
	Airport%20Lounge.mp3 \
	Backbay%20Lounge.mp3 \
	Bossa%20Antigua.mp3 \
	Cool%20Vibes.mp3 \
	Jazz%20Brunch.mp3 \
	Laid%20Back%20Guitars.mp3 \
	Lobby%20Time.mp3 \
	Smooth%20Lovin.mp3
NUUBOS_HOME_MUSIC_SOURCE = $(firstword $(NUUBOS_HOME_MUSIC_TRACKS))
NUUBOS_HOME_MUSIC_EXTRA_DOWNLOADS = $(wordlist 2,$(words $(NUUBOS_HOME_MUSIC_TRACKS)),$(NUUBOS_HOME_MUSIC_TRACKS))
NUUBOS_HOME_MUSIC_LICENSE = CC-BY-4.0
NUUBOS_HOME_MUSIC_LICENSE_FILES = ATTRIBUTION.txt
NUUBOS_HOME_MUSIC_REDISTRIBUTE = YES

define NUUBOS_HOME_MUSIC_EXTRACT_CMDS
	$(foreach t,$(NUUBOS_HOME_MUSIC_TRACKS),\
		cp "$(NUUBOS_HOME_MUSIC_DL_DIR)/$(t)" "$(@D)/$(t)"$(sep))
	cp $(NUUBOS_HOME_MUSIC_PKGDIR)/ATTRIBUTION.txt $(@D)/ATTRIBUTION.txt
endef

define NUUBOS_HOME_MUSIC_INSTALL_TARGET_CMDS
	$(foreach t,$(NUUBOS_HOME_MUSIC_TRACKS),\
		$(INSTALL) -D -m 0644 "$(@D)/$(t)" \
			"$(TARGET_DIR)/usr/share/nuubos/home-music/$(subst %20, ,$(t))"$(sep))
	$(INSTALL) -D -m 0644 $(@D)/ATTRIBUTION.txt \
		$(TARGET_DIR)/usr/share/nuubos/home-music/ATTRIBUTION.txt
endef

$(eval $(generic-package))
