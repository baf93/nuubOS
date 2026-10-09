################################################################################
#
# nuubos-scraper
#
################################################################################

NUUBOS_SCRAPER_VERSION = 0.2.0
NUUBOS_SCRAPER_SITE = $(BR2_EXTERNAL_NUUBOS_PATH)/package/nuubos/nuubos-scraper/src
NUUBOS_SCRAPER_SITE_METHOD = local
NUUBOS_SCRAPER_LICENSE = MIT
NUUBOS_SCRAPER_LICENSE_FILES = LICENSE
NUUBOS_SCRAPER_DEPENDENCIES = libcurl

define NUUBOS_SCRAPER_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-format-truncation \
		$(@D)/scraper.c -o $(@D)/nuubos-scraper -lcurl
endef

define NUUBOS_SCRAPER_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/nuubos-scraper $(TARGET_DIR)/usr/bin/nuubos-scraper
	$(INSTALL) -D -m 0644 $(@D)/scrape-game.job $(TARGET_DIR)/usr/share/nuubos/jobs/scrape-game.job
	$(INSTALL) -D -m 0644 $(@D)/scrape-bulk.job $(TARGET_DIR)/usr/share/nuubos/jobs/scrape-bulk.job
endef

$(eval $(generic-package))
