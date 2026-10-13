#ifndef _IMU_H_
#define _IMU_H_

#include "stm32h7xx_hal.h"

/* ---- IMU（IWT603，走 USB Host）初始化与轮询 ----
 *   Imu_Init()：开机调用一次（必须在 LCD_Init() 之后，
 *               USB 的 48MHz 时钟来自 LCD 配好的 PLL3Q）。
 *   Imu_Task()：主循环里周期调用。
 */
void Imu_Init(void);
void Imu_Task(void);

/* ---- IMU 诊断量：LCD 状态行读取 ---- */
extern volatile uint32_t g_rxBytes;   /* 模块口收到的字节数 */
extern volatile uint32_t g_frames;    /* IMU 帧计数 */
extern volatile uint32_t g_lastFrame; /* 最近一帧 IMU 数据的时刻(ms) */
extern uint32_t g_fps;                /* 实际帧率（帧/秒）*/
extern uint32_t g_ageMs;              /* 最新数据有多旧(ms) */
extern uint32_t g_gapMaxMs;           /* 上一秒内相邻两帧最大间隔(ms) */
extern uint32_t g_rxBps;              /* 每秒收到的字节数 */
extern uint8_t  g_modRate;            /* 模块里实际的“回传率”寄存器值 */
extern uint8_t  g_modRsw;             /* 模块里实际的“回传内容”寄存器值 */

#endif 
