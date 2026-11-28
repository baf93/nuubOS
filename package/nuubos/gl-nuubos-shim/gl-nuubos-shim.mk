################################################################################
#
# gl-nuubos-shim
#
################################################################################

GL_NUUBOS_SHIM_VERSION = 0.1.0
GL_NUUBOS_SHIM_SITE = $(BR2_EXTERNAL_NUUBOS_PATH)/package/nuubos/gl-nuubos-shim/src
GL_NUUBOS_SHIM_SITE_METHOD = local
GL_NUUBOS_SHIM_LICENSE = GPL-3.0+
GL_NUUBOS_SHIM_LICENSE_FILES = LICENSE
# The armhf pass installs a 32-bit ELF into an aarch64 rootfs on purpose.
GL_NUUBOS_SHIM_BIN_ARCH_EXCLUDE = /usr/lib32/

GL_NUUBOS_SHIM_ARMHF_CC = $(shell command -v arm-linux-gnueabihf-gcc)
ifeq ($(GL_NUUBOS_SHIM_ARMHF_CC),)
$(error gl-nuubos-shim: arm-linux-gnueabihf-gcc is required; rebuild nuubos-dev:0.5 with gcc-arm-linux-gnueabihf)
endif

# Native aarch64 pass and the armhf pass use the same source. The armhf flags
# are the ones fixed by the v2.1 specification (§1.1, second pass).
define GL_NUUBOS_SHIM_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -shared -fPIC \
		-std=c11 -D_GNU_SOURCE -O2 -Wall -Wextra -Wno-unused-parameter \
		-fvisibility=hidden $(@D)/nuubos_gl_shim.c \
		-o $(@D)/libgl_nuubos_shim.so -ldl -lpthread -lm
	$(GL_NUUBOS_SHIM_ARMHF_CC) -march=armv7-a -mfpu=neon-vfpv4 \
		-mfloat-abi=hard -shared -fPIC -std=c11 -D_GNU_SOURCE -O2 \
		-Wall -Wextra -Wno-unused-parameter -fvisibility=hidden \
		$(@D)/nuubos_gl_shim.c -o $(@D)/libgl_nuubos_shim-armhf.so \
		-ldl -lpthread -lm
endef

# SONAME-only LD_PRELOAD resolves through each ABI's own ld.so: aarch64 finds
# /usr/lib/libgl_nuubos_shim.so, armhf finds the copy in the multiarch dir it
# searches by default (/usr/lib/arm-linux-gnueabihf). /usr/lib32 is kept for
# PortMaster's own /usr/lib32 probes and for older launchers. No armhf
# ld.so.cache or LD_LIBRARY_PATH override is needed.
define GL_NUUBOS_SHIM_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/libgl_nuubos_shim.so \
		$(TARGET_DIR)/usr/lib/libgl_nuubos_shim.so
	$(INSTALL) -D -m 0755 $(@D)/libgl_nuubos_shim-armhf.so \
		$(TARGET_DIR)/usr/lib32/libgl_nuubos_shim.so
	$(INSTALL) -D -m 0755 $(@D)/libgl_nuubos_shim-armhf.so \
		$(TARGET_DIR)/usr/lib/arm-linux-gnueabihf/libgl_nuubos_shim.so
	# DT_NEEDED libGL.so.1 without a bundled gl4es: the shim supplies the
	# desktop GL entry points through this SONAME. GLX is not provided.
	ln -sfn libgl_nuubos_shim.so $(TARGET_DIR)/usr/lib/libGL.so.1
	ln -sfn libgl_nuubos_shim.so \
		$(TARGET_DIR)/usr/lib/arm-linux-gnueabihf/libGL.so.1
	ln -sfn libgl_nuubos_shim.so $(TARGET_DIR)/usr/lib32/libGL.so.1
	$(INSTALL) -D -m 0644 $(@D)/armhf-ld.so.conf \
		$(TARGET_DIR)/etc/ld.so.conf.d/40-nuubos-armhf.conf
endef

$(eval $(generic-package))
