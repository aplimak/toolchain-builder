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
ENTER_CONF_OPTS = --enable-static --bindir=/ --disable-tests --enable-binary-trimming
ifeq ($(BR2_PACKAGE_LIBSELINUX),y)
ENTER_DEPENDENCIES += libselinux
ENTER_CONF_OPTS += --enable-selinux
else
ENTER_CONF_OPTS += --disable-selinux
endif

ifeq ($(BR2_PACKAGE_ENTER_RUNTIME_WARNINGS),y)
ENTER_CONF_OPTS += --enable-runtime-warnings
else
ENTER_CONF_OPTS += --disable-runtime-warnings
endif

ifeq ($(BR2_PACKAGE_ENTER_CUSTOM_PROGRAM),y)
ENTER_CONF_OPTS += --enable-custom-program
else
ENTER_CONF_OPTS += --disable-custom-program
endif

ifeq ($(BR2_PACKAGE_ENTER_INIT_SCRIPTS),y)
ENTER_CONF_OPTS += --enable-init-scripts
else
ENTER_CONF_OPTS += --disable-init-scripts
endif

ifeq ($(BR2_PACKAGE_ENTER_OVERLAY),y)
ENTER_CONF_OPTS += --enable-overlay
else
ENTER_CONF_OPTS += --disable-overlay
endif

ifeq ($(BR2_PACKAGE_ENTER_IMAGE_OVERLAY),y)
ENTER_CONF_OPTS += --enable-image-overlay --with-mke2fs=$(BR2_PACKAGE_ENTER_MKE2FS_PATH)
else
ENTER_CONF_OPTS += --disable-image-overlay
endif

ifeq ($(BR2_PACKAGE_ENTER_PTY_RELAY),y)
ENTER_CONF_OPTS += --enable-pty-relay
else
ENTER_CONF_OPTS += --disable-pty-relay
endif

ifeq ($(BR2_PACKAGE_ENTER_USER_NAMESPACE),y)
ENTER_CONF_OPTS += --enable-user-namespace
else
ENTER_CONF_OPTS += --disable-user-namespace
endif

ifeq ($(BR2_PACKAGE_ENTER_PID_NAMESPACE),y)
ENTER_CONF_OPTS += --enable-pid-namespace
else
ENTER_CONF_OPTS += --disable-pid-namespace
endif

ifeq ($(BR2_PACKAGE_ENTER_UTS_NAMESPACE),y)
ENTER_CONF_OPTS += --enable-uts-namespace
else
ENTER_CONF_OPTS += --disable-uts-namespace
endif

ifeq ($(BR2_PACKAGE_ENTER_IPC_NAMESPACE),y)
ENTER_CONF_OPTS += --enable-ipc-namespace
else
ENTER_CONF_OPTS += --disable-ipc-namespace
endif

ifeq ($(BR2_PACKAGE_ENTER_OPEN_TREE),y)
ENTER_CONF_OPTS += --enable-enter-open-tree
else
ENTER_CONF_OPTS += --disable-enter-open-tree
endif

ifeq ($(BR2_PACKAGE_ENTER_PIVOT_ROOT),y)
ENTER_CONF_OPTS += --enable-enter-pivot-root
else
ENTER_CONF_OPTS += --disable-enter-pivot-root
endif

ifeq ($(BR2_PACKAGE_ENTER_MOVE_MOUNT),y)
ENTER_CONF_OPTS += --enable-enter-move-mount
else
ENTER_CONF_OPTS += --disable-enter-move-mount
endif

ifeq ($(BR2_PACKAGE_ENTER_CHROOT),y)
ENTER_CONF_OPTS += --enable-enter-chroot
else
ENTER_CONF_OPTS += --disable-enter-chroot
endif

ifeq ($(BR2_PACKAGE_ENTER_DEFAULT_RESOLVER),y)
ENTER_CONF_OPTS += --enable-default-resolver
else
ENTER_CONF_OPTS += --disable-default-resolver
endif

$(eval $(autotools-package))
