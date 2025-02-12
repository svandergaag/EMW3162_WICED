#
# Copyright 2014, Broadcom Corporation
# All Rights Reserved.
#
# This is UNPUBLISHED PROPRIETARY SOURCE CODE of Broadcom Corporation;
# the contents of this file may not be disclosed to third parties, copied
# or duplicated in any form, in whole or in part, without the prior
# written permission of Broadcom Corporation.
#

.PHONY: bootloader download_bootloader no_dct download_dct download

BOOTLOADER_TARGET := waf.bootloader-NoOS-NoNS-$(PLATFORM_FULL)-$(BUS)
BOOTLOADER_OUTFILE := $(BUILD_DIR)/$(call CONV_COMP,$(subst .,/,$(BOOTLOADER_TARGET)))/binary/$(call CONV_COMP,$(subst .,/,$(BOOTLOADER_TARGET)))
BOOTLOADER_LOG_FILE ?= $(BUILD_DIR)/bootloader.log
GENERATED_MAC_FILE := $(SOURCE_ROOT)generated_mac_address.txt
MAC_GENERATOR      := $(TOOLS_ROOT)/mac_generator/mac_generator.pl

OPENOCD_LOG_FILE ?= build/openocd_log.txt
DOWNLOAD_LOG := >> $(OPENOCD_LOG_FILE)

ifeq (,$(and $(OPENOCD_PATH),$(OPENOCD_FULL_NAME)))
	$(error Path to OpenOCD has not been set using OPENOCD_PATH and OPENOCD_FULL_NAME)
endif
QUIET=
VERBOSE=1
ifneq ($(VERBOSE),1)
BOOTLOADER_REDIRECT	= > $(BOOTLOADER_LOG_FILE)
endif

OPENOCD_FULL_NAME=/usr/local/bin/openocd



ifneq (,$(findstring waf/bootloader, $(BUILD_STRING))$(findstring wwd/, $(BUILD_STRING)))
NO_BUILD_BOOTLOADER:=1
NO_BOOTLOADER_REQUIRED:=1
endif

ifneq ($(NO_BUILD_BOOTLOADER),1)
bootloader:
	$(QUIET)$(ECHO) Building Bootloader
	$(QUIET)$(MAKE) -r -f $(SOURCE_ROOT)Makefile $(BOOTLOADER_TARGET) -I$(OUTPUT_DIR)  SFLASH= EXTERNAL_WICED_GLOBAL_DEFINES=$(EXTERNAL_WICED_GLOBAL_DEFINES) SUB_BUILD=bootloader $(BOOTLOADER_REDIRECT)
	$(QUIET)$(ECHO) Finished Building Bootloader
	$(QUIET)$(ECHO_BLANK_LINE)

download_bootloader: bootloader display_map_summary
	$(QUIET)$(ECHO) Downloading Bootloader ...
	$(QUIET)$(call CONV_SLASHES,$(OPENOCD_FULL_NAME)) -f $(OPENOCD_PATH)$(JTAG).cfg -f $(OPENOCD_PATH)$(HOST_OPENOCD).cfg -f $(OPENOCD_PATH)$(HOST_OPENOCD)-flash-app.cfg -c "verify_image_checksum $(BOOTLOADER_OUTFILE).stripped.elf" -c shutdown $(DOWNLOAD_LOG) 2>&1 && $(ECHO) No changes detected && $(ECHO_BLANK_LINE) || $(call CONV_SLASHES,$(OPENOCD_FULL_NAME)) -f $(OPENOCD_PATH)$(JTAG).cfg -f $(OPENOCD_PATH)$(HOST_OPENOCD).cfg -f $(OPENOCD_PATH)$(HOST_OPENOCD)-flash-app.cfg -c "init" -c "halt" -c "flash write_image erase $(BOOTLOADER_OUTFILE).stripped.elf" -c shutdown $(DOWNLOAD_LOG) 2>&1 && $(ECHO) Download complete && $(ECHO_BLANK_LINE) || $(ECHO) "**** OpenOCD failed - ensure you have installed the driver from the drivers directory, and that the debugger is not running **** In Linux this may be due to USB access permissions. In a virtual machine it may be due to USB passthrough settings. Check in the task list that another OpenOCD process is not running. Check that you have the correct target and JTAG device plugged in. **** $(BOOTLOADER_OUTFILE).stripped.elf"

copy_bootloader_output_for_eclipse: build_done
	$(QUIET)$(call MKDIR, $(BUILD_DIR)/eclipse_debug/)
	$(QUIET)$(CP) $(BOOTLOADER_OUTFILE).elf $(BUILD_DIR)/eclipse_debug/last_bootloader.elf


else
ifeq (1,$(NO_BOOTLOADER_REQUIRED))
bootloader:
	@:

else
bootloader:
	$(QUIET)$(ECHO) Skipping building bootloader due to commandline spec

endif

download_bootloader:
	@:

copy_bootloader_output_for_eclipse:
	@:

endif



ifneq (no_dct,$(findstring no_dct,$(MAKECMDGOALS)))
download_dct: $(STRIPPED_LINK_DCT_FILE) display_map_summary download_bootloader
	$(QUIET)$(ECHO) Downloading DCT ...
	$(QUIET)$(call CONV_SLASHES,$(OPENOCD_FULL_NAME)) -f $(OPENOCD_PATH)$(JTAG).cfg -f $(OPENOCD_PATH)$(HOST_OPENOCD).cfg -f $(OPENOCD_PATH)$(HOST_OPENOCD)-flash-app.cfg -c "verify_image_checksum $(STRIPPED_LINK_DCT_FILE)" -c shutdown $(DOWNLOAD_LOG) 2>&1 && $(ECHO) No changes detected && $(ECHO_BLANK_LINE) || $(call CONV_SLASHES,$(OPENOCD_FULL_NAME)) -f $(OPENOCD_PATH)$(JTAG).cfg -f $(OPENOCD_PATH)$(HOST_OPENOCD).cfg -f $(OPENOCD_PATH)$(HOST_OPENOCD)-flash-app.cfg -c "init" -c "halt" -c "flash write_image erase $(STRIPPED_LINK_DCT_FILE)" -c shutdown $(DOWNLOAD_LOG) 2>&1 && $(ECHO) Download complete && $(ECHO_BLANK_LINE) || $(ECHO) "**** OpenOCD failed - ensure you have installed the driver from the drivers directory, and that the debugger is not running **** In Linux this may be due to USB access permissions. In a virtual machine it may be due to USB passthrough settings. Check in the task list that another OpenOCD process is not running. Check that you have the correct target and JTAG device plugged in. **** $(STRIPPED_LINK_DCT_FILE)"

else
download_dct:
	@:

no_dct:
	$(QUIET)$(ECHO) DCT unmodified

endif


download: $(STRIPPED_LINK_OUTPUT_FILE) display_map_summary download_bootloader $(if $(findstring no_dct,$(MAKECMDGOALS)),,download_dct)
	$(QUIET)$(ECHO) Downloading Application ...
	$(OPENOCD_FULL_NAME) -f $(OPENOCD_PATH)$(JTAG).cfg -f $(OPENOCD_PATH)$(HOST_OPENOCD).cfg -f $(OPENOCD_PATH)$(HOST_OPENOCD)-flash-app.cfg -c "init" -c "halt" -c "flash write_image erase $(STRIPPED_LINK_OUTPUT_FILE)" -c shutdown


run: $(SHOULD_I_WAIT_FOR_DOWNLOAD)
	$(QUIET)$(ECHO) Resetting target
	$(QUIET)$(call CONV_SLASHES,$(OPENOCD_FULL_NAME)) -c "log_output $(OPENOCD_LOG_FILE)" -f $(OPENOCD_PATH)$(JTAG).cfg -f $(OPENOCD_PATH)$(HOST_OPENOCD).cfg -c init -c "reset run" -c shutdown $(DOWNLOAD_LOG) 2>&1 && $(ECHO) Target running


copy_output_for_eclipse: build_done copy_bootloader_output_for_eclipse
	$(QUIET)$(call MKDIR, $(BUILD_DIR)/eclipse_debug/)
	$(QUIET)$(CP) $(LINK_OUTPUT_FILE) $(BUILD_DIR)/eclipse_debug/last_built.elf



debug: $(BUILD_STRING) $(SHOULD_I_WAIT_FOR_DOWNLOAD)
	$(QUIET)$(GDB_COMMAND) $(LINK_OUTPUT_FILE) -x .gdbinit_attach


$(GENERATED_MAC_FILE): $(MAC_GENERATOR)
	$(QUIET)$(PERL) $<  > $@


PLATFORM_PRE_BUILD_TARGETS  := $(GENERATED_MAC_FILE) bootloader
PLATFORM_POST_BUILD_TARGETS := 

