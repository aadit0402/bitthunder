/**
 *	LPC17xx Hal for BitThunder
 *	UART Driver Implementation.
 *
 *	This driver serves as robust example as to how implement a fully functional UART device driver
 *	for BitThunder.
 *
 *	This driver should be easily ported to UART peripherals on other processors with little effort.
 *
 *	@author		Robert Steinbauer <rsteinbauer@riegl.com>
 *	@copyright	(c)2012 Robert Steinbauer
 *	@copyright	(c)2012 Riegl Laser Measurement Systems GmbH
 *
 **/
#include <bitthunder.h>	 				// Include all those wonderful BitThunder APIs.
#include "uart.h"						// Includes a full hardware definition for the integrated uarts.
#include "rcc.h"						// Used for getting access to rcc regs, to determine the real Clock Freq.
#include "../../common/cortex/systick.h"						// Used for getting access to rcc regs, to determine the real Clock Freq.
#include "ioconfig.h"						// Used for getting access to IOCON regs.
#include <collections/bt_fifo.h>

/**
 *	All driver modules in the system shall be tagged with some helpful information.
 *	This way we know who to blame when things go wrong!
 **/
BT_DEF_MODULE_NAME						("LPC17xx-USART")
BT_DEF_MODULE_DESCRIPTION				("Simple Uart device for the LPC17xx Embedded Platform")
BT_DEF_MODULE_AUTHOR					("Robert Steinbauer")
BT_DEF_MODULE_EMAIL						("rsteinbauer@riegl.com")

/**
 *	We can define how a handle should look in a UART driver, probably we only need a
 *	hardware-ID number. (Remember, try to keep HANDLES as low-cost as possible).
 **/
struct _BT_OPAQUE_HANDLE {
	BT_HANDLE_HEADER 		h;			///< All handles must include a handle header.
	LPC17xx_UART_REGS	   *pRegs;
	const BT_INTEGRATED_DEVICE   *pDevice;
	BT_UART_OPERATING_MODE	eMode;		///< Operational mode, i.e. buffered/polling mode.
	BT_u32					id;
	BT_HANDLE		   		hRxFifo;		///< RX fifo - ring buffer.
	BT_HANDLE		   		hTxFifo;		///< TX fifo - ring buffer.
};

static BT_HANDLE g_USART_HANDLES[4] = {
	NULL,
	NULL,
	NULL,
	NULL,
};

static const BT_u32 g_USART_PERIPHERAL[4] = {3, 4, 24, 25};

static BT_u8 TX_FIFO_LVL[4] = {0, 0, 0, 0};

static const BT_IF_HANDLE oHandleInterface;	// Protoype for the uartOpen function.
static void disableUartPeripheralClock(BT_HANDLE hUart);


static void usartRxHandler(BT_HANDLE hUart) {
	volatile LPC17xx_UART_REGS *pRegs = hUart->pRegs;
	BT_u8 ucData;

	BT_ERROR Error = BT_ERR_NONE;

	/* Receive Line Status */
	if (pRegs->LSR & (LPC17xx_UART_LSR_OE | LPC17xx_UART_LSR_PE | LPC17xx_UART_LSR_FE | LPC17xx_UART_LSR_RXFE | LPC17xx_UART_LSR_BI))
    {
		/* There are errors or break interrupt */
		/* Read LSR will clear the interrupt */
		pRegs->FIFO;	/* Dummy read on RX to clear interrupt, then bail out */
		return;
    }

	while (pRegs->LSR & LPC17xx_UART_LSR_RDR)
	{
		ucData = pRegs->FIFO & 0xFF;
		BT_FifoWriteFromISR(hUart->hRxFifo, 1, &ucData);
	}
}


static void usartTxHandler(BT_HANDLE hUart) {
	volatile LPC17xx_UART_REGS *pRegs = hUart->pRegs;
	BT_u8 ucData;

	BT_ERROR Error = BT_ERR_NONE;

	TX_FIFO_LVL[hUart->id] = 0;

	while (!BT_FifoIsEmpty(hUart->hTxFifo, &Error) && (TX_FIFO_LVL[hUart->id] < 16)) {
		BT_FifoReadFromISR(hUart->hTxFifo, 1, &ucData);
		pRegs->FIFO = ucData;
		TX_FIFO_LVL[hUart->id]++;
	}
	if (BT_FifoIsEmpty(hUart->hTxFifo, &Error)) {
		pRegs->IER &= ~LPC17xx_UART_IER_THREIE;	// Disable the interrupt
	}
}

BT_ERROR BT_NVIC_IRQ_21(void) {
	BT_u32 IIRValue = UART0->IIR;

	if(IIRValue & (LPC17xx_UART_IIR_RDA_INT | LPC17xx_UART_IIR_RLS_INT)) {
		usartRxHandler(g_USART_HANDLES[0]);
	}
	if(IIRValue & LPC17xx_UART_IIR_THRE_INT) {
		usartTxHandler(g_USART_HANDLES[0]);
	}
	return 0;
}

BT_ERROR BT_NVIC_IRQ_22(void) {
	BT_u32 IIRValue = UART1->IIR;

	if(IIRValue & (LPC17xx_UART_IIR_RDA_INT | LPC17xx_UART_IIR_RLS_INT)) {
		usartRxHandler(g_USART_HANDLES[1]);
	}
	if(IIRValue & LPC17xx_UART_IIR_THRE_INT) {
		usartTxHandler(g_USART_HANDLES[1]);
	}
	return 0;
}
BT_ERROR BT_NVIC_IRQ_23(void) {
	BT_u32 IIRValue = UART2->IIR;

	if(IIRValue & (LPC17xx_UART_IIR_RDA_INT | LPC17xx_UART_IIR_RLS_INT)) {
		usartRxHandler(g_USART_HANDLES[2]);
	}
	if(IIRValue & LPC17xx_UART_IIR_THRE_INT) {
		usartTxHandler(g_USART_HANDLES[2]);
	}
	return 0;
}

BT_ERROR BT_NVIC_IRQ_24(void) {
	BT_u32 IIRValue = UART3->IIR;

	if(IIRValue & (LPC17xx_UART_IIR_RDA_INT | LPC17xx_UART_IIR_RLS_INT)) {
		usartRxHandler(g_USART_HANDLES[3]);
	}
	if(IIRValue & LPC17xx_UART_IIR_THRE_INT) {
		usartTxHandler(g_USART_HANDLES[3]);
	}
	return 0;
}


static BT_ERROR uartDisable(BT_HANDLE hUart);
static BT_ERROR uartEnable(BT_HANDLE hUart);


static void ResetUart(BT_HANDLE hUart)
{
	volatile LPC17xx_UART_REGS *pRegs = hUart->pRegs;

	pRegs->LCR 		|= LPC17xx_UART_LCR_DLAB;
	pRegs->DLL		= 0;
	pRegs->DLM		= 0;
	pRegs->LCR 		&= ~LPC17xx_UART_LCR_DLAB;
	pRegs->IER		= 0;
	pRegs->FCR		= 0;
	pRegs->LCR 		= 0;
	pRegs->MCR 		= 0;
	pRegs->MSR		= 0;
	pRegs->SCR   	= 0;
	pRegs->ABCR		= 0;
	pRegs->FDR 		= 0x00000010;
	pRegs->TER		= 0x00000080;
	pRegs->RS485CR 	= 0;
	pRegs->RS485AMR = 0;
	pRegs->RS485DLY	= 0;

	const BT_RESOURCE *pResource = BT_GetIntegratedResource(hUart->pDevice, BT_RESOURCE_ENUM, 0);

	TX_FIFO_LVL[pResource->ulStart] = 0;
}

/**
 *	All modules MUST provide a FULL cleanup function. This must cleanup all allocations etc
 *	e.g. we will have to populate this when we add circular buffers!
 *
 **/
static BT_ERROR uartCleanup(BT_HANDLE hUart) {
	ResetUart(hUart);

	// Disable peripheral clock.
	disableUartPeripheralClock(hUart);

	// Free any buffers if used.
	if(hUart->eMode == BT_UART_MODE_BUFFERED) {
		BT_CloseHandle(hUart->hTxFifo);
		BT_CloseHandle(hUart->hRxFifo);
	}

	const BT_RESOURCE *pResource = BT_GetIntegratedResource(hUart->pDevice, BT_RESOURCE_IRQ, 0);

	BT_DisableInterrupt(pResource->ulStart);

	pResource = BT_GetIntegratedResource(hUart->pDevice, BT_RESOURCE_ENUM, 0);

	g_USART_HANDLES[pResource->ulStart] = NULL;	// Finally mark the hardware as not used.

	return BT_ERR_NONE;
}

#define MAX_BAUD_ERROR_RATE	3	/* max % error allowed */

static BT_ERROR uartSetBaudrate(BT_HANDLE hUart, BT_u32 ulBaudrate) {
	volatile LPC17xx_UART_REGS *pRegs = hUart->pRegs;

	BT_u16	IterBAUDDIV;
	BT_u8	MulVal;
	BT_u8	DivAddVal;
	BT_u32	Divider_Value;
	BT_u32	CalcBaudRate;
	BT_u32	BaudError;
	BT_u32	Best_Fractional = 0x10;
	BT_u8	Best_BAUDDIV = 0;
	BT_u32	Best_Error = 0xFFFFFFFF;
	BT_u32	PercentError;
	BT_u32	InputClk;
	BT_u32	BaudRate = ulBaudrate;

	/*
	 *	We must determine the input clock frequency to the UART peripheral.
	 */

	const BT_RESOURCE *pResource = BT_GetIntegratedResource(hUart->pDevice, BT_RESOURCE_ENUM, 0);

	InputClk = BT_LPC17xx_GetPeripheralClock(g_USART_PERIPHERAL[pResource->ulStart]);

	/*
	 * Determine the Baud divider. It can be 4to 254.
	 * Loop through all possible combinations
	 */

	/*
	 * Calculate the exact divider incl. fractional part
	 */
	Divider_Value = InputClk / (16 * ulBaudrate);

	for (IterBAUDDIV = Divider_Value/2; IterBAUDDIV <= Divider_Value; IterBAUDDIV++)
	{
		for (MulVal = 1; MulVal < 16; MulVal++)
		{
			for (DivAddVal = 0; DivAddVal < MulVal; DivAddVal++)
			{
				/*
				 * Calculate the baud rate from the BRGR value
				 */
				CalcBaudRate = ((InputClk * MulVal) / (16 * IterBAUDDIV * (MulVal + DivAddVal)));

				/*
				 * Avoid unsigned integer underflow
				 */
				if (BaudRate > CalcBaudRate) {
					BaudError = BaudRate - CalcBaudRate;
				} else {
					BaudError = CalcBaudRate - BaudRate;
				}

				/*
				 * Find the calculated baud rate closest to requested baud rate.
				 */
				if (Best_Error > BaudError)
				{
					Best_Fractional = (MulVal << 4) + DivAddVal;
					Best_BAUDDIV = IterBAUDDIV;
					Best_Error = BaudError;
				}
			}
		}
	}

	PercentError = (Best_Error * 100) / BaudRate;
	if (MAX_BAUD_ERROR_RATE < PercentError) {
		return -1;
	}

	uartDisable(hUart);

	pRegs->LCR |= LPC17xx_UART_LCR_DLAB;             // Enable DLAB bit for setting Baudrate divider
	pRegs->DLM = Best_BAUDDIV / 256;
	pRegs->DLL = Best_BAUDDIV % 256;
	pRegs->LCR &= ~LPC17xx_UART_LCR_DLAB;		// DLAB = 0
	pRegs->FDR = Best_Fractional;

	uartEnable(hUart);
	return BT_ERR_NONE;
}

/**
 *	This actually allows the UARTS to be clocked!
 **/
static void enableUartPeripheralClock(BT_HANDLE hUart) {
	const BT_RESOURCE *pResource = BT_GetIntegratedResource(hUart->pDevice, BT_RESOURCE_ENUM, 0);

	switch(pResource->ulStart) {
	case 0: {
		LPC17xx_RCC->PCONP |= LPC17xx_RCC_PCONP_UART0EN;
		break;
	}
	case 1: {
		LPC17xx_RCC->PCONP |= LPC17xx_RCC_PCONP_UART1EN;
		break;
	}
	case 2: {
		LPC17xx_RCC->PCONP |= LPC17xx_RCC_PCONP_UART2EN;
		break;
	}
	case 3: {
		LPC17xx_RCC->PCONP |= LPC17xx_RCC_PCONP_UART3EN;
		break;
	}
	default: {
		break;
	}
	}
}

/**
 *	If the serial port is not in use, we can make it sleep!
 **/
static void disableUartPeripheralClock(BT_HANDLE hUart) {
	const BT_RESOURCE *pResource = BT_GetIntegratedResource(hUart->pDevice, BT_RESOURCE_ENUM, 0);

	switch(pResource->ulStart) {
	case 0: {
		LPC17xx_RCC->PCONP &= ~LPC17xx_RCC_PCONP_UART0EN;
		break;
	}
	case 1: {
		LPC17xx_RCC->PCONP &= ~LPC17xx_RCC_PCONP_UART1EN;
		break;
	}
	case 2: {
		LPC17xx_RCC->PCONP &= ~LPC17xx_RCC_PCONP_UART2EN;
		break;
	}
	case 3: {
		LPC17xx_RCC->PCONP &= ~LPC17xx_RCC_PCONP_UART3EN;
		break;
	}
	default: {
		break;
	}
	}
}

/**
 *	Function to test the current peripheral clock gate status of the devices.
 **/
static BT_BOOL isUartPeripheralClockEnabled(BT_HANDLE hUart) {
	const BT_RESOURCE *pResource = BT_GetIntegratedResource(hUart->pDevice, BT_RESOURCE_ENUM, 0);

	switch(pResource->ulStart) {
	case 0: {
		if(LPC17xx_RCC->PCONP & LPC17xx_RCC_PCONP_UART0EN) {
			return BT_TRUE;
		}
		break;
	}
	case 1: {
		if(LPC17xx_RCC->PCONP & LPC17xx_RCC_PCONP_UART1EN) {
			return BT_TRUE;
		}
		break;
	}
	case 2: {
		if(LPC17xx_RCC->PCONP & LPC17xx_RCC_PCONP_UART2EN) {
			return BT_TRUE;
		}
		break;
	}
	case 3: {
		if(LPC17xx_RCC->PCONP & LPC17xx_RCC_PCONP_UART3EN) {
			return BT_TRUE;
		}
		break;
	}
	default: {
		break;
	}
	}

	return BT_FALSE;
}

/**
 *	This implements the UART power management interface.
 *	It is called from the BT_SetPowerState() API!
 **/
static BT_ERROR uartSetPowerState(BT_HANDLE hUart, BT_POWER_STATE ePowerState) {

	switch(ePowerState) {
	case BT_POWER_STATE_ASLEEP: {
		disableUartPeripheralClock(hUart);
		break;
	}
	case BT_POWER_STATE_AWAKE: {
		enableUartPeripheralClock(hUart);
		break;
	}

	default: {
		//return BT_ERR_POWER_STATE_UNSUPPORTED;
		return (BT_ERROR) -1;
	}
	}

	return BT_ERR_NONE;
}

/**
 *	This implements the UART power management interface.
 *	It is called from the BT_GetPowerState() API!
 **/
static BT_ERROR uartGetPowerState(BT_HANDLE hUart, BT_POWER_STATE *pePowerState) {
	if(isUartPeripheralClockEnabled(hUart)) {
		return BT_POWER_STATE_AWAKE;
	}
	return BT_POWER_STATE_ASLEEP;
}

/**
 *	Complete a full configuration of the UART.
 **/
static BT_ERROR uartSetConfig(BT_HANDLE hUart, BT_UART_CONFIG *pConfig) {
	volatile LPC17xx_UART_REGS *pRegs = hUart->pRegs;

	BT_ERROR Error = BT_ERR_NONE;

	pRegs->LCR = (pConfig->ucDataBits - 5);
	pRegs->LCR |= (pConfig->ucStopBits) << 2;
	pRegs->LCR |= (pConfig->ucParity << 3);

	uartSetBaudrate(hUart, pConfig->ulBaudrate);

	switch(pConfig->eMode) {
	case BT_UART_MODE_POLLED: {
		if(hUart->eMode !=  BT_UART_MODE_POLLED) {

			if(hUart->hTxFifo) {
				BT_CloseHandle(hUart->hTxFifo);
				hUart->hTxFifo = NULL;
			}
			if(hUart->hRxFifo) {
				BT_CloseHandle(hUart->hRxFifo);
				hUart->hRxFifo = NULL;
			}

			// Disable TX and RX interrupts
			pRegs->IER &= ~LPC17xx_UART_IER_RBRIE;	// Disable the interrupt

			hUart->eMode = BT_UART_MODE_POLLED;
		}
		break;
	}

	case BT_UART_MODE_BUFFERED:
	{
		if(hUart->eMode != BT_UART_MODE_BUFFERED) {
			if(!hUart->hRxFifo && !hUart->hTxFifo) {
				hUart->hRxFifo = BT_FifoCreate(pConfig->ulRxBufferSize, 1, 0, &Error);
				hUart->hTxFifo = BT_FifoCreate(pConfig->ulTxBufferSize, 1, 0, &Error);

				pRegs->IER |= LPC17xx_UART_IER_RBRIE;	// Enable the interrupt
				hUart->eMode = BT_UART_MODE_BUFFERED;
			}
		}
		break;
	}

	default:
		// Unsupported operating mode!
		break;
	}

	return BT_ERR_NONE;
}

/**
 *	Get a full configuration of the UART.
 **/
static BT_ERROR uartGetConfig(BT_HANDLE hUart, BT_UART_CONFIG *pConfig) {
	volatile LPC17xx_UART_REGS *pRegs = hUart->pRegs;

	pConfig->eMode 			= hUart->eMode;

	BT_ERROR Error = BT_ERR_NONE;

	BT_u32 InputClk;

	const BT_RESOURCE *pResource = BT_GetIntegratedResource(hUart->pDevice, BT_RESOURCE_ENUM, 0);

	InputClk = BT_LPC17xx_GetPeripheralClock(g_USART_PERIPHERAL[pResource->ulStart]);

	pRegs->LCR |= LPC17xx_UART_LCR_DLAB;
	BT_u32 Divider = (pRegs->DLM << 8) + pRegs->DLL;
	pRegs->LCR &= ~LPC17xx_UART_LCR_DLAB;

	BT_u32 MulVal 	 = (pRegs->FDR >> 4);
	BT_u32 DivAddVal = (pRegs->FDR & 0x0F);
	pConfig->ulBaudrate 	= ((InputClk * MulVal) / (16 * Divider * (MulVal + DivAddVal)));		// Clk / Divisor == ~Baudrate
	pConfig->ulTxBufferSize = BT_FifoSize(hUart->hTxFifo);
	pConfig->ulRxBufferSize = BT_FifoSize(hUart->hRxFifo);
	pConfig->ucDataBits 	= (pRegs->LCR & 0x00000003) + 5;
	pConfig->ucStopBits		= ((pRegs->LCR & 0x00000004) >> 2) + 1;
	pConfig->ucParity		= (BT_UART_PARITY_MODE)((pRegs->LCR & 0x00000380) >> 3);
	pConfig->eMode			= hUart->eMode;


	return Error;
}

/**
 *	Make the UART active (Set the Enable bit).
 **/
static BT_ERROR uartEnable(BT_HANDLE hUart) {
	volatile LPC17xx_UART_REGS *pRegs = hUart->pRegs;

	pRegs->TER |= LPC17xx_UART_TER_TXEN;		// Enable TX line.
	pRegs->FCR |= LPC17xx_UART_FCR_FIFO_ENB | LPC17xx_UART_FCR_RX_PURGE | LPC17xx_UART_FCR_TX_PURGE;		// Reset TX and RX FIFO's.

	return BT_ERR_NONE;
}

/**
 *	Make the UART inactive (Clear the Enable bit).
 **/
static BT_ERROR uartDisable(BT_HANDLE hUart) {
	volatile LPC17xx_UART_REGS *pRegs = hUart->pRegs;

	pRegs->TER &= ~LPC17xx_UART_TER_TXEN;
	pRegs->FCR &= ~LPC17xx_UART_FCR_FIFO_ENB;

	return BT_ERR_NONE;
}

static BT_ERROR uartGetAvailable(BT_HANDLE hUart, BT_u32 *pTransmit, BT_u32 *pReceive) {
	switch(hUart->eMode) {
	case BT_UART_MODE_POLLED:
	{
		if(!(hUart->pRegs->LSR & LPC17xx_UART_LSR_RDR)) *pTransmit = 1;
		if((hUart->pRegs->LSR & LPC17xx_UART_LSR_THRE)) *pReceive = 1;
		break;
	}

	case BT_UART_MODE_BUFFERED:
	{
		*pTransmit = BT_FifoGetAvailable(hUart->hTxFifo);
		*pReceive = BT_FifoGetAvailable(hUart->hRxFifo);
		break;
	}

	default:
		return BT_ERR_GENERIC;

	}

	return BT_ERR_NONE;
}

static BT_S32 uartRead(BT_HANDLE hUart, BT_u32 ulFlags, BT_u32 ulSize, BT_u8 *pucDest) {
	volatile LPC17xx_UART_REGS *pRegs = hUart->pRegs;

	BT_ERROR Error = BT_ERR_NONE;
	BT_s32 slRead = 0;

	switch(hUart->eMode) {
	case BT_UART_MODE_POLLED:
	{
		while(ulSize) {
			while((pRegs->LSR & LPC17xx_UART_LSR_RDR)) {
				BT_ThreadYield();
			}

			*pucDest++ = pRegs->FIFO & 0x000000FF;
			ulSize--;
			slRead++;
		}
		break;
	}

	case BT_UART_MODE_BUFFERED:
	{
		// Get bytes from RX buffer very quickly.
		slRead = BT_FifoRead(hUart->hRxFifo, ulSize, pucDest, 0);
		break;
	}

	default:
		// ERR, invalid handle configuration.
		break;
	}

	return slRead;
}

/**
 *	Implementing the CHAR dev write API.
 *
 *	Note, this doesn't implement ulFlags specific options yet!
 **/
static BT_S32 uartWrite(BT_HANDLE hUart, BT_u32 ulFlags, BT_u32 ulSize, const BT_u8 *pucSource) {
	volatile LPC17xx_UART_REGS *pRegs = hUart->pRegs;

	BT_ERROR Error = BT_ERR_NONE;
	BT_u8 ucData;
	BT_u8 *pSrc = (BT_u8*)pucSource;
	BT_s32 slWritten = 0;

	switch(hUart->eMode) {
	case BT_UART_MODE_POLLED:
	{
		while(ulSize) {
			while(!(pRegs->LSR & LPC17xx_UART_LSR_THRE)) {
				BT_ThreadYield();
			}
			pRegs->FIFO = *pucSource++;
			ulSize--;
			slWritten++;
		}
		break;
	}

	case BT_UART_MODE_BUFFERED:
	{
		slWritten = BT_FifoWrite(hUart->hTxFifo, ulSize, pSrc, 0);

		pRegs->IER &= ~LPC17xx_UART_IER_THREIE;	// Disable the interrupt

		while (!BT_FifoIsEmpty(hUart->hTxFifo, &Error) && (TX_FIFO_LVL[hUart->id] < 16)) {
			BT_FifoRead(hUart->hTxFifo, 1, &ucData, 0);
			pRegs->FIFO = ucData;
			TX_FIFO_LVL[hUart->id]++;
		}
		pRegs->IER |= LPC17xx_UART_IER_THREIE;	// Enable the interrupt

		break;
	}

	default:
		break;
	}
	return slWritten;
}

static BT_u32 file_read(BT_HANDLE hUart, BT_u32 ulFlags, BT_u32 ulSize, void *pBuffer) {
	return uartRead(hUart, ulFlags, ulSize, pBuffer);
}

static BT_u32 file_write(BT_HANDLE hUart, BT_u32 ulFlags, BT_u32 ulSize, const void *pBuffer) {
	return uartWrite(hUart, ulFlags, ulSize, pBuffer);
}


static const BT_DEV_IF_UART oUartConfigInterface = {
	.pfnSetBaudrate		= uartSetBaudrate,											///< UART setBaudrate implementation.
	.pfnSetConfig		= uartSetConfig,												///< UART set config imple.
	.pfnGetConfig		= uartGetConfig,
	.pfnEnable			= uartEnable,													///< Enable/disable the device.
	.pfnDisable			= uartDisable,
	.pfnGetAvailable	= uartGetAvailable,
};

static const BT_IF_POWER oPowerInterface = {
	uartSetPowerState,											///< Pointers to the power state API implementations.
	uartGetPowerState,											///< This gets the current power state.
};

static const BT_DEV_IFS oConfigInterface = {
	(BT_DEV_INTERFACE) &oUartConfigInterface,
};

const BT_IF_DEVICE BT_LPC17xx_UART_oDeviceInterface = {
	&oPowerInterface,											///< Device does not support powerstate functionality.
	BT_DEV_IF_T_UART,											///< Allow configuration through the UART api.
	.unConfigIfs = {
		(BT_DEV_INTERFACE) &oUartConfigInterface,
	},
};

static const BT_IF_FILE oFileInterface = {
	.pfnRead = file_read,
	.pfnWrite = file_write,
};

static const BT_IF_HANDLE oHandleInterface = {
	BT_MODULE_DEF_INFO,
	.oIfs = {
		(BT_HANDLE_INTERFACE) &BT_LPC17xx_UART_oDeviceInterface,
	},
	.pFileIF 	= &oFileInterface,
	.eType 		= BT_HANDLE_T_DEVICE,											///< Handle Type!
	.pfnCleanup = uartCleanup,												///< Handle's cleanup routine.
};

static BT_HANDLE uart_probe(const BT_INTEGRATED_DEVICE *pDevice, BT_ERROR *pError) {

	BT_ERROR Error = BT_ERR_NONE;
	BT_HANDLE hUart = NULL;

	const BT_RESOURCE *pResource = BT_GetIntegratedResource(pDevice, BT_RESOURCE_ENUM, 0);
	if(!pResource) {
		Error = BT_ERR_NO_MEMORY;
		goto err_free_out;
	}

	if (g_USART_HANDLES[pResource->ulStart]){
		Error = BT_ERR_GENERIC;
		goto err_free_out;
	}

	hUart = BT_CreateHandle(&oHandleInterface, sizeof(struct _BT_OPAQUE_HANDLE), pError);
	if(!hUart) {
		goto err_out;
	}

	hUart->id = pResource->ulStart;

	g_USART_HANDLES[pResource->ulStart] = hUart;

	hUart->pDevice = pDevice;

	pResource = BT_GetIntegratedResource(pDevice, BT_RESOURCE_MEM, 0);
	if(!pResource) {
		Error = BT_ERR_NO_MEMORY;
		goto err_free_out;
	}

	hUart->pRegs = (LPC17xx_UART_REGS *) pResource->ulStart;

	uartSetPowerState(hUart, BT_POWER_STATE_AWAKE);

	// Reset all registers to their defaults!

	ResetUart(hUart);

	pResource = BT_GetIntegratedResource(pDevice, BT_RESOURCE_IRQ, 0);
	if(!pResource) {
		Error = BT_ERR_GENERIC;
		goto err_free_out;
	}

	BT_SetInterruptPriority(pResource->ulStart, 2);

/*	On NVIC we don't need to register interrupts, LINKER has patched vector for us
 * Error = BT_RegisterInterrupt(pResource->ulStart, uart_irq_handler, hUart);
	if(Error) {
		goto err_free_out;
	}*/


	Error = BT_EnableInterrupt(pResource->ulStart);

	return hUart;

err_free_out:
	BT_DestroyHandle(hUart);

err_out:
	if(pError) {
		*pError = Error;
	}
	return NULL;
}

BT_INTEGRATED_DRIVER_DEF uart_driver = {
	.name 		= "LPC17xx,usart",
	.pfnProbe	= uart_probe,
};
