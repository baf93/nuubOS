################################################################################
#
# skyscraper
#
################################################################################

SKYSCRAPER_VERSION = 3.21.0
SKYSCRAPER_SITE = $(call github,Gemba,skyscraper,$(SKYSCRAPER_VERSION))
SKYSCRAPER_LICENSE = GPL-3.0+
SKYSCRAPER_LICENSE_FILES = LICENSE
SKYSCRAPER_DEPENDENCIES = qt6base

# Upstream builds with qmake, which Buildroot's Qt6 does not provide: a
# CMake project of our own (same sources, defines and bundled resources).
define SKYSCRAPER_ADD_CMAKELISTS
	$(INSTALL) -m 0644 $(SKYSCRAPER_PKGDIR)/CMakeLists.txt $(@D)/CMakeLists.txt
endef
SKYSCRAPER_POST_EXTRACT_HOOKS += SKYSCRAPER_ADD_CMAKELISTS

SKYSCRAPER_CONF_OPTS = -DSKYSCRAPER_VERSION=$(SKYSCRAPER_VERSION)

$(eval $(cmake-package))
