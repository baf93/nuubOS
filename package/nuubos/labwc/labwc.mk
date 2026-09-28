################################################################################
#
# labwc
#
################################################################################

LABWC_VERSION = 0.20.2
LABWC_SITE = $(call github,labwc,labwc,$(LABWC_VERSION))
LABWC_LICENSE = GPL-2.0+
LABWC_LICENSE_FILES = LICENSE
LABWC_DEPENDENCIES = cairo libxml2 pango wayland wlroots xkeyboard-config libpng
LABWC_CONF_OPTS =     -Dman-pages=disabled     -Dxwayland=disabled     -Dsvg=disabled     -Dicon=disabled     -Dlabnag=disabled     -Dnls=disabled     -Dsystemd-session=disabled     -Dtest=disabled

$(eval $(meson-package))
