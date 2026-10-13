/**
  ******************************************************************************
  * @file    relay.h
  * @brief   按键 KEY1 控制继电器（按住吸合，松开释放）
  *
  *   接线：
  *     板载 KEY1 = PA0（按下把 PA0 拉到有效电平；空闲电平上电时自动识别）
  *     继电器模块 IN  -> PA12（本工程没用的空闲脚）
  *     继电器模块 VCC -> 板子  3V3（按模块要求）
  *     继电器模块 GND -> 板子 GND（必须共地）
  *
  *   动作：
  *     KEY1 按下  -> 继电器线圈通电（COM-NO 接通）
  *     KEY1 松开  -> 继电器释放（COM-NC 常闭，默认状态）
  *
  *   要改引脚 / 触发极性：只改下面 5 行宏，别的都不用动。
  ******************************************************************************
  */

#ifndef __RELAY_H
#define __RELAY_H

#include "stm32h7xx_hal.h"

/* ---- 按键 KEY1（板载按键）---- */
#define RELAY_KEY_PORT        GPIOC
#define RELAY_KEY_PIN         GPIO_PIN_13   /* 野火板子 KEY2 = PC13 */

/* ---- 继电器模块控制脚（IN）---- */
#define RELAY_IN_PORT         GPIOA
#define RELAY_IN_PIN          GPIO_PIN_12     

/* ---- 极性 ----
 *   RELAY_ACTIVE_LOW      1 = 低电平吸合（常见的光耦继电器模块，IN 拉低吸合）
 *                         0 = 高电平吸合
*/
#define RELAY_ACTIVE_LOW      0U      /* 0 = 高电平吸合 */
#define RELAY_KEY_ACTIVE_HIGH     2U

void    Relay_Init(void);        /* 初始化（开机调用一次）*/
void    Relay_Task(void);        /* 主循环里周期调用：读 KEY1 → 驱动继电器 */
uint8_t Relay_IsPressed(void);   /* 当前按键是否按下（去抖后）*/
uint8_t Relay_IsOn(void);        /* 当前继电器是否吸合 */

#endif 
