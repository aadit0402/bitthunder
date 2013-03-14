/**
 *	SD Host Controller Interface Driver Implementation for BitThunder.
 *
 *	This allows commands to be sent and received, as well as large data blocks
 *	to be transferred.
 *
 *	This is a generic layer, on-top of which the SD-Card driver sits, but
 *	its possible for a Bluetooth/Wifi/GPS drivers to sit above this, to
 *	provide support for such cards.
 **/

#include <bitthunder.h>
#include "sdhci.h"
#include "../core.h"
#include "../host.h"

BT_DEF_MODULE_NAME				("Generic SDHCI Controller Driver")
BT_DEF_MODULE_DESCRIPTION		("Implements a driver for SDCARD abstraction for SDHCI controllers")
BT_DEF_MODULE_AUTHOR			("James Walmsley")
BT_DEF_MODULE_EMAIL				("james@fullfat-fs.co.uk")

struct _BT_OPAQUE_HANDLE {
	BT_HANDLE_HEADER 			h;
	const BT_INTEGRATED_DEVICE *pDevice;
	SDHCI_REGS 		   		   *pRegs;
	BT_MMC_CARD_EVENTRECEIVER 	pfnEventReceiver;
	MMC_HOST				   *pHost;
	BT_MMC_HOST_OPS			   *pHostOps;
};

static const BT_IF_HANDLE oHandleInterface;

static BT_ERROR sdhci_irq_handler(BT_u32 ulIRQ, void *pParam) {

	BT_HANDLE hSDHCI = (BT_HANDLE) pParam;
	if(!hSDHCI) {
		return -1;
	}

	if(hSDHCI->pRegs->NORMAL_INT_STATUS & NORMAL_INT_CARD_INSERTED) {
		// Signal to SDCARD driver that we have inserted a card,
		// and the card can be initialised.
		hSDHCI->pRegs->NORMAL_INT_STATUS |= NORMAL_INT_CARD_INSERTED;

		if(hSDHCI->pRegs->PRESENT_STATE & STATE_CARD_INSERTED) {

			// Enable the SDHCI clock.
			hSDHCI->pRegs->CLOCK_CONTROL |=  CLOCK_INTERNAL_ENABLE;
			//
			// On insertion, the SD controllers clock is enabled!
			//
			// This does not guarantee that the clock is stable, this must be determined before
			// card-initialisation begins.
			//

			if(hSDHCI->pfnEventReceiver) {
				hSDHCI->pfnEventReceiver(hSDHCI->pHost, BT_MMC_CARD_DETECTED, BT_TRUE);
			}
		}
	}

	if(hSDHCI->pRegs->NORMAL_INT_STATUS & NORMAL_INT_CARD_REMOVED) {
		// Signal to SDCARD driver that we have removed a card.
		// and all data can be flagged for cleanup.
		hSDHCI->pRegs->NORMAL_INT_STATUS |= NORMAL_INT_CARD_REMOVED;

		if(!(hSDHCI->pRegs->PRESENT_STATE & STATE_CARD_INSERTED)) {

			// Disable the SDHCI clock.
			hSDHCI->pRegs->CLOCK_CONTROL &= ~CLOCK_INTERNAL_ENABLE;

			if(hSDHCI->pfnEventReceiver) {
				hSDHCI->pfnEventReceiver(hSDHCI->pHost, BT_MMC_CARD_REMOVED, BT_TRUE);
			}
		}
	}

	return BT_ERR_NONE;
}

static BT_ERROR sdhci_cleanup(BT_HANDLE hSDIO) {
	const BT_RESOURCE *pResource = BT_GetIntegratedResource(hSDIO->pDevice, BT_RESOURCE_IRQ, 0);
	if(pResource) {
		BT_DisableInterrupt(pResource->ulStart);
		BT_UnregisterInterrupt(pResource->ulStart, sdhci_irq_handler, hSDIO);
	}

	BT_kFree(hSDIO);

	return BT_ERR_NONE;
}

static void sdhci_enable_clock(BT_HANDLE hSDIO) {
	hSDIO->pRegs->CLOCK_CONTROL |= 4;
}

static void sdhci_disable_clock(BT_HANDLE hSDIO) {
	hSDIO->pRegs->CLOCK_CONTROL &= ~4;
}

static BT_ERROR sdhci_request(BT_HANDLE hSDIO, MMC_COMMAND *pCommand) {

	// Wait until the the command is not inhibited.
	while(hSDIO->pRegs->PRESENT_STATE & STATE_COMMAND_INHIBIT_CMD) {
		;
	}

	hSDIO->pRegs->ARGUMENT = pCommand->arg;

	BT_u16 cmd_reg = pCommand->opcode << 8;
	if(pCommand->bCRC) {
		cmd_reg |= 0x8;
	}

	if(pCommand->ulResponseType == 48) {
		cmd_reg |= 0x2;
	}

	if(pCommand->ulResponseType == 136) {
		cmd_reg |= 0x1;
	}

	hSDIO->pRegs->COMMAND = cmd_reg;

	while(!(hSDIO->pRegs->NORMAL_INT_STATUS & NORMAL_INT_COMMAND_COMPLETE)) {
		BT_GpioSet(7, BT_TRUE);
	}

	hSDIO->pRegs->NORMAL_INT_STATUS |= NORMAL_INT_COMMAND_COMPLETE;

	// Wait until command is complete.
	pCommand->response[0] = hSDIO->pRegs->RESPONSE[0] | (hSDIO->pRegs->RESPONSE[1] << 16);
	pCommand->response[1] = hSDIO->pRegs->RESPONSE[2] | (hSDIO->pRegs->RESPONSE[3] << 16);
	pCommand->response[2] = hSDIO->pRegs->RESPONSE[4] | (hSDIO->pRegs->RESPONSE[5] << 16);
	pCommand->response[3] = hSDIO->pRegs->RESPONSE[6] | (hSDIO->pRegs->RESPONSE[7] << 16);

	return BT_ERR_NONE;
}


static BT_ERROR sdhci_event_subscribe(BT_HANDLE hSDIO, BT_MMC_CARD_EVENTRECEIVER pfnReceiver, MMC_HOST *pHost) {

	hSDIO->pfnEventReceiver = pfnReceiver;
	hSDIO->pHost 			= pHost;

	// Test for card presence, and generate detection signal.

	if(hSDIO->pRegs->PRESENT_STATE & STATE_CARD_INSERTED) {
		if(hSDIO->pfnEventReceiver) {
			hSDIO->pfnEventReceiver(hSDIO->pHost, BT_MMC_CARD_DETECTED, BT_FALSE);
		}
	}

	return BT_ERR_NONE;
}

static BT_BOOL sdhci_is_card_present(BT_HANDLE hSDIO, BT_ERROR *pError) {
	return (hSDIO->pRegs->PRESENT_STATE & STATE_CARD_INSERTED);
}

static BT_ERROR sdhci_reset(BT_HANDLE hSDIO, BT_u8 ucMask) {
	hSDIO->pRegs->SOFTWARE_RESET = ucMask;

	// Wait for reset to complete

	while(hSDIO->pRegs->SOFTWARE_RESET & ucMask) {
		BT_ThreadSleep(1);
	}

	hSDIO->pRegs->AUTO_CMD12_ERROR = 0;	// Clear Auto CMD12 status.

	// Enable the Card detection IRQs.
	hSDIO->pRegs->NORMAL_INT_ENABLE 		|=	NORMAL_INT_CARD_INSERTED
												| 	NORMAL_INT_CARD_REMOVED
												| 	NORMAL_INT_COMMAND_COMPLETE;

	// Ensure we don't have any card-detection interrupts on init,
	// For some reason this can send the interrupt controller wild, even though
	// we have already registered our IRQ!

	hSDIO->pRegs->NORMAL_INT_STATUS		|= NORMAL_INT_CARD_INSERTED | NORMAL_INT_CARD_REMOVED;

	// Enable the interrupts to be signalled to the CPU.
	hSDIO->pRegs->NORMAL_INT_SIGNAL_ENABLE 	|= NORMAL_INT_CARD_INSERTED | NORMAL_INT_CARD_REMOVED;

	return BT_ERR_NONE;
}

static BT_ERROR sdhci_initialise(BT_HANDLE hSDIO) {

	// Completely reset the SDIO hardware.
	sdhci_reset(hSDIO, RESET_ALL);

	BT_ThreadSleep(25);	// Sometimes card is not really debounced in some hardware.

	if(!sdhci_is_card_present(hSDIO, NULL)) {
		return BT_ERR_GENERIC;
	}

	// Enable the SDIO clock, and attempt to reset the card if present.
	BT_u32 reg = hSDIO->pRegs->CLOCK_CONTROL;
	reg &= 0x00FF;	// Mask out the clock selection freq!
	reg |= (128 << 8);	// Set SDClk to be base (50mhz / 128 ~400khz).
	reg |= 1;			// Enable internal SDHCI clock.
	hSDIO->pRegs->CLOCK_CONTROL = reg;

	while(!(hSDIO->pRegs->CLOCK_CONTROL & 2)) {	// Spin until the clock is stable.
		;
	}

	hSDIO->pRegs->TIMEOUT_CONTROL = 0x7;	// Timeout at TMCLK 2 ^ 20;

	BT_u32 caps = hSDIO->pRegs->CAPABILITIES;
	if(caps & CAPS_1_8V) {
		POWER_SELECT_SET(hSDIO->pRegs->POWER_CONTROL, POWER_SELECT_1_8V);
	}

	if(caps & CAPS_3_0V) {
		POWER_SELECT_SET(hSDIO->pRegs->POWER_CONTROL, POWER_SELECT_3_0V);
	}

	if(caps & CAPS_3_3V) {
		POWER_SELECT_SET(hSDIO->pRegs->POWER_CONTROL, POWER_SELECT_3_3V);
	}

	// Enable bus power!
	POWER_ENABLE_SET(hSDIO->pRegs->POWER_CONTROL, 1);

	sdhci_enable_clock(hSDIO);	// Enable the clock to allow the SDCard to initialise.

	BT_ThreadSleep(10);			// Need to provide atlease 72 clocks, we'll just delay a really long time.



	// Enable the Card detection IRQs.
	hSDIO->pRegs->NORMAL_INT_ENABLE 		|=	NORMAL_INT_CARD_INSERTED
											| 	NORMAL_INT_CARD_REMOVED
											| 	NORMAL_INT_COMMAND_COMPLETE;

	// Ensure we don't have any card-detection interrupts on init,
	// For some reason this can send the interrupt controller wild, even though
	// we have already registered our IRQ!

	hSDIO->pRegs->NORMAL_INT_STATUS		|= NORMAL_INT_CARD_INSERTED | NORMAL_INT_CARD_REMOVED;

	// Enable the interrupts to be signalled to the CPU.
	hSDIO->pRegs->NORMAL_INT_SIGNAL_ENABLE 	|= NORMAL_INT_CARD_INSERTED | NORMAL_INT_CARD_REMOVED;

	return BT_ERR_NONE;
}

static const BT_MMC_OPS sdhci_mmc_ops = {
	.ulCapabilites1 	= 0,
	.pfnRequest			= sdhci_request,
	.pfnEventSubscribe 	= sdhci_event_subscribe,
	.pfnIsCardPresent	= sdhci_is_card_present,
	.pfnInitialise 		= sdhci_initialise,
};

static BT_HANDLE sdhci_probe(const BT_INTEGRATED_DEVICE *pDevice, BT_ERROR *pError) {

	BT_ERROR Error;

	BT_HANDLE hSDIO = BT_CreateHandle(&oHandleInterface, sizeof(struct _BT_OPAQUE_HANDLE), pError);
	if(!hSDIO) {
		goto err_out;
	}

	hSDIO->pDevice = pDevice;	// Provide access to parameters on cleanup.

	const BT_RESOURCE *pResource = BT_GetIntegratedResource(pDevice, BT_RESOURCE_MEM, 0);
	if(!pResource) {
		Error = BT_ERR_GENERIC;
		goto err_free_out;
	}

	hSDIO->pRegs = (SDHCI_REGS *) pResource->ulStart;

	pResource = BT_GetIntegratedResource(pDevice, BT_RESOURCE_IRQ, 0);
	if(!pResource) {
		Error = BT_ERR_GENERIC;
		goto err_free_out;
	}

	BT_u32 ulIRQ = pResource->ulStart;

	Error = BT_RegisterInterrupt(ulIRQ, sdhci_irq_handler, hSDIO);
	if(Error) {
		goto err_free_out;
	}

	// This enables the interrupt at with the interrupt controller.
	Error = BT_EnableInterrupt(ulIRQ);
	if(Error) {
		goto err_free_irq;
	}

	sdhci_reset(hSDIO, RESET_ALL);

	BT_u32 hc_version 	= hSDIO->pRegs->SLOT_INT_STAT_HCVERSION;
	BT_u32 vendor 		= HCVERSION_VENDOR_GET(hc_version);
	BT_u32 sdversion 	= HCVERSION_SPECV_GET(hc_version);

	// We could check controller version to ensure compatibility, but
	// in all likely-hood (clutching at straws) newer versions would remain mostly
	// compatible.

	pResource = BT_GetIntegratedResource(pDevice, BT_RESOURCE_PARAM, 0);
	if(!pResource) {
		Error = BT_ERR_GENERIC;
		goto err_free_irq;
	}

	hSDIO->pHostOps = (BT_MMC_HOST_OPS *) pResource->pParam;

	// Register with the SD Host controller.
	Error = BT_RegisterSDHostController(hSDIO, &sdhci_mmc_ops);
	if(Error) {
		goto err_free_irq;
	}

	return hSDIO;

err_free_irq:
	BT_UnregisterInterrupt(ulIRQ, sdhci_irq_handler, hSDIO);

err_free_out:
	BT_kFree(hSDIO);

	if(pError) {
		*pError = Error;
	}

err_out:
	return NULL;
}

static const BT_IF_HANDLE oHandleInterface = {
	BT_MODULE_DEF_INFO,
	.pfnCleanup = sdhci_cleanup,
	/*.oIfs = {
		.pDevIF = &oDeviceInterface,
		},*/
};

BT_INTEGRATED_DRIVER_DEF sdhci_driver = {
	.name		= "mmc,sdhci",
	.pfnProbe	= sdhci_probe,
};
