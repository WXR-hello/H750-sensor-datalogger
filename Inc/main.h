/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    main.h
  * @brief   STM32H750XBH6 + IWT603（维特 wit_c_sdk）+ 数据记录到 SD 卡
  *
  *  IMU 固定走 USB Host 一条链路（模块的 USB 是标准 CDC 虚拟串口）：
  *      接线：模块 Type-C --A 转 C 数据线--> 底板 USB HOST 口；板子 DC 供电
  *      MCU ：USB_OTG_HS 的片内全速 PHY（D+/D- = PB14/PB15），
  *            48MHz 内核时钟来自 PLL3Q（PLL3 同时给 LTDC 供 27MHz，见 bsp_lcd.c）
  *
  *  收到的数据进 wit_c_sdk --> sReg[] --> LCD 显示 + SD 卡 CSV。
  ******************************************************************************
  */
/* USER CODE END Header */

#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32h7xx_hal.h"

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* Private defines -----------------------------------------------------------*/
/* IMU 固定走 USB Host（模块的 USB 是标准 CDC 虚拟串口），不再支持串口链路。 */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
