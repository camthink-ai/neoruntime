/****
 * 
 * af_bu24036mwv.h
 * 
 * Des:
 *   Define bus4025mwv driver base information.
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


#ifndef __AF_BU24036MWV_H__
#define __AF_BU24036MWV_H__

#include "hi3516dv300_gpio_common.h"


#define	AF1_STATE11			MS_GPIO(5,2)	// for focus busy status pin
#define	AF1_STATE12			MS_GPIO(5,0)	// for focus cache status pin
#define AF1_STATE21			MS_GPIO(5,1)	// for zoom busy status pin
#define AF1_STATE22			MS_GPIO(5,3)	// for zoom cache status pin

#define	M24_AF1_STATE11			MS_GPIO(7,0)	// for focus busy status pin
#define	M24_AF1_STATE12			MS_GPIO(7,1)	// for focus cache status pin

#define	AF1_IRCUT			IRCUT_IO

#define STATE11_MUX		(36)
#define STATE11_MUX_DATA	(0x0)

#define STATE12_MUX		(34)
#define STATE12_MUX_DATA	(0x0)

#define STATE21_MUX		(35)
#define STATE21_MUX_DATA	(0x0)

#define STATE22_MUX		(37)
#define STATE22_MUX_DATA	(0x0)

#define IRCUT_MUX		(31)
#define IRCUT_MUX_DATA	(0x1)

//#define M24_STATE11_MUX		(53)
//#define M24_STATE11_MUX_DATA	(0x0)

//#define M24_STATE12_MUX		(54)
//#define M24_STATE12_MUX_DATA	(0x0)


/* Register address definition, and descryption.  */
#if 0
//1 Initial setting
/* [0xCh] */
#define CLK_DIV	// Set "fmain" (the clock supplied to the main logic).
#define CLK_EN	// Set ON/OFF of "fmain" (the clock supplied to the main logic).
/* [0xBh] */
#define EXT_CTL	// Set the control of 3ch, 5ch, and 6ch driver.
/* [0xEh] */
#define CMD_RST	// Set Command-reset.
#define STB		// Set "Stand-by" (the mode for low power consumption at preserved registers).
//1 Stepping motor driver
/* [Bh] */
#define A_CTL	// Set the control mode of stepping motor.
#define A_ANSEL	// Set the method of Autonomous mode.

/* [Ch] */
#define CHOPPING	// Set PWM chopping frequency for stepping motor driver.
#define STM_RST	// Set "STM-reset".

/* [0x1h] */
#define A_PS	// Set Power ON/OFF of stepping motor driver.
/* [0x5h] */
#define B_PS	// Set Power ON/OFF of stepping motor driver.
/* [0xC] */
#define CACHE_M	// Set the mode of Cache register.
/* [0xB] */
#define EDGE	// (use in CLockIn mode)Set the mode of the detecting edge.


/* [0h] */
#define A_MODE	// CHA Clock IN and Autonomous(Cache/Up down) mode.
#define A_SEL	// CHA Mode edge/signal
#define A_different_out_voltage	// CHA Set the different output voltage between OUTxA and OUTxB.
/* [1h] */
#define A_CYCLE	// CHA Set the frequency for stepping motor rotation (128 * x pps)
#define A_BEXC	// CHA Set ON/OFF of the pre-excitation.
#define A_AEXC	// CHA Set ON/OFF of the post-excitation.
#define A_BSL	// CHA Set time of the pre-excitation.
#define A_ASL	// CHA Set time of the post-excitation.
#define A_STOP	// CHA Set the execution of the forced interruption.
#define A_UPDW_STOP	// CHA Set the forced down interruption.
#define A_POS	// CHA Set the stop position of the motor at the forced interruption.
#define A_START_POS // CHA Set the start position for the stepping motor.


/* [2h] */
#define A_PULES	// CHA Set the amount of rotation.
#define A_UPDW_Cycle	// CHA the frequency of rotation
#define A_RT // Set the rotating direction of stepping motor.
#define A_EN // Set the excitation/unexcitation.

/* [4h] */
#define B_MODE	// CHB Clock IN and Autonomous(Cache/Up down) mode.
#define B_SEL	// CHB Mode edge/signal
#define B_different_out_voltage	// CHB Set the different output voltage between OUTxA and OUTxB.

/* [5h] */
#define B_CYCLE	// CHB Set the frequency for stepping motor rotation (128 * x pps)
#define B_BEXC	// CHB Set ON/OFF of the pre-excitation.
#define B_AEXC	// CHB Set ON/OFF of the post-excitation.
#define B_BSL	// CHB Set time of the pre-excitation.
#define B_ASL	// CHB Set time of the post-excitation.
#define B_STOP	// CHB Set the execution of the forced interruption.
#define B_UPDW_STOP	// CHB Set the forced down interruption.
#define B_POS	// CHB Set the stop position of the motor at the forced interruption.
#define B_START_POS // CHB Set the start position for the stepping motor.
// Other configure
#define 3_CHOP	// Set the PWM chopping frequency for 3ch driver.
#define 4_CHOP	// Set the PWM chopping frequency for 4ch driver.
#define 3_STATE_CTRL	// Set the PWM chopping frequency for 3ch driver.
#define 4_STATE_CTRL	// Set the PWM chopping frequency for 4ch driver.
#define 3_PWM_DUTY	// Set the PWM duty ratio of 3ch driver.
#define 4_PWM_DUTY	// Set the PWM duty ratio of 4ch driver.


/* [6h] */
#define A_PULES	// CHA Set the amount of rotation.
#define A_UPDW_Cycle	// CHA the frequency of rotation
#define A_RT // Set the rotating direction of stepping motor.
#define A_EN // Set the excitation/unexcitation.


/* [0xCh] */
#define 5_MODE	// Set the 5ch driving mode

/* [0xEh] */
#define 5_CHOP	// Set the PWM chopping frequency for 5ch driver.
#define 5_STATE_CTRL	// Common to the voltage driver and current driver mode.
#define 5_PWM_DUTY	// Set the PWM duty ratio of 5ch driver.
#define 5_IOUT	// Set the output current value of 5ch current driver.

//1 Speed contol.
/* [0xDh] */
#define SPEN	// Select the driver which uses the speed control.
#define DET_SEL	// Set the mode of the speed detect at Speed control mode =ON.
#define TARSP	// Set the target speed at Speed control mode =ON.
#define PSP	// Set the coefficient of proportion at Speed control mode =ON.
#define ISP	// Set the coefficient of integrator at Speed control mode =ON.
#define SPC_LIMIT	// Set the limit of the PWM duty ratio at Speed control mode =ON.
#define SPC_LIMIT_OUT	// Select the output signal of STATE22 pin.


//1 6CH currnt control
/* [0xEh] */
#define 6_STATE_CTL	// Set the state of 6ch driver and PI driver(2ch) at Register control.
#define 6_IOUT	// Set the output current value of 6ch current driver.

//1 PI driver circuit
/* [0xDh] */
#define PI_CTL1	// Set ON/OFF to PI driver.
#define PI_CTL2 // Set ON/OFF to PI driver.


//1 Wave forming circuit
/* [0xEh] */
#define Waveform_Vthh // Set L -> H the detection voltage of wave forming circuit.
#define Waveform_Vthl // Set H -> L the detection voltage of wave forming circuit.



/**********************************************************************
*	 Data format
*	 [15-12] A15 A14 A13 A12
*	 [11-8] D11 D10 D9 D8
*	 [7-0]  D7 D6 D5 D4 D3 D2 D1 D0
*
*	 A15 to A12 : Address of register
*
**********************************************************************/
#endif

//1 Clock IN mode.
/**********************************************************************
[u-step mode, the detecting edge=rising only, the electrical angle cycle=512 edge, Input 512 clock to STATE pin]
command 1:	Command-reset											----- CMD_RS
command 2: 	Released from Command-reset and Stand-by					----- CMD_RS & STB
command 3: 	Set the clock supplied to the main logic, set to clock=ON 		----- CLK_DIV & CLK_EN
command 4: 	Released from STM-reset									----- STM_RS
command 5: 	Set the control mode of stepping motor (Clock IN mode)		----- A/B_CTL
		      	Set the mode of the detecting edge (Rising only)				----- Edge
command 6: 	Set PWM chopping frequency for stepping motor driver		----- Chopping
command 7: 	Set the mode of stepping motor control (u-step)				----- A/B_Mode
			Set the number of edge for the electrical angle cycle(512 edges) ----- A/B_SEL
			Set the different output voltage 							----- A/B_different_output_voltage
command 8:	Set Power ON/OFF of stepping motor driver					----- A/B_PS
command 9:	Set the excitation/un excitation of driver(excitation)			----- A/B_EN
			Set the rotating direction 									----- A/B_RT
command 10:Set the excitation/un excitation of driver(unexcitation)		----- A/B_EN

**********************************************************************/
//1 Autonomous mode (Cache method). 
/**********************************************************************
[u-step mode, the pre-excitation=ON, the post-excitation=ON, 1 rotation]
command 1:	Command-reset. 												----- CMD_RS
command 2:	Released from Command-reset and Stand-by.					----- CMD_RS+STB
command 3:	Set the clock supplied to the main logic, set to clock=ON. 	 		----- CLK_DIV+CLK_EN
command 4:	Released from STM-reset.										----- STM_RS
command 5:	Set the control mode of stepping motor.(Autonomous mode) 	   	----- A/B_CTL
command 6:	Set PWM chopping frequency for stepping motor driver.			----- Chopping
			Set the mode of Cache register. 								----- CacheM
command 7:	Set the mode of stepping motor control.(u-step) 					----- A/B_Mode
			Set the output signal from STATE pin							----- A/B_SEL
			Set the different output voltage									----- A/B_different_output_voltage
command 8,9:	Set the frequency for stepping motor rotation					----- A/B_Cycle
command 10:	Set ON/OFF of the pre-excitation.(ON)						----- A/B_BEXC
				Set time of the pre-excitation. 								----- A/B_BSL
				Set ON/OFF of the post-excitation.(ON)						----- A/B_AEXC
				Set time of the post-excitation.								----- A/B_ASL
command 11:	Set Power ON/OFF of stepping motor driver					----- A/B_PS
command 12:	Set the excitation/un excitation of driver(excitation) 		 	----- A/B_EN
				Set the rotating direction									----- A/B_RT
				Set the amount of rotation									----- A/B_Pulse
**********************************************************************/
//1 Autonomous mode (Up down method ). 
/**********************************************************************
[u-step mode, the pre-excitation=ON, the post-excitation=ON, Up/Down 1 rotation, Constant 4 rotation]
command 1: 	Command-reset.												----- CMD_RS
command 2: 	Released from Command-reset and Stand-by.					----- CMD_RS+STB
command 3: 	Set the clock supplied to the main logic, set to clock=ON.			----- CLK_DIV+CLK_EN
command 4: 	Released from STM-reset.										----- STM_RS
command 5: 	Set the control mode of stepping motor.(Autonomous Up down)	----- A/B_CTL+A/B_ANSEL
command 6: 	Set PWM chopping frequency for stepping motor driver.			----- Chopping
command 7: 	Set the mode of stepping motor control.(u-step)					----- A/B_Mode
			Set the output signal from STATE pin 							----- A/B_SEL
			Set the different output voltage 								----- A/B_different_output_voltage
command 8: 	Set the coefficient of frequency for stepping motor rotation			----- A/B_Cycle
command 9: 	Set ON/OFF of the pre-excitation.(ON)							----- A/B_BEXC
			Set time of the pre-excitation.									----- A/B_BSL
			Set ON/OFF of the post-excitation.(ON)							----- A/B_AEXC
			Set time of the post-excitation. 								----- A/B_ASL
command 10:	Set Power ON/OFF of stepping motor driver					----- A/B_PS
command 11:	Set the excitation/un excitation of driver(excitation)			----- A/B_EN
				Set the frequency of Up, Down rotation A 					----- A/B_UPDW_Cycle
command 12:	Set the frequency of Up, Down rotation B					----- A/B_UPDW_Cycle
command 13:	Set the frequency of Up, Down rotation C					----- A/B_UPDW_Cycle
command 14:	Set the frequency of Up, Down rotation D					----- A/B_UPDW_Cycle
command 15:	Set the frequency of Constant rotation E						----- A/B_UPDW_Cycle
command 16:	Set the amount of Up, Down rotation F						----- A/B_Pulse
command 17:	Set the amount of Constant rotation G (lower 10bit) 			----- A/B_Pulse
command 18:	Set the amount of Constant rotation G (upper 6bit)			----- A/B_Pulse
				Set the rotating direction									----- A/B_RT

**********************************************************************/

//1 All registers define
/**********************************************************************/
#define BU24036MWV_FCLK		(27*1000*1000)	// 27M
#define BU24036MWV_FMIN		(BU24036MWV_FCLK)	// CLK_DIV = 0
#define PPS_2_CYCLE(pps)	(BU24036MWV_FMIN/(pps*128))


/* channel12 on the motor driver */
#define CHN12_MODE			0x0000		/* Channels 1 and 2 A_MODE [11:10] A_SEL[9:7] A_diff_out_valtage[6:0] */
#define	CHN12_SPEED_H		0x1200		/* Channels 1 and 2 pps [13:6] */
#define	CHN12_SPEED_L		0x1000		/* Channels 1 and 2 pps [5:0] */
#define CHN12_START_POS		0x1400		/* Channels 1 and 2 start_pos [3:0] */
#define CHN12_EXCITATION	0x1600		/* Channels 1 and 2 A_BEXC[7] A_BSL[4]  A_AEXC[3] A_ASL[0]  */
#define CHN12_PW_CTRL		0x1e00		/* Channels 1 and 2 A_POS[5:4] A_UPDW_Stop[2]  A_PS[1] A_STOP[0]  */
#define CHN12_OP_CTRL		0x2000		/* Channels 1 and 2 run control: A_EN[11] A_RT[10]  A_Pulse/A_UPDW_cycle[9:0]  */

/* channel34 on the motor driver */
#define CHN34_MODE			0x4000		/* Channels 3 and 4 B_MODE [11:10] B_SEL[9:7] B_diff_out_valtage[6:0] */
#define	CHN34_SPEED_H		0x5200		/* Channels 3 and 4 pps [13:6] */
#define	CHN34_SPEED_L		0x5000		/* Channels 3 and 4 pps [5:0] */
#define CHN34_START_POS		0x5400		/* Channels 3 and 4 start_pos [3:0] */
#define CHN34_EXCITATION	0x5600		/* Channels 3 and 4 B_BEXC[7] B_BSL[4]  B_AEXC[3] B_ASL[0] */
#define CHN34_PW_CTRL		0x5e00		/* Channels 3 and 4 B_POS[5:4] B_UPDW_Stop[2]  B_PS[1] B_STOP[0]  */
#define CHN34_OP_CTRL		0x6000		/* Channels 3 and 4 run control: B_EN[11] B_RT[10]  B_Pulse/B_UPDW_cycle[9:0]  */
#define CHN3_CFG			0x5a00		/* Channels 3 3_STATE_CTL[8:7] 3_PWM_DUTY[6:0]  */
#define CHN4_CFG			0x5b00		/* Channels 4 4_STATE_CTL[8:7] 4_PWM_DUTY[6:0]  */

/* channe12 and channel34 on the motor driver */
#define STEPPER_CFG			0xb000		/* StepperA/B B_ANSEL[7] A_ANSEL[6] Edge[5] B_CTL[1] A_CTL[0] */
#define EXT_CTRL			0xb200		/* Channels 5 and 6. EXT_CTL[1:0] */


/* channe5 on the motor driver */
#define CHN5_MODE			0xc000		/* Channels 5 mode configure */
#define CHN5_IOUT			0xe000		/* Channels 5 currnt out, 5_IOUT[7:0] */
#define CHN5_PWM			0xe200		/* Channels 5 PWM, 5_PWM_DUTY[6:0] */
#define CHN5_CTRL			0xe400		/* Channels 5 cfg, 5_CHOP[5:4] 5_STATE_CTL[1:0] */

/* channe6 on the motor driver */
#define CHN6_STATE			0xe600		/* Channels 6 STATE, 6_STATE_CTL[2:0] */
#define CHN6_IOUT			0xe800		/* Channels 6 currnt out, 6_IOUT[7:0] */

/* common configure of the motor driver */
#define COMM_CLK_CACHE		0xc000		/* Common cfg, Chopping[9:8] CacheM[7] 5_Mode[5] CLK_EN[4] CLK_DIV[3:0]  */
#define COMM_PI_CTRL		0xd000		/* Common cfg, PI_CTL[1] PI_CTL[0]  */
#define COMM_SPEED_CTRL		0xd200		/* Common speed, DET_SEL[7] SPEN[5:4]  */
#define COMM_TAR_SPEED		0xd600		/* Common speed target, TARSP[7:0] */
#define COMM_SPEED_COEF		0xd700		/* Common speed target, PSP[6:4] ISP[3:0] */
#define COMM_SPEED_LIMIT	0xd800		/* Common speed limit, SPC_LIMIT_OUT[7] SPC_LIMIT[3:0] */

/* common command configure */
#define COMM_WAVEFORM_VH	0xea00		/* Common wave form set, Waveform_Vthh[5:0]*/
#define COMM_WAVEFORM_VL	0xeb00		/* Common wave form set, Waveform_Vthl[5:0]*/	
#define COMM_RST			0xec00		/* Common reset, STB[4]  STM_RS[1] CMD_RS[0]*/


/* Stepper micro step mode control. */
#define MICRO_U_STEP		(0x0 << 10)	// 1/4, 1024 step
#define MICRO_FULL_STEP		(0x1 << 10)	// 1
#define MICRO_HALF_STEP		(0x2 << 10)	// 1/2
#define MICRO_NM_STEP		(0x3 << 10)	// only use in channel34 for Independent drive brush motor

/* Stepper control mode and autonomous mode configure. */
#define ST_A_AUTO			(0x0 << 0) //autonomous mode
#define ST_A_CLK_IN			(0x1 << 0) // clock in mode
#define ST_B_AUTO			(0x0 << 1)
#define ST_B_CLK_IN			(0x1 << 1)
// In auto mode, we must set cache/up-down mode.
#define ST_A_CACHE			(0x0 << 6)
#define ST_A_UPDN			(0x1 << 6)
#define ST_B_CACHE			(0x0 << 7)
#define ST_B_UPDN			(0x1 << 7)

/* Stepper statex1 and statetx2 control. */
//4 Only use in Autonomous mode (Up down method ). 
#define STATE_MO_BUSY		(0x0 << 7) // statex1 = mo, statex2 = busy
#define STATE_BUSY_ACT		(0x2 << 7) // statex1 = busy, statex2 = act
#define STATE_MOEN_BUSY		(0x4 << 7) // statex1 = mo&en, statex2 = busy
#define STATE_ACT_MO		(0x6 << 7) // statex1 = act, statex2 = mo

/* Stepper edge control. */
//4 Only use in ClockIn mode. 
#define EDGE_4				(0x0 << 7)
#define EDGE_8				(0x1 << 7)
#define EDGE_32				(0x2 << 7)
#define EDGE_64				(0x3 << 7)
#define EDGE_128			(0x4 << 7)
#define EDGE_256			(0x5 << 7)
#define EDGE_512			(0x6 << 7)
#define EDGE_1024			(0x7 << 7)
#define EDGE_RS_ONLY		(0x0 << 5)
#define EDGE_RS_FL			(0x1 << 5)

/* Stepper cache number control. */
#define CACHEM_1			(0x1 << 7)
#define CACHEM_2			(0x0 << 7)

/* Common register reset control. */
#define STB_ON				(0x0 << 4)
#define STB_OFF				(0x1 << 4)
#define RST_ON				(0x0 << 0)
#define RST_OFF				(0x1 << 0)
#define STM_RST_ON			(0x0 << 1)
#define STM_RST_OFF			(0x1 << 1)

/* Common register clock control. */
// Fmain = FCLK * CLK_DIV_n
#define CLK_ON				(0x1 << 4)
#define CLK_OFF				(0x0 << 4)
#define CLK_DIV_1			(0x0 << 0)
#define CLK_DIV_1_5			(0x1 << 0)
#define CLK_DIV_2			(0x2 << 0)
#define CLK_DIV_3			(0x3 << 0)
#define CLK_DIV_4			(0x4 << 0)
#define CLK_DIV_6			(0x5 << 0)
#define CLK_DIV_8			(0x6 << 0)
#define CLK_DIV_12			(0x7 << 0)
#define CLK_DIV_16			(0x8 << 0)
#define CLK_DIV_24			(0x9 << 0)

/* Common register chopping control. */
// PWM_freq = Fmain / (32* PWM_DIV_n)
#define PWM_DIV_4			(0x1 << 8)
#define PWM_DIV_5			(0x2 << 8)
#define PWM_DIV_6			(0x3 << 8)

/* Common register clk and channel5 control. */
#define COMM_CLK_CFG		(COMM_CLK_CACHE | CLK_ON | CLK_DIV_1 | CACHEM_2)

#define CHN5_MODE_VOLATGE	(0x0 << 5)
#define CHN5_MODE_CURRENT	(0x1 << 5)
#define CHN5_MODE_CFG		(COMM_CLK_CFG | CHN5_MODE_VOLATGE)

/* Stepper output volatge control . */
// DVDD=3.3V, MVCC=5V, OUT = DVDD*2/128*0x64 = 5V.(max=MVCC)
#define	VOLT_5v				(0x64 << 0)	/* Constant current value */
#define	VOLT_4_5v			(0x57 << 0)
#define	VOLT_4v				(0x4e << 0)
#define	VOLT_3_5v			(0x44 << 0)
#define	VOLT_3_4v			(0x42 << 0)
#define VOLT_3_3v			(0x40 << 0)
#define VOLT_3_2v			(0x3e << 0)
#define VOLT_3_1v			(0x3c << 0)
#define VOLT_3v				(0x3a << 0)

/* Stepper output frequency control . */
// PPS = fmain/(128*x), x = fmain/128/pps
#define CYCLE_H(pps)		((PPS_2_CYCLE(pps) >> 6) & 0x3fc)
#define CYCLE_L(pps)		(PPS_2_CYCLE(pps) & 0x3f)

/* Stepper Power State control. */
#define PS_ON				(0x1 << 1)
#define PS_OFF				(0x0 << 1)
#define	PS12_ON				PS_ON
#define	PS12_OFF			PS_OFF
#define	PS34_ON				PS_ON
#define	PS34_OFF			PS_OFF

/* Stepper excitation on control. */
#define PRE_EXCIT_ON		(0x1 << 7)
#define PRE_EXCIT_OFF		(0x0 << 7)
#define POST_EXCIT_ON		(0x1 << 3)
#define POST_EXCIT_OFF		(0x0 << 3)

#define CHN12_EXC_OFF		(CHN12_EXCITATION | PRE_EXCIT_OFF | PRE_EXCITIME_0 | POST_EXCIT_OFF | POST_EXCITIME_0)
#define CHN34_EXC_OFF		(CHN34_EXCITATION | PRE_EXCIT_OFF | PRE_EXCITIME_0 | POST_EXCIT_OFF | POST_EXCITIME_0)
#define CHN12_EXC_ON		(CHN12_EXCITATION | PRE_EXCIT_ON | PRE_EXCITIME_0 | POST_EXCIT_ON| POST_EXCITIME_0)
#define CHN34_EXC_ON		(CHN34_EXCITATION | PRE_EXCIT_ON | PRE_EXCITIME_0 | POST_EXCIT_ON | POST_EXCITIME_0)
#define CHN12_EXC_ON1		(CHN12_EXCITATION | PRE_EXCIT_ON | PRE_EXCITIME_0 | POST_EXCIT_OFF| POST_EXCITIME_0)
#define CHN34_EXC_ON1		(CHN34_EXCITATION | PRE_EXCIT_ON | PRE_EXCITIME_0 | POST_EXCIT_OFF | POST_EXCITIME_0)
#define CHN12_EXC_ON2		(CHN12_EXCITATION | PRE_EXCIT_OFF | PRE_EXCITIME_0 | POST_EXCIT_ON| POST_EXCITIME_0)
#define CHN34_EXC_ON2		(CHN34_EXCITATION | PRE_EXCIT_OFF | PRE_EXCITIME_0 | POST_EXCIT_ON | POST_EXCITIME_0)


/* Stepper  excitation time control . */
//4 Only use in Autonomous mode.
// Time = 1/fmain*120000, when fmain = 24M, EXCITIME_0 = 5ms, EXCITIME_1 = 10ms
#define PRE_EXCITIME_0		(0x0 << 4)	/* 5ms */
#define PRE_EXCITIME_1		(0x1 << 4)	/* 10ms */
#define POST_EXCITIME_0		(0x0 << 0)	/* 5ms */
#define POST_EXCITIME_1		(0x1 << 0)	/* 10ms */

/* Stepper directory control. */
#define ST_FORWARD			(0x0 << 10)
#define ST_REVERSE			(0x1 << 10)

/* Stepper enable control. */
#define ST_ON				(0x1 << 11)
#define ST_OFF				(0x0 << 11)

/* Pulse configure .*/
#define STEP_2_PULSE(step)	(step << 2) //[9:0],  use 16 u-step

/* Stepper PI state control. */
#define PI1_ON				(0x1 << 0)
#define PI1_OFF				(0x0 << 0)
#define PI2_ON				(0x1 << 1)
#define PI2_OFF				(0x0 << 1)

/* Stepper stop interrupt control, will delete pulse data . */
#define ST_STOP				(0x1 << 0)
#define ST_RUN				(0x0 << 0)
#define ST_UPDW_STOP		(0x1 << 2)
#define ST_UPDW_RUN			(0x0 << 2)


/* Stepper start and stop position configure . */
#define	INIT_POS(n)			((n & 0xf) << 0)	// n must in [0,15], defalut 4
#define STOP_POS(n)			((n & 0x3) << 4)	// n must in [0,3], defalut 0

#define CHN12_INIT_POS		(CHN12_START_POS | INIT_POS(4))
#define CHN34_INIT_POS		(CHN34_START_POS | INIT_POS(4))	

#define CHN12_STOP_POS		(CHN12_PW_CTRL | ST_RUN | ST_UPDW_RUN | STOP_POS(0))
#define CHN34_STOP_POS		(CHN34_PW_CTRL | ST_RUN | ST_UPDW_RUN | STOP_POS(0))

/* Channel 5 freq configure . */
#define CHN5_PWM_N(n)		((n & 0x3) << 4)

/* Channel 5 control . */
#define CHN5_HIZ			(0x0 << 0)
#define CHN5_FORWARD		(0x1 << 0)
#define CHN5_REVERSE		(0x2 << 0)
#define CHN_STOP			(0x3 << 0)

/* Channel 5 speed control . */
// duty = 100% * n / 128. SPC must be Set SPC_NO_USE, or n will be 0.
#define CHN5_DUTY(n)		((n & 3f) << 0)

/* Channel 3/5 SPC mode control .*/
#define SPC_NO_USE			(0x0 << 4)
#define SPC_USE_3CH			(0x1 << 4)
#define SPC_USE_5CH			(0x2 << 4)
#define SPC_FORBIT			(0x3 << 4)

/* Channel 5 current value control . */
// current = DVDD * 0.1333/256 * 256 / RRNF. (n must >= 32, or forbit)
#define CHN5_CURRENT(n)		((n & 3f) << 0)


/* Channel 5/6 control mode configure . */
#define CHN56_REG_IO		(0x0 << 0)
#define CHN56_REG_REG		(0x1 << 0)
#define CHN56_IO_REG		(0x2 << 0)
#define CHN56_FORBIT		(0x3 << 0)


#if 1
#define STEPPER_FOCUS		CHN12_OP_CTRL
#define FS_SPEED_H			CHN12_SPEED_H
#define FS_SPEED_L			CHN12_SPEED_L
#define FS_MODE				CHN12_MODE

#define STEPPER_ZOOM		CHN34_OP_CTRL
#define ZM_SPEED_H			CHN34_SPEED_H
#define ZM_SPEED_L			CHN34_SPEED_L
#define ZM_MODE				CHN34_MODE

#define FS_BUSY_IO			(AF1_STATE11)
#define FS_CACHE_IO			(AF1_STATE12)
#define ZM_BUSY_IO			(AF1_STATE21)
#define ZM_CACHE_IO			(AF1_STATE22)
#else
#define STEPPER_FOCUS		CHN34_OP_CTRL
#define FS_SPEED_H			CHN34_SPEED_H
#define FS_SPEED_L			CHN34_SPEED_L
#define FS_MODE				CHN34_MODE

#define STEPPER_ZOOM		CHN12_OP_CTRL
#define ZM_SPEED_H			CHN12_SPEED_H
#define ZM_SPEED_L			CHN12_SPEED_L
#define ZM_MODE				CHN12_MODE

#define FS_BUSY_IO			(AF1_STATE21)
#define FS_CACHE_IO			(AF1_STATE22)
#define ZM_BUSY_IO			(AF1_STATE11)
#define ZM_CACHE_IO			(AF1_STATE12)
#endif


// Diffrent micro step PSUMxx to step count is different.
/*
	16-step	1/4 step mode.
	8-step	half step mode.
	4-step	full step mode.
*/
#define U_STEP_MODE			MICRO_U_STEP
#if (U_STEP_MODE == MICRO_U_STEP)
	#define DISTANCE2USTEP		(2)
	#define PULSE_VAL(dist)		((dist << 0)&0x3ff)
	#define PULSE_MAX			(1000)
#elif (U_STEP_MODE == MICRO_HALF_STEP)
	#define DISTANCE2USTEP		(1)
	#define PULSE_VAL(dist)		((dist << 1)&0x3fe)
	#define PULSE_MAX			(500)
#elif (U_STEP_MODE == MICRO_FULL_STEP)
	#define DISTANCE2USTEP		(1/2)
	#define PULSE_VAL(dist)		((dist << 2)&0x3fc)
	#define PULSE_MAX			(250)
#else
	#error "Unknow step mode"
#endif

#define BOX_U_STEP_MODE			MICRO_FULL_STEP		//for box piris
#if (BOX_U_STEP_MODE == MICRO_U_STEP)
	#define BOX_DISTANCE2USTEP		(2)
	#define BOX_PULSE_VAL(dist)		((dist << 0)&0x3ff)
	#define BOX_PULSE_MAX			(1000)
#elif (BOX_U_STEP_MODE == MICRO_HALF_STEP)
	#define BOX_DISTANCE2USTEP		(1)
	#define BOX_PULSE_VAL(dist)		((dist << 1)&0x3fe)
	#define BOX_PULSE_MAX			(500)
#elif (BOX_U_STEP_MODE == MICRO_FULL_STEP)
	#define BOX_DISTANCE2USTEP		1/2
	#define BOX_PULSE_VAL(dist)		((dist << 2)&0x3fc)
	#define BOX_PULSE_MAX			(250)
#else
	#error "Unknow step mode"
#endif

/**********************************************************************/


#endif /* __AF_BU24036MWV_H__ */

