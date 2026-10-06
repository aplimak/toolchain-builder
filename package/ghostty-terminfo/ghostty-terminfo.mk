################################################################################
#
# ghostty-terminfo
#
################################################################################

GHOSTTY_TERMINFO_VERSION = 1.0
GHOSTTY_TERMINFO_SITE = $(BR2_EXTERNAL_AELIUX_EXTERNAL_PATH)/package/ghostty-terminfo
GHOSTTY_TERMINFO_SITE_METHOD = local
GHOSTTY_TERMINFO_LICENSE = MIT
GHOSTTY_TERMINFO_DEPENDENCIES = host-ncurses

define GHOSTTY_TERMINFO_BUILD_CMDS
    $(HOST_DIR)/bin/tic -x -o $(@D)/terminfo $(@D)/xterm-ghostty.terminfo
endef

define GHOSTTY_TERMINFO_INSTALL_TARGET_CMDS
    mkdir -p $(TARGET_DIR)/usr/share/terminfo/x
    cp -a $(@D)/terminfo/x/xterm-ghostty $(TARGET_DIR)/usr/share/terminfo/x/
endef

$(eval $(generic-package))
