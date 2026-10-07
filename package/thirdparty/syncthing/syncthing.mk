################################################################################
#
# syncthing
#
################################################################################

SYNCTHING_VERSION = 2.1.6
SYNCTHING_SITE = $(call github,syncthing,syncthing,v$(SYNCTHING_VERSION))
SYNCTHING_LICENSE = MPL-2.0
SYNCTHING_LICENSE_FILES = LICENSE
SYNCTHING_GOMOD = github.com/syncthing/syncthing
SYNCTHING_BUILD_TARGETS = cmd/syncthing
# nuubOS updates through its own OTA: Syncthing never replaces itself.
# No embedded web GUI: nuubOS manages Syncthing over REST on a unix socket
# (nuubos-syncctl), so the GUI assets (and their JS licences) are left out.
SYNCTHING_TAGS = noupgrade noassets
SYNCTHING_LDFLAGS = \
	-X github.com/syncthing/syncthing/lib/build.Version=v$(SYNCTHING_VERSION) \
	-X github.com/syncthing/syncthing/lib/build.User=nuubos \
	-X github.com/syncthing/syncthing/lib/build.Host=buildroot

$(eval $(golang-package))
