#
#	BitThunder OS
#

#
#	If we are building a fully fledged OS, then include the real memory manager.
#
include $(BASE)os/.config.mk

OBJECTS-$(BT_CONFIG_OS) += $(BUILD_DIR)os/src/mm/bt_mm.o

SUB_OBJDIRS += $(BASE)arch/$(ARCH)/mach/$(SUBARCH)/
#OBJECTS-$(BT_CONFIG_OS) += $(BUILD_DIR)arch/$(ARCH)/mach/$(SUBARCH)/$(SUBARCH).o
