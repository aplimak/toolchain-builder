# Avoid surprises by disabling default rules
MAKEFLAGS += --no-builtin-rules
.SUFFIXES:

THIS_EXTERNAL_PATH := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

OUTPUT_BASEDIR = $(THIS_EXTERNAL_PATH)/output
OUTPUT_DIR = $(OUTPUT_BASEDIR)/$*

MAKE_BUILDROOT = $(MAKE) -C $(THIS_EXTERNAL_PATH)/buildroot BR2_EXTERNAL=$(THIS_EXTERNAL_PATH)

%: $(THIS_EXTERNAL_PATH)/configs/%.config
	"$(THIS_EXTERNAL_PATH)/gen-config.sh" "$*"
	$(MAKE_BUILDROOT) O="$(OUTPUT_BASEDIR)/$*" \
	    BR2_DEFCONFIG="$(THIS_EXTERNAL_PATH)/.build/$*" defconfig
