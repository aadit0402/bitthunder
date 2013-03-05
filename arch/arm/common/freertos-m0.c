/**
 *	Copyright (c) 2013 James Walmsley
 *
 *
 **/

#include <bitthunder.h>
#include "bt_types.h"

#include <FreeRTOS.h>
#include <task.h>
#include <portmacro.h>

struct _BT_OPAQUE_HANDLE {
	BT_HANDLE_HEADER h;
};

/* For backward compatibility, ensure configKERNEL_INTERRUPT_PRIORITY is
 defined.  The value should also ensure backward compatibility.
 FreeRTOS.org versions prior to V4.4.0 did not include this definition. */
#ifndef configKERNEL_INTERRUPT_PRIORITY
#define configKERNEL_INTERRUPT_PRIORITY 255
#endif

static BT_ERROR tick_isr_handler(BT_u32 ulIRQ, void *pParam) {
	vTaskIncrementTick();

#if configUSE_PREEMPTION == 1
	vTaskSwitchContext();
#endif

	// Timer interrupt status is handled automatically by BitThunder driver.
	return BT_ERR_NONE;
}

/* Constants required to setup the task context. */
/* System mode, ARM mode, interrupts enabled. */
#define portINITIAL_XPSR			( 0x01000000 )

/* Constants required to manipulate the NVIC. */
#define portNVIC_SYSTICK_CTRL		( ( volatile unsigned long *) 0xe000e010 )
#define portNVIC_SYSTICK_LOAD		( ( volatile unsigned long *) 0xe000e014 )
#define portNVIC_INT_CTRL			( ( volatile unsigned long *) 0xe000ed04 )
#define portNVIC_SYSPRI2			( ( volatile unsigned long *) 0xe000ed20 )
#define portNVIC_SYSTICK_CLK		0x00000004
#define portNVIC_SYSTICK_INT		0x00000002
#define portNVIC_SYSTICK_ENABLE		0x00000001
#define portNVIC_PENDSVSET			0x10000000
#define portMIN_INTERRUPT_PRIORITY	( 255UL )
#define portNVIC_PENDSV_PRI			( portMIN_INTERRUPT_PRIORITY << 16UL )
#define portNVIC_SYSTICK_PRI		( portMIN_INTERRUPT_PRIORITY << 24UL )


/*
 * Exception handlers.
 */
void BT_NVIC_PendSV_Handler(void) __attribute__ (( naked ));
void BT_NVIC_SysTick_Handler(void);
void BT_NVIC_SVC_Handler(void) __attribute__ (( naked ));

static unsigned portBASE_TYPE uxCriticalNesting = 0xaaaaaaaa;

/**
 *	Initialise the stack of a task to look as if a call to
 *	portSAVE_CONTEXT has been called.
 *
 **/
/*
 * See header file for description.
 */
portSTACK_TYPE *pxPortInitialiseStack(portSTACK_TYPE *pxTopOfStack,
		pdTASK_CODE pxCode, void *pvParameters) {
	/* Simulate the stack frame as it would be created by a context switch
	interrupt. */
	pxTopOfStack--; /* Offset added to account for the way the MCU uses the stack on entry/exit of interrupts. */
	*pxTopOfStack = portINITIAL_XPSR;	/* xPSR */
	pxTopOfStack--;
	*pxTopOfStack = ( portSTACK_TYPE ) pxCode;	/* PC */
	pxTopOfStack -= 6;	/* LR, R12, R3..R1 */
	*pxTopOfStack = ( portSTACK_TYPE ) pvParameters;	/* R0 */
	pxTopOfStack -= 8; /* R11..R4. */

	return pxTopOfStack;
}

void vPortExitTask(void) {
	vTaskDelete(NULL);
}

static void prvSetupTimerInterrupt(void) {

	BT_ERROR Error;
	BT_MACHINE_DESCRIPTION *pMachine = BT_GetMachineDescription(&Error);
	if (!pMachine) {
		return;
	}

	const BT_INTEGRATED_DRIVER *pDriver;
	BT_HANDLE hTimer = NULL;

	pDriver = BT_GetIntegratedDriverByName(pMachine->pSystemTimer->name);
	if (!pDriver) {
		return;
	}

	hTimer = pDriver->pfnProbe(pMachine->pSystemTimer, &Error);
	if (!hTimer) {
		return;
	}

	BT_SetSystemTimerHandle(hTimer);

	const BT_DEV_IF_SYSTIMER *pTimerOps =
			hTimer->h.pIf->oIfs.pDevIF->unConfigIfs.pSysTimerIF;
	pTimerOps->pfnSetFrequency(hTimer, configTICK_RATE_HZ);
	pTimerOps->pfnRegisterInterrupt(hTimer, tick_isr_handler, NULL);
	pTimerOps->pfnEnableInterrupt(hTimer);
	pTimerOps->pfnStart(hTimer);
}

extern void enable_irq(void);

void vPortStartFirstTask(void) {
	__asm volatile(
					" movs r0, #0x00 	\n" /* Locate the top of stack. */
					" ldr r0, [r0] 		\n"
					" msr msp, r0		\n" /* Set the msp back to the start of the stack. */
					" cpsie i			\n" /* Globally enable interrupts. */
					" svc 0				\n" /* System call to start first task. */
					" nop				\n"
				);
}

portBASE_TYPE xPortStartScheduler(void) {

	/* Make PendSV, CallSV and SysTick the same priroity as the kernel. */
	*(portNVIC_SYSPRI2) |= portNVIC_PENDSV_PRI;
	*(portNVIC_SYSPRI2) |= portNVIC_SYSTICK_PRI;

	// Setup Hardware Timer!
	prvSetupTimerInterrupt();
	// Start first task

	uxCriticalNesting = 0;

	vPortStartFirstTask();

	// Should not get here!
	return 0;
}

/*
 *	Critical Sections.
 *
 **/

/* The code generated by the GCC compiler uses the stack in different ways at
 different optimisation levels.  The interrupt flags can therefore not always
 be saved to the stack.  Instead the critical section nesting level is stored
 in a variable, which is then saved as part of the stack context. */
void vPortEnterCritical(void) {
	/* Disable interrupts as per portDISABLE_INTERRUPTS(); 					*/
	portDISABLE_INTERRUPTS();
	uxCriticalNesting++;
}

void vPortExitCritical(void) {
	uxCriticalNesting--;
	if (!uxCriticalNesting) {
		portENABLE_INTERRUPTS();
	}
}

void vPortEndScheduler(void) {
	/* It is unlikely that the ARM port will require this function as there
	 is nothing to return to.  */
}

/*
 * FreeRTOS bottom-level IRQ vector handler
 */
void vFreeRTOS_IRQInterrupt(void) __attribute__((naked));
void vFreeRTOS_IRQInterrupt(void) {

	unsigned portLONG ulDummy;

	/* If using preemption, also force a context switch. */
	//#if configUSE_PREEMPTION == 1
	//	SCB->ICSR = portNVIC_PENDSVSET;
	//#endif

	ulDummy = portSET_INTERRUPT_MASK_FROM_ISR();
	{
		vTaskIncrementTick();
	}
	portCLEAR_INTERRUPT_MASK_FROM_ISR( ulDummy );

	/* Save the context of the interrupted task. */
	//portSAVE_CONTEXT();

	//ulCriticalNesting++;

	//__asm volatile( "clrex" );

	/* Call the handler provided with the standalone BSP */
	//__asm volatile( "bl  BT_ARCH_ARM_GIC_IRQHandler" );

	//ulCriticalNesting--;

	/* Restore the context of the new task. */
	//portRESTORE_CONTEXT();
}

void BT_NVIC_SVC_Handler(void) {
	__asm volatile (
					"	ldr	r3, pxCurrentTCBConst2		\n" /* Restore the context. */
					"	ldr r1, [r3]					\n" /* Use pxCurrentTCBConst to get the pxCurrentTCB address. */
					"	ldr r0, [r1]					\n" /* The first item in pxCurrentTCB is the task top of stack. */
					"	add r0, r0, #16					\n" /* Move to the high registers. */
					"	ldmia r0!, {r4-r7}				\n" /* Pop the high registers. */
					" 	mov r8, r4						\n"
					" 	mov r9, r5						\n"
					" 	mov r10, r6						\n"
					" 	mov r11, r7						\n"
					"									\n"
					"	msr psp, r0						\n" /* Remember the new top of stack for the task. */
					"									\n"
					"	sub r0, r0, #32					\n" /* Go back for the low registers that are not automatically restored. */
					" 	ldmia r0!, {r4-r7}              \n" /* Pop low registers.  */
					"	mov r1, r14						\n" /* OR R14 with 0x0d. */
					"	movs r0, #0x0d					\n"
					"	orr r1, r0						\n"
					"	bx r1							\n"
					"									\n"
					"	.align 2						\n"
					"pxCurrentTCBConst2: .word pxCurrentTCB	\n"
				);
}

void BT_NVIC_PendSV_Handler(void) {
	/* This is a naked function. */

	__asm volatile
	(
	"	mrs r0, psp							\n"
	"										\n"
	"	ldr	r3, pxCurrentTCBConst			\n" /* Get the location of the current TCB. */
	"	ldr	r2, [r3]						\n"
	"										\n"
	"	sub r0, r0, #32						\n" /* Make space for the remaining low registers. */
	"	str r0, [r2]						\n" /* Save the new top of stack. */
	"	stmia r0!, {r4-r7}					\n" /* Store the low registers that are not saved automatically. */
	" 	mov r4, r8							\n" /* Store the high registers. */
	" 	mov r5, r9							\n"
	" 	mov r6, r10							\n"
	" 	mov r7, r11							\n"
	" 	stmia r0!, {r4-r7}              	\n"
	"										\n"
	"	push {r3, r14}						\n"
	"	cpsid i								\n"
	"	bl vTaskSwitchContext				\n"
	"	cpsie i								\n"
	"	pop {r2, r3}						\n" /* lr goes in r3. r2 now holds tcb pointer. */
	"										\n"
	"	ldr r1, [r2]						\n"
	"	ldr r0, [r1]						\n" /* The first item in pxCurrentTCB is the task top of stack. */
	"	add r0, r0, #16						\n" /* Move to the high registers. */
	"	ldmia r0!, {r4-r7}					\n" /* Pop the high registers. */
	" 	mov r8, r4							\n"
	" 	mov r9, r5							\n"
	" 	mov r10, r6							\n"
	" 	mov r11, r7							\n"
	"										\n"
	"	msr psp, r0							\n" /* Remember the new top of stack for the task. */
	"										\n"
	"	sub r0, r0, #32						\n" /* Go back for the low registers that are not automatically restored. */
	" 	ldmia r0!, {r4-r7}              	\n" /* Pop low registers.  */
	"										\n"
	"	bx r3								\n"
	"										\n"
	"	.align 2							\n"
	"pxCurrentTCBConst: .word pxCurrentTCB	  "
	);
}

void BT_NVIC_SysTick_Handler(void) {
	unsigned long ulDummy;

	/* If using preemption, also force a context switch. */
#if configUSE_PREEMPTION == 1
	*(portNVIC_INT_CTRL) = portNVIC_PENDSVSET;
#endif

	ulDummy = portSET_INTERRUPT_MASK_FROM_ISR();
	{
		vTaskIncrementTick();
	}
	portCLEAR_INTERRUPT_MASK_FROM_ISR( ulDummy );
}

void vPortYieldFromISR(void) {
	/* Set a PendSV to request a context switch. */
	*(portNVIC_INT_CTRL) = portNVIC_PENDSVSET;
}