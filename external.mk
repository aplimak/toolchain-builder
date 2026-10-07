include $(sort $(wildcard $(BR2_EXTERNAL_AELIUX_EXTERNAL_PATH)/package/*/*.mk))

HOST_READLINE_CONF_OPTS = \
	--disable-install-examples \
	--with-curses \
	--with-shared-termcap-library
