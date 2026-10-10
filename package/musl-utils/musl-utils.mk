################################################################################
#
# musl-utils
#
################################################################################

MUSL_UTILS_VERSION = 1.0
MUSL_UTILS_SITE = $(BR2_EXTERNAL_AELIUX_EXTERNAL_PATH)/package/musl-utils
MUSL_UTILS_SITE_METHOD = local
MUSL_UTILS_LICENSE = MIT
MUSL_UTILS_LICENSE_FILES =

define MUSL_UTILS_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -o $(@D)/getconf $(@D)/getconf.c
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -o $(@D)/getent $(@D)/getent.c
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -o $(@D)/iconv $(@D)/iconv.c
endef

define MUSL_UTILS_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/getconf $(TARGET_DIR)/usr/bin/getconf
	$(INSTALL) -D -m 0755 $(@D)/getent $(TARGET_DIR)/usr/bin/getent
	$(INSTALL) -D -m 0755 $(@D)/iconv $(TARGET_DIR)/usr/bin/iconv
	$(LN) -sf /lib/libc.so $(TARGET_DIR)/usr/bin/ldd
endef

$(eval $(generic-package))
