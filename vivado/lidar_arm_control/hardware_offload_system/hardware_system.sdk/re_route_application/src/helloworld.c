/******************************************************************************
*
* Copyright (C) 2002 - 2015 Xilinx, Inc.  All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
* XILINX  BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
* WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
* OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*
* Except as contained in this notice, the name of the Xilinx shall not be used
* in advertising or otherwise to promote the sale, use or other dealings in
* this Software without prior written authorization from Xilinx.
*
******************************************************************************/
/*****************************************************************************/
/**
* @file ultrasonic_sensor_driver.c
*
* This file contains a design for an Ultrasonic Sensor (HC-SR04) driver using the AXI GPIO driver (XGpio) and
* hardware device.  It only uses channel 1 of a GPIO device and assumes that
* the bit 0 of the GPIO is connected to the sensor Trigger pin and bit 1 to the Echo pin on the HW board.
*
******************************************************************************/

/***************************** Include Files *********************************/

#include <stdio.h>
#include <math.h>
#include "xparameters.h"
#include "xgpio.h"
#include "xuartlite.h"
#include "xil_printf.h"
#include "xtime_l.h"

#include "xil_exception.h"
#include "xscugic.h"

/************************** Constant Definitions *****************************/

// The following constant is used to determine which channel of the GPIO is
// used for the sensor pins if there are 2 channels supported.
#define CHANNEL 1
#define LIDAR_MOTOR 0x1   // Assumes bit 0 of GPIO is connected to an LIDAR_MOTOR pin

// The following constant controls the length of the buffers to be sent
// and received with the UartLite device.
#define SEND_BUFFER_SIZE	2
#define RECV_BUFFER_SIZE	2000
#define PACKET_SIZE			5

// The following constants map to the XPAR parameters created in the
// xparameters.h file. They are defined here such that a user can easily
// change all the needed parameters in one place.
#define GPIO_EXAMPLE_DEVICE_ID  XPAR_GPIO_0_DEVICE_ID
#define UARTLITE_DEVICE_ID	    XPAR_UARTLITE_0_DEVICE_ID
#define UARTLITE_IRPT_INTR	    XPAR_FABRIC_UARTLITE_0_VEC_ID
#define INTC_DEVICE_ID          XPAR_SCUGIC_SINGLE_DEVICE_ID
#define INTC                    XScuGic
#define INTC_HANDLER            XScuGic_InterruptHandler

#define ROBOT_CTRL_BASEADDR 	XPAR_ROBOT_ARM_CONTROL_0_S00_AXI_BASEADDR	// ROBOT ARM BASE ADDR

#define PWM_PERIOD              50000000		// PWM period in (.500 ms)
#define PWM_MIN_HIGH            277777      	// PWM minimujm high time (.00278 ms)
#define TMRCTR_0                0            	// Timer 0 ID
#define TMRCTR_1                1            	// Timer 1 ID
#define CYCLE_PER_DUTYCYCLE     10           	// Clock cycles per duty cycle
#define MAX_DUTYCYCLE           100          	// Max duty cycle
#define DUTYCYCLE_DIVISOR       4            	// Duty cycle Divisor
#define WAIT_COUNT              PWM_PERIOD   	// Interrupt wait counter
#define ANGLE_SHIFT				5				// degrees to shift angle


/************************** Function Prototypes ******************************/


int UartLiteInit(INTC *IntcInstancePtr,
		XUartLite *UartLiteInstPtr,
		u16 UartLiteDeviceId,
		u16 UartLiteIntrId);
int UartLiteStartScan(INTC *IntcInstancePtr,
		XUartLite *UartLiteInstPtr,
		u16 UartLiteDeviceId,
		u16 UartLiteIntrId);
int UartLiteSampleLidar(INTC *IntcInstancePtr,
		XUartLite *UartLiteInstPtr,
		u16 UartLiteDeviceId,
		u16 UartLiteIntrId);
static void UartLiteSendHandler(void *CallBackRef, unsigned int EventData);
static void UartLiteRecvHandler(void *CallBackRef, unsigned int EventData);
int UartLiteSetupIntrSystem(INTC *IntcInstancePtr,
		XUartLite *UartLiteInstPtr,
		u16 UartLiteIntrId);
static void UartLiteDisableIntrSystem(INTC *IntcInstancePtr,
		u16 UartLiteIntrId);

void startLidarMotor();


/************************** Variable Definitions *****************************/

XGpio Gpio; 						// The Instance of the GPIO Driver
XUartLite UartLite;            		// The instance of the UartLite Device

XScuGic_Config *IntcConfig;			// create GIC interrupt object

INTC InterruptController;  			// The instance of the Interrupt Controller

static INTC IntcInstance;			// The instance of the Interrupt Controller
static XUartLite UartLiteInst;  	// The instance of the UartLite Device

// The following buffers are used to send data and receive data with the UartLite.
u8 SendBuffer[SEND_BUFFER_SIZE];
u8 RecvBuffer[RECV_BUFFER_SIZE];

// The following counters are used to determine when the entire buffer has
// been sent.
static volatile int TotalSentCount;
static volatile int TotalRecvCount;

int intrFlag = 0;

float UPPER_ARM = 85;				// upper section of the arm
float LOWER_ARM	= 160;				// lower section of the arm

int FINISHED 	= 0;	// end of test flag
int TEST_STATE 	= 0;	// current test state

int inPos = 0;
int picked = 0;
int dropped = 0;
int baseAngle		= 90;	// start angle of the base servo
int shoulderAngle	= 0;	// start angle of the shoulder servo
int elbowAngle		= 90;	// start angle of the elbow servo
int clawAngle		= 90;	// start angle of the claw servo

XTime tStart_React, tEnd_React;
float us_reaction;

typedef struct ObjectCoordinate{
    int angle;
    int distance;
} ObjectCoordinate;

ObjectCoordinate currentObject;


/*****************************************************************************/
//////////
// main //
//////////
int main(void){

	xil_printf("\n>>> Start LIDAR object detection and ARM control\r\n\n");
	// application health status
	int Status;

	// Initialize the GPIO driver
	Status = XGpio_Initialize(&Gpio, GPIO_EXAMPLE_DEVICE_ID);
	if (Status != XST_SUCCESS) {
		return XST_FAILURE;
	}
	xil_printf(" GPIO: Gpio Initialization\r\n");

	// Set the direction for all signals as inputs except the LIDAR_MOTOR output.
	XGpio_SetDataDirection(&Gpio, CHANNEL, ~(LIDAR_MOTOR));

	// Start rotating the LIDAR motor.
	startLidarMotor();

	// trigger the LIDAR to start scanning
	Status = UartLiteStartScan(&IntcInstance,
			&UartLiteInst,
			UARTLITE_DEVICE_ID,
			UARTLITE_IRPT_INTR);
	if (Status != XST_SUCCESS) {
		return XST_FAILURE;
	}
	xil_printf("LIDAR: Triggered Scanning\r\n");


	///////////////
	// main loop //
	///////////////
	while (1) {
		// get sample from the LIDAR device
		Status = UartLiteSampleLidar(&IntcInstance,
				&UartLiteInst,
				UARTLITE_DEVICE_ID,
				UARTLITE_IRPT_INTR);
		if (Status != XST_SUCCESS) {
			xil_printf("LIDAR: Failed to obtain sample\r\n");
		}

		int check_hw = Xil_In32(ROBOT_CTRL_BASEADDR);
		xil_printf("HW-Status: %d\r\n", check_hw);
	}

	xil_printf("UART: Disabling UartLite Intr System\r\n");
	UartLiteDisableIntrSystem(&IntcInstance, UARTLITE_DEVICE_ID);


	while(1);

	return XST_SUCCESS;
}


/****************************************************************************
*
* This function clears the GPIO channel before writing to the LIDAR MOTOR
* bit on this channel. This will rotate the DC motor on the RPLIDAR device.
*
* @return	void.
*
* @note		None.
*
****************************************************************************/
void startLidarMotor(void){
	// Clear the LIDAR_MOTOR bit
	XGpio_DiscreteClear(&Gpio, CHANNEL, LIDAR_MOTOR);

	// Set the LIDAR_MOTOR to High
	XGpio_DiscreteWrite(&Gpio, CHANNEL, LIDAR_MOTOR);
}


/****************************************************************************
*
* This function will initialize the UartLite driver and perform a self test.
* It then sends a 2 byte word over the AXI-UartLite interface to the
* RPLIDAR device. This word will trigger the scan operation on the device.
*
* This function sends data through the UartLite.
*
* This function uses the interrupt driver mode of the UartLite.  The calls to
* the  UartLite driver in the interrupt handlers, should only use the
* non-blocking calls.
*
* @param	IntcInstancePtr is a pointer to the instance of INTC driver.
* @param	UartLiteInstPtr is a pointer to the instance of UartLite driver.
* @param	UartLiteDeviceId is the Device ID of the UartLite Device and
*		is the XPAR_<UARTLITE_instance>_DEVICE_ID value from
*		xparameters.h.
* @param	UartLiteIntrId is the Interrupt ID and is typically
*		XPAR_<INTC_instance>_<UARTLITE_instance>_VEC_ID value from
*		xparameters.h.
*
* @return	XST_SUCCESS if successful, otherwise XST_FAILURE.
*
* @note		None.
*
****************************************************************************/
int UartLiteInit(INTC *IntcInstancePtr,
			XUartLite *UartLiteInstPtr,
			u16 UartLiteDeviceId,
			u16 UartLiteIntrId){

	int Status;

	// Initialize the UartLite driver so that it's ready to use.
	Status = XUartLite_Initialize(UartLiteInstPtr, UartLiteDeviceId);
	if (Status != XST_SUCCESS) {
		xil_printf("UART: UartLite Initialize\n");

		return XST_FAILURE;
	}

//	// Perform a self-test to ensure that the hardware was built correctly.
//	Status = XUartLite_SelfTest(UartLiteInstPtr);
//	if (Status != XST_SUCCESS) {
//		xil_printf("UART: UartLite SelfTest Failed\n");
//		return XST_FAILURE;
//	}

	// Connect the UartLite to the interrupt subsystem such that interrupts
	// can occur. This function is application specific.
	Status = UartLiteSetupIntrSystem(IntcInstancePtr,
					 UartLiteInstPtr,
					 UartLiteIntrId);
	if (Status != XST_SUCCESS) {
		xil_printf("UART: UartLite Setup Intc Failed\n");
		return XST_FAILURE;
	}

	// Setup the handlers for the UartLite that will be called from the
	// interrupt context when data has been sent and received,
	// specify a pointer to the UartLite driver instance as the callback
	// reference so the handlers are able to access the instance data.
	XUartLite_SetSendHandler(UartLiteInstPtr, UartLiteSendHandler,
							 UartLiteInstPtr);
	XUartLite_SetRecvHandler(UartLiteInstPtr, UartLiteRecvHandler,
							 UartLiteInstPtr);

	//Enable the interrupt of the UartLite so that the interrupts will occur.
	XUartLite_EnableInterrupt(UartLiteInstPtr);

	return XST_SUCCESS;
}


/****************************************************************************
*
* This function will initialize the UartLite driver and perform a self test.
* It then sends a 2 byte word over the AXI-UartLite interface to the
* RPLIDAR device. This word will trigger the scan operation on the device.
*
* This function sends data through the UartLite.
*
* This function uses the interrupt driver mode of the UartLite.  The calls to
* the  UartLite driver in the interrupt handlers, should only use the
* non-blocking calls.
*
* @param	IntcInstancePtr is a pointer to the instance of INTC driver.
* @param	UartLiteInstPtr is a pointer to the instance of UartLite driver.
* @param	UartLiteDeviceId is the Device ID of the UartLite Device and
*		is the XPAR_<UARTLITE_instance>_DEVICE_ID value from
*		xparameters.h.
* @param	UartLiteIntrId is the Interrupt ID and is typically
*		XPAR_<INTC_instance>_<UARTLITE_instance>_VEC_ID value from
*		xparameters.h.
*
* @return	XST_SUCCESS if successful, otherwise XST_FAILURE.
*
* @note		None.
*
****************************************************************************/
int UartLiteStartScan(INTC *IntcInstancePtr,
			XUartLite *UartLiteInstPtr,
			u16 UartLiteDeviceId,
			u16 UartLiteIntrId){

	int Status;

	// trigger the LIDAR to start scanning
	Status = UartLiteInit(&IntcInstance,
			&UartLiteInst,
			UARTLITE_DEVICE_ID,
			UARTLITE_IRPT_INTR);
	if (Status != XST_SUCCESS) {
		return XST_FAILURE;
	}

	// Initialize the send buffer bytes with a pattern to send.
	SendBuffer[0] = 0xA5;
	SendBuffer[1] = 0x20;

	// Send the buffer using the UartLite.
	XUartLite_Send(UartLiteInstPtr, SendBuffer, SEND_BUFFER_SIZE);

	return XST_SUCCESS;
}



/****************************************************************************
*
* This function does a minimal test on the UartLite device and driver as a
* design example. The purpose of this function is to illustrate how to use
* the XUartLite component.
*
* This function sends data through the UartLite.
*
* This function uses the interrupt driver mode of the UartLite.  The calls to
* the  UartLite driver in the interrupt handlers, should only use the
* non-blocking calls.
*
* @param	IntcInstancePtr is a pointer to the instance of INTC driver.
* @param	UartLiteInstPtr is a pointer to the instance of UartLite driver.
* @param	UartLiteDeviceId is the Device ID of the UartLite Device and
*		is the XPAR_<UARTLITE_instance>_DEVICE_ID value from
*		xparameters.h.
* @param	UartLiteIntrId is the Interrupt ID and is typically
*		XPAR_<INTC_instance>_<UARTLITE_instance>_VEC_ID value from
*		xparameters.h.
*
* @return	XST_SUCCESS if successful, otherwise XST_FAILURE.
*
* @note		None.
*
* This function contains an infinite loop such that if interrupts are not
* working it may never return.
*
****************************************************************************/
int UartLiteSampleLidar(INTC *IntcInstancePtr,
			XUartLite *UartLiteInstPtr,
			u16 UartLiteDeviceId,
			u16 UartLiteIntrId){

	int Status;

	// trigger the LIDAR to start scanning
	Status = UartLiteInit(&IntcInstance,
			&UartLiteInst,
			UARTLITE_DEVICE_ID,
			UARTLITE_IRPT_INTR);
	if (Status != XST_SUCCESS) {
		return XST_FAILURE;
	}

	// this flag ensures the handler processes the interrupt
	intrFlag = 1;

	// fill the receive buffer with incoming data
	XUartLite_Recv(UartLiteInstPtr, RecvBuffer, RECV_BUFFER_SIZE);

	// Wait for the entire buffer to be filled,  the function may get
	// locked up in this loop if the interrupts are not working correctly.
	while(!TotalRecvCount){}

	// reset the for next sample
	TotalRecvCount = 0;

	u8 byte = RecvBuffer[0];
	xil_printf("Recv Byte: %x\r\n", byte);

	Xil_Out32((ROBOT_CTRL_BASEADDR), byte);

	return XST_SUCCESS;
}



/****************************************************************************
*
* This function is the handler which performs processing to send data to the
* UartLite. It is called from an interrupt context such that the amount of
* processing performed should be minimized. It is called when the transmit
* FIFO of the UartLite is empty and more data can be sent through the UartLite.
*
* @param	CallBackRef contains a callback reference from the driver.
*		In this case it is the instance pointer for the UartLite driver.
* @param	EventData contains the number of bytes sent or received for sent
*		and receive events.
*
* @return	None.
*
* @note		None.
*
****************************************************************************/
static void UartLiteSendHandler(void *CallBackRef, unsigned int EventData){

	// verifying the number of bytes sent to LIDAR
	TotalSentCount = EventData;
	xil_printf("UART: Send Buffer Length: %d\n\n", TotalSentCount);
}


/****************************************************************************
*
* This function is the handler which performs processing to receive data from
* the UartLite. It is called from an interrupt context such that the amount of
* processing performed should be minimized. It is called when any data is
* present in the receive FIFO of the UartLite such that the data can be
* retrieved from the UartLite. The amount of data present in the FIFO is not
* known when this function is called.
*
* @param	CallBackRef contains a callback reference from the driver,
*		in this case it is the instance pointer for the UartLite driver.
* @param	EventData contains the number of bytes sent or received for sent
*		and receive events.
*
* @return	None.
*
* @note		None.
*
****************************************************************************/
static void UartLiteRecvHandler(void *CallBackRef, unsigned int EventData){

	// if the flag is set be process the interrupt
	// we set the total number of bytes currently in the buffer
	if (intrFlag){
		TotalRecvCount = EventData;
//		xil_printf("Recv Buffer Length: %d\n", TotalRecvCount);
		intrFlag=0;
	}
}


/****************************************************************************
*
* This function setups the interrupt system such that interrupts can occur
* for the UartLite.
*
* @param	IntcInstancePtr is a pointer to the instance of INTC driver.
* @param	UartLiteInstPtr is a pointer to the instance of UartLite driver.
*		XPAR_<UARTLITE_instance>_DEVICE_ID value from xparameters.h.
* @param	UartLiteIntrId is the Interrupt ID and is typically
*		XPAR_<INTC_instance>_<UARTLITE_instance>_VEC_ID
*		value from xparameters.h.
*
* @return	XST_SUCCESS if successful, otherwise XST_FAILURE.
*
* @note		None.
*
****************************************************************************/
int UartLiteSetupIntrSystem(INTC *IntcInstancePtr,
		XUartLite *UartLiteInstPtr,
		u16 UartLiteIntrId){

	int Status;


	// Initialize the interrupt controller driver so that it is ready to use.
	IntcConfig = XScuGic_LookupConfig(INTC_DEVICE_ID);
	if (NULL == IntcConfig) {
		return XST_FAILURE;
	}

	Status = XScuGic_CfgInitialize(IntcInstancePtr, IntcConfig,
					IntcConfig->CpuBaseAddress);
	if (Status != XST_SUCCESS) {
		return XST_FAILURE;
	}

	XScuGic_SetPriorityTriggerType(IntcInstancePtr, UartLiteIntrId,
					0xA0, 0x3);

	// Connect the interrupt handler that will be called when an interrupt occurs for the device.
	Status = XScuGic_Connect(IntcInstancePtr, UartLiteIntrId,
				 (Xil_ExceptionHandler)XUartLite_InterruptHandler,
				 UartLiteInstPtr);
	if (Status != XST_SUCCESS) {
		return Status;
	}

	// Enable the interrupt for the Timer device.
	XScuGic_Enable(IntcInstancePtr, UartLiteIntrId);


	// Initialize the exception table.
	Xil_ExceptionInit();

	// Register the interrupt controller handler with the exception table.
	Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_INT,
			(Xil_ExceptionHandler)INTC_HANDLER,
			IntcInstancePtr);

	// Enable exceptions.
	Xil_ExceptionEnable();

	return XST_SUCCESS;
}


/*****************************************************************************
*
* This function disables the interrupts that occur for the UartLite.
*
* @param	IntcInstancePtr is a pointer to the instance of the INTC driver.
* @param	UartLiteIntrId is the Interrupt ID and is typically
*		XPAR_<INTC_instance>_<UARTLITE_instance>_VEC_ID
*		value from xparameters.h.
*
* @return	None.
*
* @note		None.
*
******************************************************************************/
static void UartLiteDisableIntrSystem(INTC *IntcInstancePtr, u16 UartLiteIntrId){

	// Disconnect and disable the interrupt for the UartLite.
	XScuGic_Disable(IntcInstancePtr, UartLiteIntrId);
	XScuGic_Disconnect(IntcInstancePtr, UartLiteIntrId);
}
