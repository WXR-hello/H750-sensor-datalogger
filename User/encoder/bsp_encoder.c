/**
  ******************************************************************************
  * @file    bsp_encoder.c
  * @brief   增量式编码器驱动：定时器硬件编码器模式（A/B 四倍频）+ Z 相外部中断
  *
  *   A/B 由定时器的"编码器接口模式"硬件计数（上下沿都计 = 四倍频），CPU 零负担；
  *   Z 相走外部中断，每圈一次，把位置计数清零（机械零点）。
  *
  *   引脚方案见 bsp_encoder.h 的 ENCODER_PIN_SET。
  ******************************************************************************
  */

#include "main.h"
#include "bsp_encoder.h"

//其中编码器部分的引脚规定：A为PA5，B为PB3，Z为PA8，使用定时器2
#define ENC_TIM_INST     TIM2
#define ENC_TIM_CLK()    __HAL_RCC_TIM2_CLK_ENABLE()
#define ENC_AF           GPIO_AF1_TIM2
#define ENC_A_PORT       GPIOA
#define ENC_A_PIN        GPIO_PIN_5
#define ENC_B_PORT       GPIOB
#define ENC_B_PIN        GPIO_PIN_3
#define ENC_Z_PORT       GPIOA
#define ENC_Z_PIN        GPIO_PIN_8
#define ENC_Z_IRQn       EXTI9_5_IRQn

/* ---------------- Z 相确认逻辑的参数 ----------------
 *   真 Z 每圈只来一次，所以两次真 Z 之间的累计计数必然是每圈计数（ENCODER_CPR）
 *   的整数倍；干扰装出来的 Z 落不到这个倍数上，直接被丢掉。 */
#define ENC_Z_GLITCH_MS     2U                     /* 候选 Z 先等这么久，短毛刺不处理 */
#define ENC_Z_MIN_MS        5U                     /* 两次真 Z 至少隔这么久 */
#define ENC_Z_WIN_CPR       (ENCODER_CPR / 8)      /* 允许的误差：±1/8 圈 */
#define ENC_Z_BOOT_CPR      (ENCODER_CPR / 4)      /* 上电后第一次：先按转过 1/4 圈对上相位 */
#define ENC_Z_RESYNC_CPR    ((ENCODER_CPR * 3) / 4)/* 总对不上、但确实转了大半圈 → 重新同步 */
#define ENC_Z_RESYNC_MAX    3U                     /* 连续丢掉这么多次才允许重新同步 */
#define ENC_Z_MOVE_EPS     50                      /* 判断"比上一轮走得更远"的容差（计数）*/

TIM_HandleTypeDef htim_enc;

static volatile uint8_t s_zFlag = 0;   /* 1 = 刚收到 Z 脉冲 */
static volatile uint8_t s_zSeen = 0;   /* 1 = 已经找到过零位 */
static uint16_t s_last  = 0;           /* 上次读到的计数值 */
static uint32_t s_zLastTick = 0;       /* Z 相去抖：上次"确认"的 Z 时间 */
static volatile uint8_t  s_zPend = 0;  /* 1 = EXTI 刚抓到一个候选 Z 脉冲 */
static volatile uint32_t s_zPendTick = 0; /* 候选脉冲的时间 */
static int32_t  s_total = 0;           /* 累计计数（带符号）*/
static volatile int32_t s_zCount = 0;  /* 收到多少个 Z 脉冲 */
static int32_t  s_cprMeasured = 0;     /* 实测每圈计数 */
static int32_t  s_zPrevTotal = 0;      /* 上一个 Z 时刻的累计计数 */
static uint8_t  s_cprValid = 0;        /* 1 = 测到过一圈 */
static int32_t  s_zeroTotal = 0;       /* 单圈角度的零点（累计计数 s_total 空间）*/
static uint8_t  s_zReject = 0;         /* 上次真 Z 之后连续丢掉的候选 Z 个数 */
static int32_t  s_zRejDelta = 0;       /* 上一次丢掉时的计数差（判断"有没有真的在转"）*/
static int32_t  s_spdLastCnt  = 0;
static uint32_t s_spdLastTick = 0;
static float    s_rpm = 0.0f;

/**
  * @brief  定时器底层初始化（时钟 + 引脚 + Z 相中断）
  */
void HAL_TIM_Encoder_MspInit(TIM_HandleTypeDef *htim)
{
  GPIO_InitTypeDef gpio = {0};

  if (htim->Instance != ENC_TIM_INST)
  {
    return;
  }

  ENC_TIM_CLK();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();

  /* A/B 两相：定时器输入（上拉，接收器没接时不会乱跳）*/
  gpio.Mode      = GPIO_MODE_AF_PP;
  gpio.Pull      = GPIO_PULLUP;
  gpio.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
  gpio.Alternate = ENC_AF;

  gpio.Pin = ENC_A_PIN;
  HAL_GPIO_Init(ENC_A_PORT, &gpio);

  gpio.Pin = ENC_B_PIN;
  HAL_GPIO_Init(ENC_B_PORT, &gpio);

  /* Z 相：上升沿外部中断 */
  gpio.Pin   = ENC_Z_PIN;
  gpio.Mode  = GPIO_MODE_IT_FALLING;
  gpio.Pull  = GPIO_PULLUP;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(ENC_Z_PORT, &gpio);

  HAL_NVIC_SetPriority(ENC_Z_IRQn, 6, 0);
  HAL_NVIC_EnableIRQ(ENC_Z_IRQn);
}


/**
  * @brief  初始化编码器（硬件编码器模式）
  */
void Encoder_Init(void)
{
  TIM_Encoder_InitTypeDef cfg = {0};

  htim_enc.Instance               = ENC_TIM_INST;
  htim_enc.Init.Prescaler         = 0;
  htim_enc.Init.CounterMode       = TIM_COUNTERMODE_UP;
  htim_enc.Init.Period            = 0xFFFF;
  htim_enc.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
  htim_enc.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;

  cfg.EncoderMode  = TIM_ENCODERMODE_TI12;    /* A/B 两相都计 = 四倍频 */
  cfg.IC1Polarity  = TIM_ICPOLARITY_RISING;
  cfg.IC1Selection = TIM_ICSELECTION_DIRECTTI;
  cfg.IC1Prescaler = TIM_ICPSC_DIV1;
  cfg.IC1Filter    = 15;                      /* 滤波拉满：抗干扰 */
  cfg.IC2Polarity  = TIM_ICPOLARITY_RISING;
  cfg.IC2Selection = TIM_ICSELECTION_DIRECTTI;
  cfg.IC2Prescaler = TIM_ICPSC_DIV1;
  cfg.IC2Filter    = 15;

  if (HAL_TIM_Encoder_Init(&htim_enc, &cfg) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_TIM_Encoder_Start(&htim_enc, TIM_CHANNEL_ALL) != HAL_OK)
  {
    Error_Handler();
  }

  __HAL_TIM_SET_COUNTER(&htim_enc, 0);
  s_last = 0;
  s_total = 0;
  s_zCount = 0;
  s_cprMeasured = 0;
  s_cprValid = 0;
  s_zPrevTotal = 0;
  s_zeroTotal = 0;
  s_zReject = 0;
  s_zRejDelta = 0;
  s_spdLastCnt = 0;
  s_spdLastTick = HAL_GetTick();
}

uint8_t Encoder_ReadPins(void)
{
  return 0U;
}

/**
  * @brief  Z 相中断：每圈一次，把位置计数清零（机械零点）
  */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
  if (GPIO_Pin == ENC_Z_PIN)
  {
    /* 中断里只记一个"候选"，真正的判断放到主循环里做：
       编码器没接时 PA8 是悬空的，旁边的干扰很容易假装成上升沿，
       如果在这里直接清零计数，角度就会无故乱跳。*/
    s_zPend     = 1U;
    s_zPendTick = HAL_GetTick();
  }
}

/**
  * @brief  确认"候选 Z 脉冲"是不是真的（在主循环里调用）
  *
  *   真 Z 每圈只来一次，所以两次真 Z 之间的累计计数必然是每圈计数（ENCODER_CPR）
  *   的整数倍；干扰装出来的 Z 落不到这个倍数上，就丢掉。
  *   轴停着不动时累计计数不变，毛刺再多也凑不出"一圈"，所以不会被误认成零位，
  *   角度就不会自己跳回 0。
  */
static void Encoder_ConfirmZ(void)
{
  uint32_t cand;
  int32_t  delta, n, err;
  uint8_t  ok = 0U;
  uint8_t  resync = 0U;

  if (s_zPend == 0U)
  {
    return;
  }

  if ((HAL_GetTick() - s_zPendTick) < ENC_Z_GLITCH_MS)   /* 短毛刺先不理它 */
  {
    return;
  }

  cand    = s_zPendTick;      /* 用 EXTI 记下的边沿时刻，不受主循环卡顿影响 */
  s_zPend = 0U;

  if ((cand - s_zLastTick) < ENC_Z_MIN_MS)     /* 刚认过一个真 Z，这次多半是抖动 */
  {
    return;
  }

  delta = s_total - s_zPrevTotal;
  if (delta < 0)
  {
    delta = -delta;
  }

  n   = (delta + (ENCODER_CPR / 2)) / ENCODER_CPR;   /* 距上次真 Z 走了几圈（四舍五入）*/
  err = delta - (n * ENCODER_CPR);
  if (err < 0)
  {
    err = -err;
  }

  if ((n >= 1) && (err <= ENC_Z_WIN_CPR))
  {
    ok = 1U;                                   /* 正好落在整数圈上 → 真 Z */
  }
  else if ((s_zSeen == 0U) && (delta >= ENC_Z_BOOT_CPR))
  {
    ok = 1U;                                   /* 上电后第一次：先按转过 1/4 圈对上相位 */
  }
  else if ((s_zReject >= ENC_Z_RESYNC_MAX) && (delta >= ENC_Z_RESYNC_CPR))
  {
    ok = 1U;                                   /* 一直在转却总对不上 → 每圈计数可能不是这个值，重新同步 */
    resync = 1U;
  }

  if (ok == 0U)
  {
    /* 丢掉这次候选。只有"比上次丢掉的时候走得更远"才算一次有效失败：
       轴停住不动时累计计数不变，毛刺再多也凑不够重新同步的次数。*/
    if ((delta > (s_zRejDelta + ENC_Z_MOVE_EPS)) ||
        (delta < (s_zRejDelta - ENC_Z_MOVE_EPS)))
    {
      if (s_zReject < 0xFFU)
      {
        s_zReject++;
      }
    }
    s_zRejDelta = delta;
    return;
  }

  s_zLastTick  = cand;
  s_zFlag      = 1U;
  s_zSeen      = 1U;
  s_zCount++;
  s_zReject    = 0U;
  s_zRejDelta  = 0;
  s_zPrevTotal = s_total;
  s_zeroTotal  = s_total;                    /* 零点记在累计计数上（角度就是按它算的）*/
  __HAL_TIM_SET_COUNTER(&htim_enc, 0);       /* 原始计数器也清零（POS 行看着直观）*/
  s_last = 0U;

  /* 标定"每圈计数"（只给 CPR 行显示用，不参与角度和 Z 判定）：
     n == 1 说明这一格正好是一圈；重新同步那次也算一圈的量级。*/
  if ((n == 1) || ((resync != 0U) && (delta <= ((ENCODER_CPR * 3) / 2))))
  {
    s_cprMeasured = delta;
    s_cprValid    = 1U;
  }
}

int32_t Encoder_ReadDelta(void)
{
  uint16_t raw;
  int16_t  d;

  /* 先把原始计数器的增量并进累计计数（s_total），再去处理 Z：
     顺序反过来的话，Z 把计数器清零会把"这一轮还没读走的增量"吞掉。*/
  raw    = (uint16_t)__HAL_TIM_GET_COUNTER(&htim_enc);
  d      = (int16_t)(raw - s_last);
  s_last = raw;
  s_total += (int32_t)d;

  Encoder_ConfirmZ();      /* 确认候选 Z 是不是真零位（只动零点和计数器）*/

  return (int32_t)d;
}

int32_t Encoder_GetCount(void)
{
  return s_total;
}

int32_t Encoder_GetPos(void)
{
  return (int32_t)(uint16_t)__HAL_TIM_GET_COUNTER(&htim_enc);
}

/**
  * @brief  单圈角度（0~360 度）：用累计计数 s_total 算，自 Z 零位起
  *
  *   不用 16 位原始计数器：它 65536 就回绕，而每圈是 ENCODER_CPR（10000），
  *   两个周期对不上，Z 没对上时角度会在回绕点跳一下；s_total 是 32 位累计值，
  *   没有这个问题。
  */
float Encoder_GetAngleDeg(void)
{
  int32_t pos = (s_total - s_zeroTotal) % ENCODER_CPR;

  if (pos < 0)
  {
    pos += ENCODER_CPR;
  }

  return (float)pos * (360.0f / (float)ENCODER_CPR);
}

void Encoder_ResetCount(void)
{
  s_total      = 0;
  s_zPrevTotal = s_total;
  s_zeroTotal  = s_total;
}

uint8_t Encoder_GetZFlag(void)
{
  uint8_t f = s_zFlag;
  s_zFlag = 0U;
  return f;
}

uint8_t Encoder_IsZSeen(void)
{
  return s_zSeen;
}

int32_t Encoder_GetZCount(void)
{
  return s_zCount;
}

int32_t Encoder_GetCprMeasured(void)
{
  return s_cprValid ? s_cprMeasured : 0;
}

/**
  * @brief  算转速（转/分），主循环里 >=20ms 调一次
  */
void Encoder_UpdateSpeed(void)
{
  uint32_t now = HAL_GetTick();
  uint32_t dt  = now - s_spdLastTick;
  int32_t  d;

  if (dt < 20U)
  {
    return;
  }

  d = s_total - s_spdLastCnt;
  s_spdLastCnt  = s_total;
  s_spdLastTick = now;

  s_rpm = ((float)d * 60000.0f) / ((float)dt * (float)ENCODER_CPR);
}

float Encoder_GetSpeedRpm(void)
{
  return s_rpm;
}
