//*****************************************************************************
//
// MSP432 main.c template - Empty main
//
//****************************************************************************

#include "msp.h"
#include <stdio.h>
#include <driverlib.h>
#include "mcp2515.h"
#include "clock.h"
#include "canutil.h"

uint_fast8_t RXData;
int i;
uint8_t mode;
uint_fast8_t result;

/* SPI Timing Config */
const MCP_CANTimingConfig CANTimingConfig = { 24000000, /* Oscillator Frequency */
6, /* Baud Rate Prescaler */
1, /* Propagation Delay */
3, /* Phase Segment 1 */
3, /* Phase Segment 2 */
1 /* Synchronisation Jump Width */
};

void msgHandler(MCP_CANMessage *);

void main(void) {

	/* Halting the watchdog */
	MAP_WDT_A_holdTimer();

	printf("Starting...\r\n");

	/* Start the clock */
	startClockOnPin();

	/* Init the CAN controller */
	MCP_init();

	/* RESET */
	MCP_reset();
	printf("RESET\n");

	/* Make sure configuration mode is set */
	MCP_setMode(MODE_CONFIG);

	MCP_setTiming(&CANTimingConfig);

	/* Register an interrupt on RX0 and TX0 */
	MCP_enableInterrupt(MCP_ISR_TX0IE | MCP_ISR_RX0IE);

	/* Set the handler to be called when a message is received */
	MCP_setReceivedMessageHandler(&msgHandler);

	/* Activate loopback mode */
	MCP_setMode(MODE_LOOPBACK);

	result = MCP_readRegister(RCANSTAT);
	printf("CANSTAT: 0x%x\n", result);

	uint_fast8_t data[] = { 0, 1, 2, 3, 4, 5, 6, 7 };
	MCP_fillBuffer(0x0F, data, 8);

	printf("Transmitting:");
	for (i = 0; i < 8; i++) {
		printf(" 0x%x", data[i]);
	}
	printf("\n");

	MCP_sendRTS(TXB0);

	/* Do the main loop */
	while (1) {
		MAP_PCM_gotoLPM0InterruptSafe();
	}
}

void msgHandler(MCP_CANMessage * msg) {
	printf("Received:");
	for (i = 0; i < 8; i++) {
		printf(" 0x%x", msg->data[i]);
	}
	printf("\n");
}
