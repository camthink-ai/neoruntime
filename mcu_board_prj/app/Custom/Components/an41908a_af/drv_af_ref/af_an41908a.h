/****
 * 
 * af_an41908a.h
 * 
 * Des:
 *   Define AN41908a drvier base infomation.
 * 
 * History:
 *  2015/07/27  - [eric@milesight.cn] Create file.
 * 
 * Copyright (C) 2011-2015, Milesight, Inc.
 * 
 * All rights reserved. No Part of this file may be reproduced, stored
 * in a retrieval system, or transmitted, in any form, or by any means,
 * electronic, mechanical, photocopying, recording, or otherwise,
 * without the prior consent of Milesight, Inc.
 * 
 */


#ifndef __AF_AN41908A_H__
#define __AF_AN41908A_H__

#include "hi3516dv300_gpio_common.h"



 /* Register address definition, and descryption.  */
#if 0
  /* [00h] */
#define	IRS_TGT				// sets the desired value of ADC input for using in Iris control block.
  /* [01h] */
#define	OVER_LPF_FC_1		// ADC feedback filter (1) cut-off frequency
#define OVER_LPF_FC_2		// ADC feedback filter (2) cut-off frequency
#define DEC_AVE				// Moving average of Iris target
#define AS_FLT_OFF			// Filter before PID controller enable / disable
#define ASOUNT_LPF_FC		// Filter cut-off frequency before PID controller
#define DGAIN				// PID controller digital gain
  /* [02h] */
#define IRIS_CALC_NR		// PID controller integral error cumulative prevention level
#define IRIS_ROUND			// PID controller differential error cumulative prevention level
#define PID_ZERO			// PID controller zero point
#define PID_POLE			// PID controller pole
 /* [03h] */
#define ARW					// Number of bits in PID controller integrator
#define LMT_ENB				// PID controller integral stop
#define PWM_FLT_OFF			// LPF after PID controller enable / disable
#define PWM_LPF_FC			// LPF cut-off frequency after PID controller
#define PWM_IRIS			// PWM frequency of Iris block output
#define DT_ADJ_IRIS			// Dead time correction of Iris block output
 /* [04h] */
#define HALL_BIAS_DAC		// Drive current value for hall element
#define HALL_OFFSET_DAC		// Offset adjustment for hall element output amplifier
 /* [05h] */
#define TGT_LPF_FC			// Iris target value LPF cut-off frequency
#define TGT_FLT_OFF			// Iris target value LPF function enable / disable
#define PID_INV				// PID controller polarity
#define HALL_GAIN			// Hall element output amplifier gain
#define AAF_FC				// Cut-off frequency of hall element output amplifier
 /* [06h] */
#define START1				// Pulse 1 start time
 /* [07h] */
#define WIDTH1				// Pulse 1 width
#define P1EN				// Pulse 1 output enable
 /* [08h] */
#define START2				// Pulse 2 start time
 /* [09h] */
#define WIDTH2				// Pulse 2 width
#define P2EN				// Pulse 2 output enable
 /* [0Ah] */
#define TGT_IN_TEST			// Iris output duty direct specified value
#define DUTY_TEST			// Iris output duty direct specification enable
 /* [0Bh] */
#define ASWMODE				// LPF after PID controller enable / disable
#define TESTEN1				// LPF cut-off frequency after PID controller
#define MODESEL_IRIS		// VD_IS polarity selection
#define MODESEL_FZ			// VD_FZ polarity selection
#define PDWNB				// Power down of Iris block
#define ADC_TEST			// ADC read value updated timing
#define PID_CLIP			// Iris output PWM maximum duty
 /* [0Ch] */
#define IRSAD				// ADC output for Iris (read only)
 /* [0Eh] */
#define TGT_UPDATE			// IRS_TGT (iris target) update delay time
#define AVE_SPEED			// Iris target moving average speed
 /* [0Fh] */
 /* [10h] */
 
 /* [20h] */
#define DT1					// Start point wait time
#define PWMMODE				// Micro step output PWM frequency
#define PWMRES				// Micro step output PWM resolution
 /* [21h] */
#define FZTEST				// PLS1/2 pin output signal selection
#define TESTEN2				// Test mode enable 2
 /* [22h] */
#define DT2A				// 12 motor start point excitation wait time
#define PHMODAB				// 12 motor phase correction
 /* [23h] */
#define PPWA				// Driver A peak pulse width
#define PPWB				// Driver B peak pulse width
 /* [24h] */
#define PSUMAB				// 12 motor step count number
#define CCWCWAB				// 12 motor rotation direction
#define BRAKEAB				// 12 motor brake
#define ENDISAB				// 12 motor enable/disable control
#define LEDB				// LED B output control
#define MICROAB				// 12 motor sine wave division number
 /* [25h] */
#define INTCTAB				// 12 motor step cycle
 /* [27h] */
#define DT2B				// 34 motor start point excitation wait time
#define PHMODCD				// 34 motor phase correction
 /* [28h] */
#define PPWC				// Driver C peak pulse width
#define PPWD				// Driver D peak pulse width
 /* [29h] */
#define PSUMCD				// 34 motor step count number
#define CCWCWCD				// 34 motor rotation direction
#define BRAKECD				// 34 motor brake
#define ENDISCD				// 34 motor enable/disable control
#define LEDA				// LED A output control
#define MICROCD				// 34 motor sine wave division number
 /* [2Ah] */
#define INTCTCD				// 34 motor step cycle
 
/**********************************************************************
*	 Data format
*	 [0-7] A0 A1 A2 A3 A4 A5 C0 C1
*	 [8-15] D0 D1 D2 D3 D4 D5 D6 D7
*	 [16-23] D8 D9 D10 D11 D12 D13 D14 D15
*
*	 C0 : Register write / read selection 0 : write mode, 1 : read mode
*	 C1 : Unused
*	 A5 to A0 : Address of register
*
**********************************************************************/
 
#endif
 //1 Caculation Focus and Zoom basic parameters
 //2 Register 0x20h
 /**********************************************************************
			 Addr	 20h Initial Val 0x5C0A  Pls input initial value as "0xXXXX"										 
			 D15 D14 D13 D12 D11 D10 D9  D8  D7  D6  D5  D4  D3  D2  D1  D0
				 PWMRES[14:13]		 PWMMODE[12:8]			 DT1[7:0]							 
 Output 	 0	 1	 0	 1	 1	 1	 0	 0	 0	 0	 0	 0	 1	 0	 1	 0
 Input(BIN)  0	 1	 0	 1	 1	 1	 0	 0	 0	 0	 0	 0	 1	 0	 1	 0
 O/P HEX	 5				 C				 0				 A			 
 
 OSCIN Freq: 27  MHz
 DT1(StartPoint wait) = 3034.07407407407us		 
 Formula:
 PWM frequency = OSC frequency / ((PWMMODE/2^3) * 2^PWMRES) 						 
 PWMMODE[4:0] =  28
 Zoom&Focus PWM freq: 30.1kHz			 30.13392857
 Maximum Duty=PPWx/(PWMMODE*8) = 89.2857142857143%		 
 
			 PWMRES  
			 0		 1		 2
 PWMMODE 
 1			 3375.0  1687.5  843.8 
 2			 1687.5  843.8	 421.9 
 3			 1125.0  562.5	 281.3 
 4			 843.8	 421.9	 210.9 
 5			 675.0	 337.5	 168.8 
 6			 562.5	 281.3	 140.6 
 7			 482.1	 241.1	 120.5 
 8			 421.9	 210.9	 105.5 
 9			 375.0	 187.5	 93.8 
 10 		 337.5	 168.8	 84.4 
 11 		 306.8	 153.4	 76.7 
 12 		 281.3	 140.6	 70.3 
 13 		 259.6	 129.8	 64.9 
 14 		 241.1	 120.5	 60.3 
 15 		 225.0	 112.5	 56.3 
 16 		 210.9	 105.5	 52.7 
 17 		 198.5	 99.3	 49.6 
 18 		 187.5	 93.8	 46.9 
 19 		 177.6	 88.8	 44.4 
 20 		 168.8	 84.4	 42.2 
 21 		 160.7	 80.4	 40.2 
 22 		 153.4	 76.7	 38.4 
 23 		 146.7	 73.4	 36.7 
 24 		 140.6	 70.3	 35.2 
 25 		 135.0	 67.5	 33.8 
 26 		 129.8	 64.9	 32.5 
 27 		 125.0	 62.5	 31.3 
 28 		 120.5	 60.3	 30.1 
 29 		 116.4	 58.2	 29.1 
 30 		 112.5	 56.3	 28.1 
 31 		 108.9	 54.4	 27.2 
 ***********************************************************************/
 //2 Register 21h 22h/27h
 /**********************************************************************
			 Addr	 21h	 Initial Val 0x0000  Pls input initial value as "0xXXXX"										 
			 D15 D14 D13 D12 D11 D10 D9  D8  D7  D6  D5  D4  D3  D2  D1  D0
											 TESTEN2[7] 	 DFZTEST[4:0]				 
 Output 	 0	 0	 0	 0	 0	 0	 0	 0	 0	 0	 0	 0	 0	 0	 0	 0
 Input(BIN)  0	 0	 0	 0	 0	 0	 0	 0	 0	 0	 0	 0	 0	 0	 0	 0
 O/P HEX	 0				 0				 0				 0																	 
 
 =====================================================
			 Addr	 22h/27h	 Initial Val 0x0003  Pls input initial value as "0xXXXX"										 
			 D15 D14 D13 D12 D11 D10 D9  D8  D7  D6  D5  D4  D3  D2  D1  D0
					 PHMODAB[13:8]			 DT2A[7:0]							 
 Output 	 0	 0	 0	 0	 0	 0	 0	 0	 0	 0	 0	 0	 0	 0	 1	 1
 Input(BIN)  0	 0	 0	 0	 0	 0	 0	 0	 0	 0	 0	 0	 0	 0	 1	 1
 O/P HEX	 0				 0				 0				 3			 
																 
 OSCIN Freq: 27  MHz
 DT2A time	 =910.222222222222us			 PHMODAB =0degree	 
 (Start point excitation wait)					 (Resolution=360degree/512=0.7degree)
																 
 The same setting register:27h
 ***********************************************************************/
 
 //2 Register 23h/28h		 24h/29h	 25h/2Ah
 /**********************************************************************
			 Addr	 23h/28h	 Initial Val 0xC8C8  Pls input initial value as "0xXXXX"										 
			 D15 D14 D13 D12 D11 D10 D9  D8  D7  D6  D5  D4  D3  D2  D1  D0
			 PPWB[15:8] 					 PPWA[7:0]							 
 Output 	 1	 1	 0	 0	 1	 0	 0	 0	 1	 1	 0	 0	 1	 0	 0	 0
 Input(BIN)  1	 1	 0	 0	 1	 0	 0	 0	 1	 1	 0	 0	 1	 0	 0	 0
 O/P HEX	 C				 8				 C				 8			 
 
 DT1 Maximum Duty=PPWx/(PWMMODE*8)
 DT1	 =89.2857142857143%
 
 The same setting register:28h
 =====================================================
			 Addr	 24h Initial Val 0x0400  Pls input initial value as "0xXXXX"										 
			 D15 D14 D13 D12 D11 D10 D9  D8  D7  D6  D5  D4  D3  D2  D1  D0
			 MICROAB[13:12]  LEDB[11]	 ENDISAB[10] BRAKEAB[9] CCWCWAB[8] PSUMAB[7:0]						 
 Output 	 0	 0	 0	 0	 0	 1	 0	 0	 0	 0	 0	 0	 0	 0	 0	 0
 Input(BIN)  0	 0	 0	 0	 0	 1	 0	 0	 0	 0	 0	 0	 0	 0	 0	 0
 O/P HEX	 0				 4				 0				 0			 
 
 
 MICROAB	 Num of division		 MICROAB[1:0] = 0
 0			 256					 Num of division =	  256
 1			 256 
 2			 128 
 3			 64  
																 
 The same setting register:29h
 =====================================================
			 Addr	 25h Initial Val 0x0160  Pls input initial value as "0xXXXX"										 
			 D15 D14 D13 D12 D11 D10 D9  D8  D7  D6  D5  D4  D3  D2  D1  D0
			 INTCTAB[15:0]															 
 Output 	 0	 0	 0	 0	 0	 0	 0	 1	 0	 1	 1	 0	 0	 0	 0	 0
 Input(BIN)  0	 0	 0	 0	 0	 0	 0	 1	 0	 1	 1	 0	 0	 0	 0	 0
 O/P HEX	 0				 1				 6				 0			 
 
 eg,	 1)  OSCIN Freq: 27  MHz 
		 rotation freq:  100 Hz  = 800PPS (100Hz * 8pulse)
		 VD freq:	 60  Hz
		 INTCTxx=	 352		 
		 1.  INTCTxx[15:0] * 768 = OSCIN frequency / rotation frequency 					 
	 2)  PSUMxx  53 	 
		 2.  INTCTxx[15:0] * PSUMxx[7:0] * 24 = OSCIN frequency / VD frequency						 
	 3)  Since PSUMxx[7:0] is rounded off, recalculate INTCTxx[15:0] from equation in 2):
		 INTCTxx=	 354
					 
 The same setting register:2Ah
 =====================================================
 According to our setting INTCTAB[15:0],calcurate rotation freq: 99.87571023 Hz
 
 For 1-2phase exitation or stepping motor a,b;each cycle have 8 pulse.
 ***********************************************************************/
 
 
/* Define zoom and focus steps reg */
#if 1
// define AB motor to Focus, CD motor to Zoom
#define AN41908A_FOCUS_STEPS_REG  	0x24
#define AN41908A_ZOOM_STEPS_REG 	0x29
#define AN41908A_FOCUS_PPS_REG		0x25
#define AN41908A_ZOOM_PPS_REG		0x2A
#define AN41908A_FOCUS_PI_REG		0x24
#define AN41908A_ZOOM_PI_REG		0x29
#else
 // Define AB motor to Zoom, CD motor to Focus
#define AN41908A_FOCUS_STEPS_REG	0x29
#define AN41908A_ZOOM_STEPS_REG		0x24
#define AN41908A_FOCUS_PPS_REG		0x2A
#define AN41908A_ZOOM_PPS_REG		0x25
#define AN41908A_FOCUS_PI_REG		0x29
#define AN41908A_ZOOM_PI_REG		0x24
#endif
 
// Diffrent micro step PSUMxx to step count is different.
/*
	64-step	 step count = PSUMxx*2
	128-step	 step count = PSUMxx*4
	256-step	 step count = PSUMxx*8
*/
#define MICRO_STEP_MODE			256
#if (MICRO_STEP_MODE == 256)
	#define PSUM_TO_STEP		(8)
	#define DISTANCE_TO_MICRO	(32)
	#define MICROXX				(0x0 << 12)
#elif (MICRO_STEP_MODE == 128)
	#define PSUM_TO_STEP		(4)
	#define DISTANCE_TO_MICRO	(16)
	#define MICROXX				(0x2 << 12)
#elif (MICRO_STEP_MODE == 64)
	#define PSUM_TO_STEP		(2)
	#define DISTANCE_TO_MICRO	(8)
	#define MICROXX				(0x3 << 12)
#else
	#error "Unknow step mode"
#endif
 
#define MICRO_8_STEP			(8)
#define AN41908A_OSCIN			(27*1000*1000)	// 27M
 // 16HZ -> PSUMxx = 200
#define VD_FREQ_DEF				(16)	
 
 // Define an41906a control
#define PULSE_ON				(0x1 << 11)
 
 //#define INTCTXX(pps) 		 (AN41908A_OSCIN/(pps/MICRO_8_STEP)/768)
 //#define VD_PSUMXX(pps)		 (AN41908A_OSCIN/VD_FREQ_DEF/24/INTCTXX(pps))
 //#define VD_INTCTXX(pps)		 (AN41908A_OSCIN/VD_FREQ_DEF/VD_PSUMXX(pps))
 
#define VD_MDELAY_DEF			(1000/VD_FREQ_DEF)
 
 /*32 = (MICRO_STEP_MODE/VD_FREQ_DEF)*/
#define VD_PSUMXX(pps)			 ((pps<=0)?1:(((32*(pps/MICRO_8_STEP)/VD_FREQ_DEF)>=1)?(32*(pps/MICRO_8_STEP)/VD_FREQ_DEF):(1))) 
#define VD_INTCTXX(psumxx)		 (AN41908A_OSCIN/VD_FREQ_DEF/(psumxx*24))

 
 //#define VD_PSUMXX(pps)			 (MICRO_STEP_MODE/PSUM_TO_STEP*(pps/MICRO_8_STEP/VD_FREQ_DEF)
 //#define VD_INTCTXX(psumxx)		 (AN41908A_OSCIN/VD_FREQ_DEF/(psumxx*24))
 
 
// Define Focus/Zoom opration 
/*
 Motor step count(for 64-step):
 Step count = PSUMxx(decimal digit) x 2 x VD cycle count 
 For example:
	 PSUMxx = 88, VD = 5 cycle (VD Count1~5)
	 PSUMxx = 32, VD = 1cycle (VD Count6)
	 Step count = (88 x 2 x 5) + (32 x 2 x 1) = 944 step

 Focus speed: 3.0s (800 pps) 0.005mm/step		 
 Max lenght 12.024mm
 8-step sequence conversion => 12.024mm/0.005mm = 2404.8 step
 64-step sequence conversion = 8-step sequence*8 = 2404.8*8 = 19238.4
 800 pps = 800/8 Hz = 100Hz
 1VD cycle is 100/50 = 2 cycle
 1VD step is 100/50*64 = 128 step
 Max length step / 1VD step =  19238.4 / 128 = 150.3 VD
 VD frequency = 50Hz -> 1VD time = 20 msec
 Max length time = 150.3 VD * 20 msec = 3.006 sec -> 3.0s
 
 
 Zoom speed: 1.56s (800 pps) 0.005mm/step
 Max lenght 6.256mm
 8-step sequence conversion => 6.256mm/0.005mm = 1251.2 step
 64-step sequence conversion = 8-step sequence*8 = 1251.2*8 = 10009.6
 800 pps = 800/8 Hz = 100Hz
 1VD cycle is 100/50 = 2 cycle
 1VD step is 100/50*64 = 128 step
 Max length step / 1VD step = 10009.6 / 128 = 78.2 VD
 VD frequency = 50Hz -> 1VD time = 20 msec
 Max length time = 78.2 VD * 20 msec = 1.564 sec ->1.56s 
	 
#define AF_MAX_ZOOM_IDX		(390)
#define AF_MIN_ZOOM_IDX		(-850)
#define AF_MAX_FOCUS_IDX	(1610+50)
#define AF_MIN_FOCUS_IDX	(-793-60)
#define FOCUS_MAX_PPS			(800)
#define ZOOM_MAX_PPS			(800)
 */
 
// Define [temron DF010N000]  Focus/Zoom opration 
/*
 Motor step count(for 64-step):
 Step count = PSUMxx(decimal digit) x 2 x VD cycle count 
 For example:
	 PSUMxx = 88, VD = 5 cycle (VD Count1~5)
	 PSUMxx = 32, VD = 1cycle (VD Count6)
	 Step count = (88 x 2 x 5) + (32 x 2 x 1) = 944 step

 Focus speed: 1.92s (960 pps)	 0.00625mm/step 	 
 Max lenght 11.492 mm
 8-step sequence conversion => 11.492mm/0.00625mm = 1838.72 step
 64-step sequence conversion = 8-step sequence*8 = 1838.72*8 = 14709.76
 960 pps = 960/8 Hz = 120 Hz
 1VD cycle is 120/60 = 2 cycle
 1VD step is 120/60*64 = 128 step
 Max length step / 1VD step =  14709.76 / 128 = 114.92 VD
 VD frequency = 60Hz -> 1VD time = 16.667 msec
 Max length time = 114.92 VD * 16.667 msec = 1.915 sec -> 1.92s
 
 
 Zoom speed: 0.63s (960 pps) 0.01mm/step
 Max lenght 6.067mm
 8-step sequence conversion => 6.067mm/0.01mm = 606.7 step
 64-step sequence conversion = 8-step sequence*8 = 606.7*8 = 4853.6
 960 pps = 960/8 Hz = 120Hz
 1VD cycle is 120/60 = 2 cycle
 1VD step is 120/60*64 = 128 step
 Max length step / 1VD step = 4853.6 / 128 = 37.92 VD
 VD frequency = 60Hz -> 1VD time = 16.667 msec
 Max length time = 37.92 VD * 16.667 msec = 0.63199 sec ->0.632s 
 
#define AF_MAX_ZOOM_IDX		(421)
#define AF_MIN_ZOOM_IDX		(-185)
#define AF_MAX_FOCUS_IDX		(544+80)
#define AF_MIN_FOCUS_IDX		(-1224-71)
#define FOCUS_MAX_PPS			(960)
#define ZOOM_MAX_PPS			(960)
*/

#endif /*__AF_AN41908A_H__*/

