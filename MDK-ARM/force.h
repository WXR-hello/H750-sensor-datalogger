#ifndef _FORCE_H_
#define _FORCE_H_

#include "stm32h7xx_hal.h"

/* ---- 力传感器（ADC，PB0 = ADC1_INP9）初始化与采样 ----
 *   Force_Init()：开机调用一次。
 *   Force_Task()：主循环里周期调用。
 */
void Force_Init(void);
void Force_Task(void);

/* ---- 供 LCD 显示 / SD 记录读取 ---- */
extern volatile int32_t g_forceAdc;   /* 原始码值（64 次平均后的值）*/
extern int32_t          g_forceG;     /* 去皮标定后的力值（克）*/
extern int32_t          g_forceZero;  /* 去皮零点（码值）*/

#endif /* _FORCE_H_ */
