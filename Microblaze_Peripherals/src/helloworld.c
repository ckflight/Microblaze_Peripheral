/******************************************************************************
* Copyright (C) 2023 Advanced Micro Devices, Inc. All Rights Reserved.
* SPDX-License-Identifier: MIT
******************************************************************************/
/*
 * helloworld.c: simple test application
 *
 * This application configures UART 16550 to baud rate 9600.
 * PS7 UART (Zynq) is not initialized by this application, since
 * bootrom/bsp configures it to baud rate 115200
 *
 * ------------------------------------------------
 * | UART TYPE   BAUD RATE                        |
 * ------------------------------------------------
 *   uartns550   9600
 *   uartlite    Configurable only in HW design
 *   ps7_uart    115200 (configured by bootrom/bsp)
 */


#include <stdio.h>
#include "platform.h"
#include "xparameters.h"
#include "xil_printf.h"
#include "stdbool.h"

#include "xgpio.h"
#include "xuartlite.h"
#include "xspi.h"
#include <unistd.h>

#include "adxl362.h"
#include "mpu6500.h"

#include "xiltimer.h"
#include "xtmrctr.h"

#include <stdlib.h> // abs


/*
	NOTES:

	Microblaze spi io0_o is MOSI, io1_i MISO (do not select io0_i)

	The adxl362.c is added to Sources of UserConfig.cmake to build

	Input gpio must be pulled up or down in constraint file 

*/

#define LED 						0xFFFF
#define LED_DELAY     				1000000
#define LED_CHANNEL 				1

#define TRIG_CHANNEL              	1

#define ECHO_CHANNEL              	1

#define TIMER_FREQUENCY_HZ     		100000000ULL  /* e.g., 100 MHz */

XTmrCtr TimerInstance;

XSpi Spi0Instance;
XSpi Spi1Instance;

XGpio Gpio0; /* The Instance of the GPIO Driver */

XGpio Gpio1;
XGpio Gpio2;

XUartLite UartLite;		/* Instance of the UartLite Device */

uint8_t uart_rx_buffer[512];
uint8_t uart_tx_buffer[512];

uint32_t gpio_state = 0x00000000;  // Tracks current output state

int counter = 0;
int reg_id_counter = 0;

uint8_t send_buf[3] = {0x0B, 0x02, 0xFF};  // Read command, DEVID_AD, dummy
uint8_t recv_buf[4];

int16_t x_val, y_val, z_val;
uint8_t spi_rx_buffer[32];

int16_t accel[3], gyro[3];
float gyro_dps[3];
uint8_t dev_id;


void toggle_led(int led_num) {
    uint32_t bit_mask = 1u << (led_num - 1);

    // Toggle the bit
    gpio_state ^= bit_mask;

    // Write updated state
    XGpio_DiscreteWrite(&Gpio0, LED_CHANNEL, gpio_state);
}

void blink_led(void){

	for(int j = 0; j < 10; j++){
        
		XGpio_DiscreteWrite(&Gpio0, LED_CHANNEL, LED);

		usleep(10000);
		
		XGpio_DiscreteWrite(&Gpio0, LED_CHANNEL, ~LED);
        
		usleep(10000);
	}
}


u64 read_elapsed_ticks(u64 start, u64 end) {
    if (end >= start)
        return end - start;
    // handle 64-bit wraparound
    return ((u64)0xFFFFFFFFFFFFFFFFULL - start) + end + 1;
}

void trigger_hcsr04() {
    // Set TRIG high
    XGpio_DiscreteWrite(&Gpio1, TRIG_CHANNEL, 1);
    usleep(10);  // 10 microseconds
    // Set TRIG low
    XGpio_DiscreteWrite(&Gpio1, TRIG_CHANNEL, 0);
}

uint32_t measure_distance_us() {

    u64 start = XTmrCtr_GetValue(&TimerInstance, 0);

    // Wait for echo HIGH
    while (XGpio_DiscreteRead(&Gpio2, ECHO_CHANNEL) == 0);

    // Start counting
    while (XGpio_DiscreteRead(&Gpio2, ECHO_CHANNEL) == 1);

    u64 end = XTmrCtr_GetValue(&TimerInstance, 0) - start;
    
    return (uint32_t)end;
}

int main(void)
{
	int Status;

	// Initialize timer
    Status = XTmrCtr_Initialize(&TimerInstance, XPAR_AXI_TIMER_0_BASEADDR);
    if (Status != XST_SUCCESS) {
        printf("Timer init failed\n");
        return -1;
    }

    // Ensure cascade mode is set in hardware; don't use auto-reload for measuring
    XTmrCtr_SetOptions(&TimerInstance, 0, 0); // No auto-reload
    XTmrCtr_SetOptions(&TimerInstance, 1, 0);

    // Reset both halves 
	XTmrCtr_Reset(&TimerInstance, 0);  // Reset Timer 0 (low 32 bits)
	XTmrCtr_Reset(&TimerInstance, 1);  // Reset Timer 1 (high 32 bits in cascade mode)

    // Start the timer (starting counter 0 will also increment the cascaded 1)
    XTmrCtr_Start(&TimerInstance, 0);

	// Initialize the GPIO driver
	Status = XGpio_Initialize(&Gpio0, XPAR_XGPIO_0_BASEADDR);
	if (Status != XST_SUCCESS) {
		xil_printf("GPIO0 Initialization Failed\r\n");
		return XST_FAILURE;
	}

	XGpio_SetDataDirection(&Gpio0, LED_CHANNEL, 0x0000); // 0 is output set all output

	blink_led();

	// GPIO1 Init
	Status = XGpio_Initialize(&Gpio1, XPAR_XGPIO_1_BASEADDR);
	if (Status != XST_SUCCESS) {
		xil_printf("GPIO1 Initialization Failed\r\n");
		return XST_FAILURE;
	}

	XGpio_SetDataDirection(&Gpio1, TRIG_CHANNEL, 0x0); // both pins are output
	
	// GPIO2 Init
	Status = XGpio_Initialize(&Gpio2, XPAR_XGPIO_2_BASEADDR);
	if (Status != XST_SUCCESS) {
		xil_printf("GPIO2 Initialization Failed\r\n");
		return XST_FAILURE;
	}

	XGpio_SetDataDirection(&Gpio1, ECHO_CHANNEL, 0x3); // both pins are input

	// Initialize the UartLite driver so that it is ready to use.
	Status = XUartLite_Initialize(&UartLite, 0);
	if (Status != XST_SUCCESS) {
		return XST_FAILURE;
	}

	// Perform a self-test to ensure that the hardware was built correctly.
	Status = XUartLite_SelfTest(&UartLite);
	if (Status != XST_SUCCESS) {
		return XST_FAILURE;
	}

    // SPI0 Init
    Status = XSpi_Initialize(&Spi0Instance, XPAR_AXI_QUAD_SPI_0_BASEADDR);
    if (Status != XST_SUCCESS) {
        xil_printf("SPI Initialization Failed\r\n");
        return XST_FAILURE;
    }

    // Set options: master mode and manual slave select
    Status = XSpi_SetOptions(&Spi0Instance, XSP_MASTER_OPTION | XSP_MANUAL_SSELECT_OPTION);
    if (Status != XST_SUCCESS) {
        xil_printf("SPI SetOptions Failed\r\n");
        return XST_FAILURE;
    }

    XSpi_Start(&Spi0Instance);// Start the SPI driver
    XSpi_IntrGlobalDisable(&Spi0Instance); // Disable global interrupt mode

    // SPI1 Init
    Status = XSpi_Initialize(&Spi1Instance, XPAR_AXI_QUAD_SPI_1_BASEADDR);
    if (Status != XST_SUCCESS) {
        xil_printf("SPI Initialization Failed\r\n");
        return XST_FAILURE;
    }

    // Set options: master mode and manual slave select
    Status = XSpi_SetOptions(&Spi1Instance, XSP_MASTER_OPTION | XSP_MANUAL_SSELECT_OPTION | XSP_CLK_PHASE_1_OPTION | XSP_CLK_ACTIVE_LOW_OPTION);
    if (Status != XST_SUCCESS) {
        xil_printf("SPI SetOptions Failed\r\n");
        return XST_FAILURE;
    }

    XSpi_Start(&Spi1Instance);// Start the SPI driver
    XSpi_IntrGlobalDisable(&Spi1Instance); // Disable global interrupt mode


	ADXL362_SoftReset(&Spi0Instance);
	ADXL362_Init(&Spi0Instance);

    dev_id = ADXL362_ReadDeviceID(&Spi0Instance);
	xil_printf("Dev id is: %d\r\n", dev_id);



    // Initialize MPU6500
    dev_id = MPU6500_Init(&Spi1Instance);
    xil_printf("WHO_AM_I = 0x%02X\r\n", dev_id);

	while (1) {
		
		//uart_tx_buffer[0] = counter++;
		//uart_tx_buffer[0] = z_val & 0xFF;

		//XUartLite_Send(&UartLite, uart_tx_buffer, 1);

		//while(XUartLite_Recv(&UartLite, uart_rx_buffer, 1) == 0);

		if(counter > 255){
			counter = 0;
		}

		ADXL362_ReadXYZ(&Spi0Instance, &x_val, &y_val, &z_val, spi_rx_buffer);
		
        /*
		xil_printf("SPI RX Buffer: ");
		for (int i = 0; i < 9; i++) {
			xil_printf("0x%02X", spi_rx_buffer[i]);
			if (i < 8) {
				xil_printf(", ");
			}
		}
		xil_printf("\r\n");
        */
		//xil_printf("X: %d, Y: %d, Z: %d\r\n", x_val, y_val, z_val);
		

        MPU6500_ReadAccel(&Spi1Instance, accel);
        MPU6500_ReadGyro(&Spi1Instance, gyro);
        MPU6500_ConvertGyroToDPS(gyro, gyro_dps);

        //xil_printf("ACC: X=%d Y=%d Z=%d\r\n", accel[0], accel[1], accel[2]);

        //xil_printf("GYRO: X=%f Y=%f Z=%f\r\n", gyro_dps[0], gyro_dps[1], gyro_dps[2]);
        /*xil_printf("GYRO: X=%d.%02d Y=%d.%02d Z=%d.%02d\r\n",
            (int)gyro_dps[0], abs((int)(gyro_dps[0] * 100) % 100),
            (int)gyro_dps[1], abs((int)(gyro_dps[1] * 100) % 100),
            (int)gyro_dps[2], abs((int)(gyro_dps[2] * 100) % 100));
		*/
        XGpio_DiscreteWrite(&Gpio0, LED_CHANNEL, gyro[0]);


		trigger_hcsr04();
		usleep(100); // allow echo to rise if needed

		uint32_t duration_us = measure_distance_us();
		xil_printf("Duration: %lu us\r\n", duration_us);

		usleep(100); // allow echo to rise if needed

 
		/*
        toggle_led(1);
		toggle_led(2);
		toggle_led(3);
		toggle_led(5);
		toggle_led(10);
		toggle_led(16);
		*/

		usleep(10000);
	}

}