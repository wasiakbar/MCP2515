/*
 * mcp2515.c
 *
 *  Created on: 17 Jun 2016
 *      Author: Stefan van der Linden
 */

#include <driverlib.h>
#include <stdio.h>
#include "mcp2515.h"
#include "mcpdelay.h"

#include "simple_spi.h"

#define BUFFER_SIZE 	14
#define TIMEOUT 		500

#define CMD_WRITE 		0x02
#define CMD_READ 		0x03
#define CMD_RESET 		0xC0
#define CMD_READ_STATUS 0xA0
#define CMD_BIT_MODIFY 	0x05
#define CMD_RTS 		0x80
#define CMD_LOAD_TX		0x40
#define CMD_READ_RX		0x90

volatile uint_fast8_t RXData;
volatile uint_fast8_t mode;
volatile uint_fast8_t TXData[BUFFER_SIZE];
volatile uint_fast8_t TXSize;
volatile uint_fast8_t enabledIRQ;

uint_fast8_t messageData[8];

MCP_CANMessage receivedMessageB0, receivedMessageB1;

volatile uint_fast8_t BufferState;

void (*rcvdMsgHandler)( MCP_CANMessage * );
void (*bufferAvailableCallBack)( void );
void (*errorHandler) (uint_fast8_t);

/*** PROTOTYPES ***/
uint_fast8_t _getAvailableTXB( void );

/*** HIGHER LEVEL FUNCTIONS ***/

void MCP_setTiming( const MCP_CANTimingConfig * TimingConfig ) {
    uint_fast8_t buffer;
    uint_fast8_t CNF1 = 0;
    /* Make sure PS2 is always determined from the setting in CNF3 */
    uint_fast8_t CNF2 = 0x80;
    uint_fast8_t CNF3 = 0;

    /* CNF1: SJW(7:6) BRP(5:0) */
    CNF1 = 0x3F & (TimingConfig->BRP - 1);
    buffer = TimingConfig->SJW - 1;
    buffer <<= 6;
    CNF1 |= buffer;

    /* CNF2: BTLMODE(7) SAM(6) PHSEG1(5:3) PRSEG(2:0) */
    buffer = 0x07 & (TimingConfig->PS1 - 1);
    buffer <<= 3;
    CNF2 |= buffer;
    buffer = 0x07 & (TimingConfig->PROP - 1);

    /* CNF3: SOF(7) WAKFIL(6) PHSEG2(2:0) */
    CNF3 = 0x07 & (TimingConfig->PS2 - 1);

    /* Write to the register */
    MCP_writeRegister(RCNF1, CNF1);
    MCP_writeRegister(RCNF2, CNF2);
    MCP_writeRegister(RCNF3, CNF3);
}

void MCP_setMode( uint_fast8_t mode ) {
    mode <<= 5;
    MCP_modifyBit(RCANCTRL, 0xE0, mode);
}

void MCP_enableInterrupt( uint_fast8_t interrupts ) {
    enabledIRQ |= interrupts;
    MCP_modifyBit(RCANINTE, interrupts, interrupts);
}

void MCP_disableInterrupt( uint_fast8_t interrupts ) {
    MCP_modifyBit(RCANINTE, interrupts, 0x00);
    enabledIRQ &= ~interrupts;
}

void MCP_clearInterrupt( uint_fast8_t interrupts ) {
    MCP_modifyBit(RCANINTF, interrupts, 0x00);
}

void MCP_enableMasterInterrupt( void ) {
    /* Enable the dedicated INT pin (active low) */
    MAP_GPIO_setAsInputPinWithPullUpResistor(GPIO_PORT_P3, GPIO_PIN5);
    MAP_GPIO_interruptEdgeSelect(GPIO_PORT_P3, GPIO_PIN5,
    GPIO_HIGH_TO_LOW_TRANSITION);
    MAP_GPIO_clearInterruptFlag(GPIO_PORT_P3, GPIO_PIN5);
    MAP_GPIO_enableInterrupt(GPIO_PORT_P3, GPIO_PIN5);
    MAP_Interrupt_enableInterrupt(INT_PORT3);
}

void MCP_disableMasterInterrupt( void ) {
    MAP_GPIO_disableInterrupt(GPIO_PORT_P3, GPIO_PIN5);
    MAP_GPIO_clearInterruptFlag(GPIO_PORT_P3, GPIO_PIN5);
}

uint_fast8_t MCP_getInterruptStatus( void ) {
    return MCP_readRegister(RCANINTF) & enabledIRQ;
}

void MCP_setReceivedMessageHandler( void (*handle)( MCP_CANMessage * ) ) {
    rcvdMsgHandler = handle;
}

void MCP_setBufferAvailableCallback( void (*handle)( void ) ) {
    bufferAvailableCallBack = handle;
}

void MCP_setErrorHandler( void (*handle)(uint_fast8_t)) {
	errorHandler = handle;
}

uint_fast8_t MCP_isTXBufferAvailable( void ) {
    if ( _getAvailableTXB( ) == 0xFF )
        return false;
    else
        return true;
}

uint_fast8_t MCP_areAllTXBuffersAvailable( void ) {
    if ( !BufferState ) {
        return true;
    } else {
        return false;
    }
}

/*** LOWER LEVEL FUNCTIONS ***/

void MCP_init( void ) {
    /* Start the SPI module */
    SIMSPI_startSPI( );

    enabledIRQ = 0;

    MCP_enableMasterInterrupt( );

    /* Enabling MASTER interrupts */
    MAP_Interrupt_enableMaster( );

    /* Initialise variables */
    RXData = 0;
    BufferState = 0;
    receivedMessageB0.ID = 0;
    receivedMessageB0.isExtended = 0;
    receivedMessageB0.isRequest = 0;
    receivedMessageB0.length = 0;
    receivedMessageB0.data = messageData;

    receivedMessageB1.ID = 0;
    receivedMessageB1.isExtended = 0;
    receivedMessageB1.isRequest = 0;
    receivedMessageB1.length = 0;
    receivedMessageB1.data = messageData;
}

uint_fast8_t MCP_reset( void ) {
    DELAY_WITH_TIMEOUT(mode);
    if ( mode )
        return 1;
    mode = CMD_RESET;

    /* Send the command */
    MCP_CS_LOW

    SIMSPI_transmitByte(0xC0);

    MCP_CS_HIGH
    mode = 0;
    return 0;
}

uint_fast8_t MCP_readStatus( void ) {
    DELAY_WITH_TIMEOUT(mode);
    if ( mode )
        return 0xFF;

    mode = CMD_READ_STATUS;

    /* Populate the TX buffer */
    TXData[0] = CMD_READ_STATUS;
    TXData[1] = 0x00;
    TXData[2] = 0x00;

    /* Perform transaction */
    MCP_CS_LOW
    RXData = SIMSPI_transmitBytes((uint_fast8_t *) TXData, 3);
    MCP_CS_HIGH

    mode = 0;

    return RXData;
}

uint_fast8_t MCP_readRegister( uint_fast8_t address ) {
    DELAY_WITH_TIMEOUT(mode);
    if ( mode )
        return 0xFF;

    mode = CMD_READ;

    TXData[0] = CMD_READ;
    TXData[1] = address;
    TXData[2] = 0x00;

    /* Perform transaction */
    MCP_CS_LOW
    RXData = SIMSPI_transmitBytes((uint_fast8_t *) TXData, 3);
    MCP_CS_HIGH

    mode = 0;

    return RXData;
}

uint_fast8_t MCP_writeRegister( uint_fast8_t address, uint_fast8_t value ) {
    DELAY_WITH_TIMEOUT(mode);
    if ( mode )
        return 1;

    mode = CMD_WRITE;

    // By setting TXData, the data byte is sent once the tx buffer is available
    TXData[0] = CMD_WRITE;
    TXData[1] = address;
    TXData[2] = value;

    /* Perform transaction */
    MCP_CS_LOW
    RXData = SIMSPI_transmitBytes((uint_fast8_t *) TXData, 3);
    MCP_CS_HIGH

    mode = 0;

    return 0;
}

uint_fast8_t MCP_modifyBit( uint_fast8_t address, uint_fast8_t mask,
        uint_fast8_t value ) {
    DELAY_WITH_TIMEOUT(mode);
    if ( mode )
        return 1;

    mode = CMD_BIT_MODIFY;

    TXData[0] = CMD_BIT_MODIFY;
    TXData[1] = address;
    TXData[2] = mask;
    TXData[3] = value;
    TXSize = 4;

    /* Perform transaction */
    MCP_CS_LOW
    RXData = SIMSPI_transmitBytes((uint_fast8_t *) TXData, 4);
    MCP_CS_HIGH

    mode = 0;

    return 0;
}

uint_fast8_t MCP_sendRTS( uint_fast8_t whichBuffer ) {
    DELAY_WITH_TIMEOUT(mode);
    if ( mode )
        return 1;

    mode = CMD_RTS;

    TXData[0] = CMD_RTS;
    TXData[0] |= whichBuffer;

    /* Send the command */
    MCP_CS_LOW
    RXData = SIMSPI_transmitByte(TXData[0]);
    MCP_CS_HIGH
    mode = 0;

    return 0;
}

uint_fast8_t MCP_fillBuffer( MCP_CANMessage * msg ) {
    DELAY_WITH_TIMEOUT(mode);
    if ( mode )
        return 0xFF;
    if ( msg->length > 8 )
        return 0xFF;

    mode = CMD_LOAD_TX;

    uint_fast8_t TXB = _getAvailableTXB( );
    uint_fast8_t ii;
    if ( TXB != 0xFF ) {
        BufferState |= TXB;
        /* TXB0 is a special case since it is equal to 0x00 */
        if ( TXB == TXB0 )
            TXB = 0;

        /* Prepare the transmit queue */
        TXData[0] = CMD_LOAD_TX | TXB; /* Command + Address */
        TXData[1] = (uint8_t) (msg->ID >> 3); /* SIDH */
        TXData[2] = (uint8_t) (msg->ID << 5); /* SIDL */
        if ( msg->isExtended ) {
            TXData[2] |= (uint8_t) BIT3; /* SIDL */
            TXData[3] = (uint8_t) (msg->ID >> 8); /* EID8 */
            TXData[4] = (uint8_t) msg->ID; /* EID0 */

        } else {
            TXData[3] = (uint8_t) 0x00; /* EID8 */
            TXData[4] = (uint8_t) 0x00; /* EID0 */
        }
        TXData[5] = (uint8_t) (0x0F & msg->length); /* DLC  */
        if ( msg->isRequest )
            TXData[5] |= 0x40;

        /* Transmit actual data to buffer */
        for ( ii = 0; ii < msg->length; ii++ ) {
            TXData[6 + ii] = msg->data[ii];
        }

        /* Do transaction */
        TXSize = 6 + msg->length;

        /* Perform transaction */
        MCP_CS_LOW
        RXData = SIMSPI_transmitBytes((uint_fast8_t *) TXData, TXSize);
        MCP_CS_HIGH

        mode = 0;
    }
    if ( !TXB ) {
        return TXB0;
    } else {
        return TXB;
    }
}

uint_fast8_t MCP_fillGivenBuffer( MCP_CANMessage * msg, uint_fast8_t TXB ) {
    DELAY_WITH_TIMEOUT(mode);
    if ( mode )
        return 0xFF;
    if ( msg->length > 8 )
        return 0xFF;

    mode = CMD_LOAD_TX;

    uint_fast8_t ii;
    if ( TXB != 0xFF ) {
        BufferState |= TXB;
        /* TXB0 is a special case since it is equal to 0x00 */
        if ( TXB == TXB0 )
            TXB = 0;

        /* Prepare the transmit queue */
        TXData[0] = CMD_LOAD_TX | TXB; /* Command + Address */
        TXData[1] = (uint8_t) (msg->ID >> 3); /* SIDH */
        TXData[2] = (uint8_t) (msg->ID << 5); /* SIDL */
        if ( msg->isExtended ) {
            TXData[2] |= (uint8_t) BIT3; /* SIDL */
            TXData[3] = (uint8_t) (msg->ID >> 8); /* EID8 */
            TXData[4] = (uint8_t) msg->ID; /* EID0 */
        } else {
            TXData[3] = (uint8_t) 0x00; /* EID8 */
            TXData[4] = (uint8_t) 0x00; /* EID0 */
        }
        TXData[5] = (uint8_t) (0x0F & msg->length); /* DLC  */
        if ( msg->isRequest )
            TXData[5] |= 0x40;

        /* Transmit actual data to buffer */
        for ( ii = 0; ii < msg->length; ii++ ) {
            TXData[6 + ii] = msg->data[ii];
        }

        /* Do transaction */
        TXSize = 6 + msg->length;

        /* Perform transaction */
        MCP_CS_LOW
        RXData = SIMSPI_transmitBytes((uint_fast8_t *) TXData, TXSize);
        MCP_CS_HIGH

        mode = 0;
    }
    if ( !TXB ) {
        return TXB0;
    } else {
        return TXB;
    }
}

uint_fast8_t MCP_readBuffer( MCP_CANMessage * msgBuffer, uint_fast8_t RXB ) {
    DELAY_WITH_TIMEOUT(mode);
    if ( mode )
        return 1;
    mode = CMD_READ_RX;

    if ( RXB == RXB0 )
        RXB = 0x00;

    uint_fast8_t it;
    uint_fast8_t rxbuffer[8];

    TXData[0] = CMD_READ_RX + (RXB << 1);

    MCP_CS_LOW
    SIMSPI_transmitByte(CMD_READ_RX + (RXB << 1));
    SIMSPI_readBytes(rxbuffer, 6);
    MCP_CS_HIGH

    msgBuffer->length = 0x0F & rxbuffer[4];
    msgBuffer->isExtended = ((BIT3 & rxbuffer[1]) >> 3);
    msgBuffer->isRequest = ((BIT4 & rxbuffer[1]) >> 4)
            | ((BIT6 & rxbuffer[4]) >> 6);

    if ( msgBuffer->isExtended ) {
        msgBuffer->ID = (uint_fast32_t) rxbuffer[3];
        msgBuffer->ID |= ((uint_fast32_t) rxbuffer[2]) << 8;
    } else {
        msgBuffer->ID = (uint_fast32_t) rxbuffer[1] >> 5;
        msgBuffer->ID |= ((uint_fast32_t) rxbuffer[0]) << 3;
    }

    // If there is no data attached to this message, then clean up return
    if ( !msgBuffer->length || msgBuffer->isRequest ) {
        msgBuffer->length = 0;
        mode = 0;
        return 0;
    }

    RXB++;

    MCP_CS_LOW
    SIMSPI_transmitByte(CMD_READ_RX + (RXB << 1));
    SIMSPI_readBytes(rxbuffer, msgBuffer->length);
    MCP_CS_HIGH

    for ( it = 0; it < msgBuffer->length; it++ )
        msgBuffer->data[it] = rxbuffer[it];

    mode = 0;
    return 0;
}

uint_fast8_t MCP_sendMessage( MCP_CANMessage * msg ) {
    uint_fast8_t buff, timeout;
    timeout = 0xFF;
    while ( !MCP_isTXBufferAvailable( ) && timeout ) {
        SysCtlDelay(1000);
        timeout--;
    }
    if ( !timeout )
        return 1;

    /* Disable any interrupt from messing with the message transmission */
    MAP_GPIO_disableInterrupt(GPIO_PORT_P3, GPIO_PIN5);

    buff = MCP_fillBuffer(msg);

    /* Send the signal to transmit the message */
    MCP_sendRTS(buff);

    MAP_GPIO_enableInterrupt(GPIO_PORT_P3, GPIO_PIN5);
    return 0;
}

uint_fast8_t MCP_sendBulk( MCP_CANMessage * msgList, uint_fast8_t num ) {
    /* Not really efficient code, but necessary to work around the interrupt problems */
    MAP_GPIO_disableInterrupt(GPIO_PORT_P3, GPIO_PIN5);

    uint_fast8_t status, idx;
    idx = 0;
    uint32_t timeout = TIMEOUT;

    while ( timeout ) {
        status = MCP_readStatus( );
        if ( !(status & BIT2 ) ) {
            MCP_fillGivenBuffer(&msgList[idx], TXB0);
            MCP_sendRTS(TXB0);
            idx++;
        }
        if ( idx == num ) {
            MAP_GPIO_enableInterrupt(GPIO_PORT_P3, GPIO_PIN5);
            return 0;
        }
        if ( !(status & BIT4 ) ) {
            MCP_fillGivenBuffer(&msgList[idx], TXB1);
            MCP_sendRTS(TXB1);
            idx++;
        }
        if ( idx == num ) {
            MAP_GPIO_enableInterrupt(GPIO_PORT_P3, GPIO_PIN5);
            return 0;
        }
        if ( !(status & BIT6 ) ) {
            MCP_fillGivenBuffer(&msgList[idx], TXB2);
            MCP_sendRTS(TXB2);
            idx++;
        }
        if ( idx == num ) {
            MAP_GPIO_enableInterrupt(GPIO_PORT_P3, GPIO_PIN5);
            return 0;
        }
        timeout--;
    }
    /* If we reach this, then we got a timeout */
    MCP_abortAll();
    MAP_GPIO_enableInterrupt(GPIO_PORT_P3, GPIO_PIN5);
    return 1;
}

void MCP_abortAll( void ) {
	/* Set abort flag */
	MCP_modifyBit(RCANCTRL, BIT4, BIT4);
	while(MCP_readStatus() & 0x54);
	MCP_modifyBit(RCANCTRL, BIT4, 0);
}

/*** PRIVATE FUNCTIONS ***/

uint_fast8_t _getAvailableTXB( void ) {
    if ( !(BufferState & TXB0) )
        return 0x01;
    if ( !(BufferState & TXB1) )
        return 0x02;
    if ( !(BufferState & TXB2) )
        return 0x04;
    return 0xFF;
}

/*** ISR HANDLERS ***/

void GPIOP3_ISR( void ) {
    uint32_t status;
    uint_fast8_t CANStatus;

    /* Check whether no other transaction is ongoing, skip (and return later) if this is the case */
    status = MAP_GPIO_getEnabledInterruptStatus(GPIO_PORT_P3);

    if ( status & GPIO_PIN5 ) {
        /* Read and clear the interrupt flags */
        CANStatus = MCP_getInterruptStatus( );
        //printf("CANStatus: 0x%x\n", CANStatus);

        /* The TXn interrupts are triggered when a message has been sent succesfully */
        if ( CANStatus & MCP_ISR_TX0IE ) {
            BufferState &= ~TXB0;
            MCP_clearInterrupt(MCP_ISR_TX0IE);
        }

        if ( CANStatus & MCP_ISR_TX1IE ) {
            BufferState &= ~TXB1;
            MCP_clearInterrupt(MCP_ISR_TX1IE);
        }

        if ( CANStatus & MCP_ISR_TX2IE ) {
            BufferState &= ~TXB2;
            MCP_clearInterrupt(MCP_ISR_TX2IE);
        }

        /* The RXn interrupts are triggered when a message is available to be 'read' */
        if ( CANStatus & MCP_ISR_RX0IE ) {
            MCP_readBuffer(&receivedMessageB0, RXB0);
            if ( rcvdMsgHandler ) {
                (*rcvdMsgHandler)(&receivedMessageB0);
            }
        }

        if ( CANStatus & MCP_ISR_RX1IE ) {
            MCP_readBuffer(&receivedMessageB1, RXB1);
            if ( rcvdMsgHandler ) {
                (*rcvdMsgHandler)(&receivedMessageB1);
            }
        }

        /* Triggered in case of an error */
        if ( CANStatus & MCP_ISR_ERRIE ) {
            //printf("ERRIE\n");
        	uint_fast8_t status = MCP_readRegister(RREC);
            uint_fast8_t result = MCP_readRegister(REFLG);
            //printf("Message Error: 0x%x\n", result);
            if(errorHandler)
            	errorHandler(result);
            //MCP_clearInterrupt(MCP_ISR_ERRIE);
        }

        /*if ( bufferAvailableCallBack
                && (CANStatus & MCP_ISR_TX0IE || CANStatus & MCP_ISR_TX1IE
                        || CANStatus & MCP_ISR_TX2IE) ) {
            bufferAvailableCallBack( );
        }*/

        //CANStatus = MCP_getInterruptStatus( );
        //if ( !CANStatus )
        MAP_GPIO_clearInterruptFlag(GPIO_PORT_P3, status);
    }
}
