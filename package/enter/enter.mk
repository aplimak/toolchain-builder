################################################################################
#
# enter
#
################################################################################

ENTER_VERSION = 1.0
ENTER_SITE = $(BR2_EXTERNAL_AELIUX_EXTERNAL_PATH)/package/enter
ENTER_SITE_METHOD = local
ENTER_LICENSE = MIT
ENTER_LICENSE_FILES =

# Static link: enter runs inside an isolated rootfs where the host's
# dynamic loader and shared libs are not guaranteed to be reachable.
# musl and glibc toolchains provide libc.a; uClibc-ng does too if the
# toolchain was built with static support.
define ENTER_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-static -o $(@D)/enter $(@D)/enter.c
endef

define ENTER_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/enter $(TARGET_DIR)/enter
endef

$(eval $(generic-package))
