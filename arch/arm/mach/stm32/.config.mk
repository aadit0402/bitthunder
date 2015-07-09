BT_CONFIG_ARCH_ARM_CORTEX-M3=y
BT_CONFIG_ARCH_ARM_USE_NVIC=y

#
#	Configure the NVIC parameters for this platform
#
#BT_CONFIG_ARCH_ARM_NVIC_BASE	// Use default
BT_CONFIG_ARCH_ARM_NVIC_TOTAL_IRQS=56

ifeq ($(BT_CONFIG_DRIVERS_SDCARD), y)
BT_CONFIG_DRIVERS_SDCARD_HOST_SDHCI=y
endif

BT_CONFIG_FREERTOS_PORT_ARCH=BT_CONFIG_FREERTOS_M3
