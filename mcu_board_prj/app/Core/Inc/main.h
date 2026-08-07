/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32g0xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define PD_ADC_Pin GPIO_PIN_1
#define PD_ADC_GPIO_Port GPIOA
#define U2_TX_Pin GPIO_PIN_2
#define U2_TX_GPIO_Port GPIOA
#define U2_RX_Pin GPIO_PIN_3
#define U2_RX_GPIO_Port GPIOA
#define SPI1_CS_Pin GPIO_PIN_4
#define SPI1_CS_GPIO_Port GPIOA
#define SPI1_CLK_Pin GPIO_PIN_5
#define SPI1_CLK_GPIO_Port GPIOA
#define SPI1_MISO_Pin GPIO_PIN_6
#define SPI1_MISO_GPIO_Port GPIOA
#define SPI1_MOSI_Pin GPIO_PIN_7
#define SPI1_MOSI_GPIO_Port GPIOA
#define RS485_TX3_Pin GPIO_PIN_4
#define RS485_TX3_GPIO_Port GPIOC
#define RS485_RX3_Pin GPIO_PIN_5
#define RS485_RX3_GPIO_Port GPIOC
#define RADAR_EN_Pin GPIO_PIN_0
#define RADAR_EN_GPIO_Port GPIOB
#define RS485_DE3_Pin GPIO_PIN_1
#define RS485_DE3_GPIO_Port GPIOB
#define TEMP_ADC_Pin GPIO_PIN_2
#define TEMP_ADC_GPIO_Port GPIOB
#define ALARM_IN0_Pin GPIO_PIN_13
#define ALARM_IN0_GPIO_Port GPIOB
#define ALARM_IN0_EXTI_IRQn EXTI4_15_IRQn
#define ALARM_IN1_Pin GPIO_PIN_14
#define ALARM_IN1_GPIO_Port GPIOB
#define ALARM_IN1_EXTI_IRQn EXTI4_15_IRQn
#define ALARM_OUT0_Pin GPIO_PIN_15
#define ALARM_OUT0_GPIO_Port GPIOB
#define ALARM_OUT1_Pin GPIO_PIN_8
#define ALARM_OUT1_GPIO_Port GPIOA
#define DEBUG_TX1_Pin GPIO_PIN_9
#define DEBUG_TX1_GPIO_Port GPIOA
#define WIEGAND_OUT0_Pin GPIO_PIN_6
#define WIEGAND_OUT0_GPIO_Port GPIOC
#define WIEGAND_OUT1_Pin GPIO_PIN_7
#define WIEGAND_OUT1_GPIO_Port GPIOC
#define PWR_RST_Pin GPIO_PIN_8
#define PWR_RST_GPIO_Port GPIOD
#define SYS_LED_Pin GPIO_PIN_9
#define SYS_LED_GPIO_Port GPIOD
#define DEBUG_RX1_Pin GPIO_PIN_10
#define DEBUG_RX1_GPIO_Port GPIOA
#define HEAT_EN_Pin GPIO_PIN_15
#define HEAT_EN_GPIO_Port GPIOA
#define PWM_NEAR_Pin GPIO_PIN_8
#define PWM_NEAR_GPIO_Port GPIOC
#define PWM_FAR_Pin GPIO_PIN_9
#define PWM_FAR_GPIO_Port GPIOC
#define VD_IS_Pin GPIO_PIN_0
#define VD_IS_GPIO_Port GPIOD
#define VD_FZ_Pin GPIO_PIN_1
#define VD_FZ_GPIO_Port GPIOD
#define PLS1_Pin GPIO_PIN_2
#define PLS1_GPIO_Port GPIOD
#define PLS1_EXTI_IRQn EXTI2_3_IRQn
#define PLS2_Pin GPIO_PIN_3
#define PLS2_GPIO_Port GPIOD
#define PLS2_EXTI_IRQn EXTI2_3_IRQn
#define RSTB_Pin GPIO_PIN_4
#define RSTB_GPIO_Port GPIOD
#define Z_RST_Pin GPIO_PIN_5
#define Z_RST_GPIO_Port GPIOD
#define Z_RST_EXTI_IRQn EXTI4_15_IRQn
#define F_RST_Pin GPIO_PIN_6
#define F_RST_GPIO_Port GPIOD
#define F_RST_EXTI_IRQn EXTI4_15_IRQn
#define LENS_EN_Pin GPIO_PIN_3
#define LENS_EN_GPIO_Port GPIOB
#define PWM_RLED_Pin GPIO_PIN_4
#define PWM_RLED_GPIO_Port GPIOB
#define PWM_WLED_Pin GPIO_PIN_5
#define PWM_WLED_GPIO_Port GPIOB
#define RLED_EN_Pin GPIO_PIN_6
#define RLED_EN_GPIO_Port GPIOB
#define WLED_EN_Pin GPIO_PIN_7
#define WLED_EN_GPIO_Port GPIOB
#define IR_CUT_EN_Pin GPIO_PIN_8
#define IR_CUT_EN_GPIO_Port GPIOB
#define FAN_EN_Pin GPIO_PIN_9
#define FAN_EN_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
