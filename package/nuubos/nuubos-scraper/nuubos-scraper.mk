################################################################################
#
# nuubos-scraper
#
################################################################################

NUUBOS_SCRAPER_VERSION = 0.1.0
NUUBOS_SCRAPER_SITE = $(BR2_EXTERNAL_NUUBOS_PATH)/package/nuubos/nuubos-scraper/src
NUUBOS_SCRAPER_SITE_METHOD = local
NUUBOS_SCRAPER_LICENSE = MIT
NUUBOS_SCRAPER_LICENSE_FILES = LICENSE
NUUBOS_SCRAPER_DEPENDENCIES = libcurl cjson zlib

# Developer credentials issued to nuubOS by ScreenScraper: never in the
# repository (local/ is ignored by git).
NUUBOS_SCRAPER_DEV_CONF = $(BR2_EXTERNAL_NUUBOS_PATH)/local/screenscraper-dev.conf
NUUBOS_SCRAPER_DEVID = $(shell sed -n 's/^DEVID=//p' $(NUUBOS_SCRAPER_DEV_CONF) 2>/dev/null)
NUUBOS_SCRAPER_DEVPASSWORD = $(shell sed -n 's/^DEVPASSWORD=//p' $(NUUBOS_SCRAPER_DEV_CONF) 2>/dev/null)

define NUUBOS_SCRAPER_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-format-truncation \
		-DSS_DEVID='"$(NUUBOS_SCRAPER_DEVID)"' \
		-DSS_DEVPASSWORD='"$(NUUBOS_SCRAPER_DEVPASSWORD)"' \
		$(@D)/scraper.c -o $(@D)/nuubos-scraper -lcurl -lcjson -lz
endef

define NUUBOS_SCRAPER_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/nuubos-scraper $(TARGET_DIR)/usr/bin/nuubos-scraper
	$(INSTALL) -D -m 0644 $(@D)/scrape-game.job $(TARGET_DIR)/usr/share/nuubos/jobs/scrape-game.job
	$(INSTALL) -D -m 0644 $(@D)/scrape-bulk.job $(TARGET_DIR)/usr/share/nuubos/jobs/scrape-bulk.job
endef

$(eval $(generic-package))
