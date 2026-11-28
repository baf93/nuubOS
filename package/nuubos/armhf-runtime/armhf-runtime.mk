################################################################################
#
# armhf-runtime
#
################################################################################

ARMHF_RUNTIME_VERSION = 1
ARMHF_RUNTIME_SITE = $(BR2_EXTERNAL_NUUBOS_PATH)/package/nuubos/armhf-runtime
ARMHF_RUNTIME_SITE_METHOD = local
ARMHF_RUNTIME_LICENSE = LGPL-2.1+, GPL-3.0+ with GCC exception, MIT, Zlib, Apache-2.0 with LLVM exception
ARMHF_RUNTIME_LICENSE_FILES = NOTICE.md
# Only 32-bit ELF libraries live under this path.
ARMHF_RUNTIME_BIN_ARCH_EXCLUDE = /usr/lib/arm-linux-gnueabihf/

# SITE_METHOD=local rsyncs the package directory; unpack the prebuilt runtime
# here instead of relying on the generic tarball download path.
define ARMHF_RUNTIME_BUILD_CMDS
	tar -xf $(@D)/armhf-runtime.tar.xz -C $(@D)
endef

define ARMHF_RUNTIME_INSTALL_TARGET_CMDS
	$(INSTALL) -d -m 0755 $(TARGET_DIR)/usr/lib/arm-linux-gnueabihf
	cp -a $(@D)/usr/lib/arm-linux-gnueabihf/. $(TARGET_DIR)/usr/lib/arm-linux-gnueabihf/
	$(INSTALL) -d -m 0755 $(TARGET_DIR)/usr/share/glvnd/egl_vendor.d
	$(INSTALL) -m 0644 $(@D)/usr/share/glvnd/egl_vendor.d/50_mesa.json \
		$(TARGET_DIR)/usr/share/glvnd/egl_vendor.d/50_mesa.json
	ln -sfn /usr/lib/arm-linux-gnueabihf/ld-linux-armhf.so.3 \
		$(TARGET_DIR)/lib/ld-linux-armhf.so.3
endef

$(eval $(generic-package))
