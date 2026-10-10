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
ENTER_CONF_OPTS = --enable-static --bindir=/

ifeq ($(BR2_PACKAGE_ENTER_RUNTIME_WARNINGS),y)
ENTER_CONF_OPTS += --enable-runtime-warnings
else
ENTER_CONF_OPTS += --disable-runtime-warnings
endif

$(eval $(autotools-package))
