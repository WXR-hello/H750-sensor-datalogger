/**
  ******************************************************************************
  * @file    relay.c
  * @brief   KEY2 按键 → 继电器（按住吸合，松开释放，COM-NC 常闭）
  ******************************************************************************
  */

#include "relay.h"

#define RELAY_DEBOUNCE_MS   10U      /* 去抖：电平稳定这么久才算数 */

static uint8_t  s_initDone   = 0U;
static uint8_t  s_keyActiveHigh = 0U;   /* 1 = 按下时读到高电平 */
static uint8_t  s_pressed    = 0U;      /* 去抖后的按键状态 */
static uint8_t  s_relayOn    = 0U;      /* 当前继电器状态 */
static uint8_t  s_rawLast    = 0U;      /* 上一次采到的原始电平 */
static uint32_t s_rawChange  = 0U;      /* 电平最后一次变化的时刻 */

/* 把“继电器吸合/释放”写进硬件 */
static void Relay_Write(uint8_t on)
{
  GPIO_PinState level;

  if (RELAY_ACTIVE_LOW != 0U)
  {
    level = (on != 0U) ? GPIO_PIN_RESET : GPIO_PIN_SET;
  }
  else
  {
    level = (on != 0U) ? GPIO_PIN_SET : GPIO_PIN_RESET;
  }

  HAL_GPIO_WritePin(RELAY_IN_PORT, RELAY_IN_PIN, level);
}

/**
  * @brief  初始化：KEY2 做输入（不带内部上下拉，靠板上的电阻定空闲电平），
  *         IN 做推挽输出。上电时先读一次 KEY2 的空闲电平，用来判断“按下是高还是低”。
  */
void Relay_Init(void)
{
  GPIO_InitTypeDef gpio = {0};

  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();

  /* KEY1 */
  gpio.Pin   = RELAY_KEY_PIN;
  gpio.Mode  = GPIO_MODE_INPUT;
  gpio.Pull  = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(RELAY_KEY_PORT, &gpio);

  /* 继电器 IN：先按“释放”写出去，避免上电瞬间误吸合 */
  Relay_Write(0U);
  gpio.Pin   = RELAY_IN_PIN;
  gpio.Mode  = GPIO_MODE_OUTPUT_PP;
  gpio.Pull  = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(RELAY_IN_PORT, &gpio);
  Relay_Write(0U);

#if (RELAY_KEY_ACTIVE_HIGH == 2U)
  /* 空闲电平：读一次。读到高 → 按下应该是低（低有效）；读到低 → 按下是高。 */
  s_keyActiveHigh = (HAL_GPIO_ReadPin(RELAY_KEY_PORT, RELAY_KEY_PIN) == GPIO_PIN_RESET) ? 1U : 0U;
#else
  s_keyActiveHigh = (RELAY_KEY_ACTIVE_HIGH != 0U) ? 1U : 0U;
#endif

  s_rawLast   = (HAL_GPIO_ReadPin(RELAY_KEY_PORT, RELAY_KEY_PIN) == GPIO_PIN_SET) ? 1U : 0U;
  s_rawChange = HAL_GetTick();
  s_pressed   = 0U;
  s_relayOn   = 0U;
  s_initDone  = 1U;
}

/**
  * @brief  主循环里周期调用
  */
/* 按下按键后继电器吸合的时间（给失电型电磁铁的消磁脉冲，单位 ms）*/
#define RELAY_PULSE_MS        2000U

static uint8_t  s_lastPressed = 0U;   /* 上一次的按键状态（做上升沿用）*/
static uint32_t s_pulseTick   = 0U;   /* 脉冲开始时刻 */

void Relay_Task(void)
{
  uint8_t raw;

  if (s_initDone == 0U)
  {
    return;
  }

  /* 读原始电平（1 = 高）*/
  raw = (HAL_GPIO_ReadPin(RELAY_KEY_PORT, RELAY_KEY_PIN) == GPIO_PIN_SET) ? 1U : 0U;

  if (raw != s_rawLast)
  {
    s_rawLast   = raw;
    s_rawChange = HAL_GetTick();
    return;                                   /* 刚变化，等它稳定 */
  }

  if ((HAL_GetTick() - s_rawChange) >= RELAY_DEBOUNCE_MS)
  {
    uint8_t pressed;

    pressed = (s_keyActiveHigh != 0U) ? ((raw != 0U) ? 1U : 0U)
                                      : ((raw == 0U) ? 1U : 0U);
    s_pressed = pressed;

    /* ---- 上升沿触发：按一下 -> 吸合 RELAY_PULSE_MS -> 自动断开 ----
       失电型电磁铁：通一下电就消磁释放；按住不放也只触发一次 */
    if ((pressed != 0U) && (s_lastPressed == 0U))
    {
      s_relayOn   = 1U;
      Relay_Write(1U);
      s_pulseTick = HAL_GetTick();
    }

    if ((s_relayOn != 0U) && ((HAL_GetTick() - s_pulseTick) >= RELAY_PULSE_MS))
    {
      s_relayOn = 0U;
      Relay_Write(0U);
    }

    s_lastPressed = pressed;
  }
}

uint8_t Relay_IsPressed(void)
{
  return s_pressed;
}

uint8_t Relay_IsOn(void)
{
  return s_relayOn;
}

/************************ (C) COPYRIGHT *****END OF FILE************************/
