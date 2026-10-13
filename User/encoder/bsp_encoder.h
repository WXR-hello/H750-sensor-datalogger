/**
  ******************************************************************************
  * @file    bsp_encoder.h
  * @brief   增量式差分编码器（A/B 相 + Z 相）
  *
  *   编码器：2500 P/R，差分输出 -> 差分接收器 -> 单端 3.3V -> 单片机
  *   四倍频计数：2500 x 4 = 10000 计数/圈
  *
    ### 引脚方案（用 ENCODER_PIN_SET 选，改完重新编译即可）###
              A:PA15 B:PB3  Z:PA8   [TIM2]  
  ******************************************************************************
  */

#ifndef __BSP_ENCODER_H
#define __BSP_ENCODER_H

#include "stm32h7xx_hal.h"

/* 每圈计数：2500 P/R × 4（A/B 两相上下沿都计）= 10000
 *   ← 单圈角度的比例基准，同时是 Z 相判定窗口的基准；
 *     屏幕上的 CPR 行固定显示这个值（不会变）。
 *     想确认编码器是不是 2500 P/R：手转整一圈，看 POS 行一共走了多少
 *     （或者用仿真器看 Encoder_GetCprMeasured()，那是每过一圈实测的值）；
 *     不是 10000 就把这里改成实测值。*/
#define ENCODER_CPR            10000

/* Z 相（PA8，每圈一次）就是机械零点：认到一次就把单圈角度清 0。
 *   为了不让"轴没转时的干扰"把角度清 0，只有落在"整数圈"上的 Z 才认
 *   （距上次真 Z 的累计计数 = 每圈计数的整数倍，误差 ±1/8 圈），
 *   判定过程见 bsp_encoder.c 里的 Encoder_ConfirmZ()。*/

extern TIM_HandleTypeDef htim_enc;

void    Encoder_Init(void);          /* 初始化定时器编码器模式 + Z 相中断 */
int32_t Encoder_ReadDelta(void);     /* 主循环周期调用：读增量并累加 */
int32_t Encoder_GetCount(void);      /* 累计计数（带符号，多圈）*/
int32_t Encoder_GetPos(void);        /* 自 Z 零位起的位置（0~65535）*/
float   Encoder_GetAngleDeg(void);   /* 单圈角度 0~360 度（自 Z 零位起算；
                                        基于累计计数，不会在 16 位回绕点跳）*/
void    Encoder_ResetCount(void);
uint8_t Encoder_GetZFlag(void);
uint8_t Encoder_IsZSeen(void);
int32_t Encoder_GetZCount(void);     /* 收到过多少个 Z 脉冲 */
int32_t Encoder_GetCprMeasured(void);/* 实测每圈计数（0 = 还没测到，只用于显示）*/
void    Encoder_UpdateSpeed(void);   /* 主循环周期调用（>=20ms 一次）*/
float   Encoder_GetSpeedRpm(void);
uint8_t Encoder_ReadPins(void);      /* 观测模式：A/B/Z 电平，bit0~2 */

#endif /* __BSP_ENCODER_H */
