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

# Static link: the launcher must not depend on libraries in the rootfs.
ENTER_SOURCES = \
	$(@D)/main.c \
	$(@D)/container.c \
	$(@D)/diagnostics.c \
	$(@D)/environment.c \
	$(@D)/filesystem.c \
	$(@D)/namespaces.c \
	$(@D)/process.c

define ENTER_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=gnu99 -Wall -Wextra -Wformat=2 \
		-static -o $(@D)/enter $(ENTER_SOURCES)
endef

define ENTER_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/enter $(TARGET_DIR)/enter
endef

$(eval $(generic-package))
