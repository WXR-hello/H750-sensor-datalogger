/**
  ******************************************************************************
  * @file    force.c
  * @brief   力传感器（ADC，PB0 = ADC1_INP9）初始化与采样
  *
  *   16 位分辨率，软件触发单次转换；每次读 64 个点求平均（并去掉一个最大值、
  *   一个最小值），结果按 0~50kg 标定后换算成克。KEY1(PA0) 按住 1.5 秒去皮。

  *   对外接口：Force_Init() 开机调用一次，Force_Task() 主循环里周期调用。
  ******************************************************************************
  */

#include "main.h"
#include "force.h"
#include "stm32h7xx_hal_adc.h"      /* 力传感器 ADC（PB0）*/
#include "stm32h7xx_hal_adc_ex.h"


/* 力传感器 ADC（PB0 = ADC1_INP9）：已启用；要临时停用就把它改成 #if 0。*/
/* ==========================================================================
 * 力传感器模拟输入：PB0 = ADC1_INP9（注意：PB0 与板载红色 LED 共用，
 * 必须拔掉 J73 跳线帽，否则 LED 电路会分走电流、读数不准）
 *   16 位分辨率，软件触发单次转换，取 8 次平均；结果当整数记进 CSV 的 force 列。
 * ========================================================================== */


#define FORCE_ADC_GPIO   GPIOB
#define FORCE_ADC_GPIN   GPIO_PIN_0
#define FORCE_ADC_CH     ADC_CHANNEL_9
ADC_HandleTypeDef hadc1;
volatile int32_t g_forceAdc = 0;      /* 原始码值（已滤波）*/
static uint32_t g_forceTick = 0U;

/* ---- 力传感器标定（BSQ-3：输出 0~3.3V 对应 0~50kg）--------------
   FORCE_FULL_CODE：3.3V（满量程）时 ADC 实际读到的码值 —— 实测 64600 左右，
                    用它吸收“参考电压/增益差”那 1.6% 的偏差；
   FORCE_RANGE_G  ：满量程对应的克数（50kg = 50000 g）。*/
#define FORCE_FULL_CODE   64600L
#define FORCE_RANGE_G     50000L
#define FORCE_TARE_N      20U        /* 上电头 1 秒采零点（此时秤盘必须空载）*/

int32_t g_forceZero = 0;              /* 去皮零点（码值）*/
static int32_t g_forceSpan = FORCE_FULL_CODE;/* 零点 -> 满量程的码数跨度 */
static int32_t g_forceRaw  = 0;              /* 标定后（未平滑）的力值（克）*/
int32_t g_forceG    = 0;              /* 平滑后的力值（克），给屏幕/CSV 用 */
static uint8_t g_tareCnt   = 0U;
static int32_t g_tareSum   = 0;

//引脚PB0 

static void Force_ADC1_Init(void)
{
  GPIO_InitTypeDef      gpio = {0};
  ADC_ChannelConfTypeDef cfg = {0};

  __HAL_RCC_ADC12_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /* 力传感器模拟输入：PB0 = ADC1_INP9 */
  gpio.Pin  = FORCE_ADC_GPIN;              /* GPIO_PIN_0 */
  gpio.Mode = GPIO_MODE_ANALOG;
  gpio.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(FORCE_ADC_GPIO, &gpio);    /* GPIOB */

  /* KEY1(PA0)：去皮按键（按住 1.5 秒把当前值当*/
  gpio.Pin  = GPIO_PIN_0;
  gpio.Mode = GPIO_MODE_INPUT;
  gpio.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOA, &gpio);

  hadc1.Instance                      = ADC1;
  hadc1.Init.ClockPrescaler           = ADC_CLOCK_SYNC_PCLK_DIV4;
  hadc1.Init.Resolution               = ADC_RESOLUTION_16B;
  hadc1.Init.ScanConvMode             = ADC_SCAN_DISABLE;
  hadc1.Init.EOCSelection             = ADC_EOC_SINGLE_CONV;
  hadc1.Init.LowPowerAutoWait         = DISABLE;
  hadc1.Init.ContinuousConvMode       = DISABLE;
  hadc1.Init.NbrOfConversion          = 1U;
  hadc1.Init.DiscontinuousConvMode    = DISABLE;
  hadc1.Init.ExternalTrigConv         = ADC_SOFTWARE_START;
  hadc1.Init.ExternalTrigConvEdge     = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc1.Init.ConversionDataManagement = ADC_CONVERSIONDATA_DR;
  hadc1.Init.Overrun                  = ADC_OVR_DATA_OVERWRITTEN;
  hadc1.Init.LeftBitShift             = ADC_LEFTBITSHIFT_NONE;
  /* 不用硬件过采样：改在 ForceAdc_Read() 里做 64 次采样平均 + 去极值，
     效果一样，而且不依赖不同 HAL 版本里不一致的过采样宏名。*/
  hadc1.Init.OversamplingMode         = DISABLE;
  (void)HAL_ADC_Init(&hadc1);
  (void)HAL_ADCEx_Calibration_Start(&hadc1, ADC_CALIB_OFFSET, ADC_SINGLE_ENDED);

  cfg.Channel      = FORCE_ADC_CH;
  cfg.Rank         = ADC_REGULAR_RANK_1;
  cfg.SamplingTime = ADC_SAMPLETIME_810CYCLES_5;
  cfg.SingleDiff   = ADC_SINGLE_ENDED;
  cfg.OffsetNumber = ADC_OFFSET_NONE;
  cfg.Offset       = 0U;
  (void)HAL_ADC_ConfigChannel(&hadc1, &cfg);
}

/* 读一次力传感器：采 64 次求平均（并去掉一个最大值、一个最小值，
   削掉偶发毛刺）。64 次大约 2.2ms，50ms 的主循环完全放得下。
   噪声按 √64 = 8 倍下降（实测的 200 个码 -> 二十几个码）。*/
#define FORCE_ADC_N   64U
static int32_t ForceAdc_Read(void)
{
  uint32_t acc = 0U;
  uint32_t v;
  uint32_t vmin = 0xFFFFFFFFU;
  uint32_t vmax = 0U;
  uint8_t  i;
  uint8_t  n = 0U;

  for (i = 0U; i < FORCE_ADC_N; i++)
  {
    HAL_ADC_Start(&hadc1);
    if (HAL_ADC_PollForConversion(&hadc1, 5U) == HAL_OK)
    {
      v = HAL_ADC_GetValue(&hadc1);
      acc += v;
      if (v < vmin) { vmin = v; }
      if (v > vmax) { vmax = v; }
      n++;
    }
    HAL_ADC_Stop(&hadc1);
  }

  if (n == 0U)                 { return 0; }
  if (n > 2U) { acc -= (vmin + vmax); n = (uint8_t)(n - 2U); }
  return (int32_t)(acc / (uint32_t)n);
}
/* 去皮 + 码值换算成克：
   上电头 1 秒（FORCE_TARE_N 次）取平均当零点，此时传感器必须空载；
   之后 (码值 - 零点) / 满量程跨度 × 满量程克数 = 净重。*/
static void Force_Update(void)
{
  int32_t d;

  d = g_forceAdc;                     /* 不去皮：直接用 ADC 值换算 */
  if (d < 0) { d = 0; }
  g_forceRaw = (int32_t)(((int64_t)d * FORCE_RANGE_G) / FORCE_FULL_CODE);

  /* 冲击测量：不做一阶滤波，直接用 64 次平均后的值（响应最快）*/
  g_forceG = g_forceRaw;
}

/* ---- 按键去皮：KEY1(PA0) 按住 1.5 秒，把当前码值当零点 ----
   松开后才能再触发一次，避免按键一直按着反复去皮。*/
static void Force_KeyTask(void)
{
  static uint32_t s_lowTick = 0U;
  static uint8_t  s_fired   = 0U;
  uint8_t  low = (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_0) == GPIO_PIN_RESET) ? 1U : 0U;

  if (low == 0U)                     /* 松开：允许下一次 */
  {
    s_lowTick = 0U;
    s_fired   = 0U;
    return;
  }

  if (s_lowTick == 0U) { s_lowTick = HAL_GetTick(); return; }

  if ((s_fired == 0U) && ((HAL_GetTick() - s_lowTick) >= 1500U))
  {
    s_fired     = 1U;
    g_forceZero = g_forceAdc;
    g_forceSpan = FORCE_FULL_CODE - g_forceZero;
    if (g_forceSpan < 1000L) { g_forceSpan = FORCE_FULL_CODE; }
    g_forceRaw  = 0;
    g_forceG    = 0;
  }
}



/**
  * @brief  力传感器初始化：ADC 配置 + 校准。开机调用一次。
  */
void Force_Init(void)
{
  /* ---- 力传感器模拟输入（PB0 = ADC1_INP9）---- */
  Force_ADC1_Init();
}

/**
  * @brief  力传感器轮询：主循环里周期调用，每 50ms 采一次（内部 64 次平均）。
  */
void Force_Task(void)
{
  /* 力传感器：每 50ms 采一次（内部 64 次平均）*/
  if ((HAL_GetTick() - g_forceTick) >= 50U)
  {
    g_forceTick = HAL_GetTick();
    g_forceAdc  = ForceAdc_Read();
    Force_Update();            /* 换算成克 */
  }
  Force_KeyTask();             /* KEY1(PA0) 按住 1.5 秒：重新去皮 */
}

/************************ (C) COPYRIGHT *****END OF FILE************************/
