/**
  ******************************************************************************
  * @file    stm32h7xx_it.c
  * @brief   Interrupt handlers.
  *
  *  The IWT603 IMU module is connected over the USB Host port (the module
  *  exposes a standard CDC virtual serial port), so no UART / USART
  *  interrupt is needed here.
  *
  *  The SD card is accessed in polling mode (HAL_SD_ReadBlocks/WriteBlocks),
  *  so no SDMMC interrupt is needed here.
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "stm32h7xx_it.h"

/* External variables --------------------------------------------------------*/
extern HCD_HandleTypeDef  hhcd;      /* USB 主机控制器句柄（IMU 走 USB 时用）*/

/******************************************************************************/
/*           Cortex-M7 Processor Interruption and Exception Handlers          */
/******************************************************************************/

/**
  * @brief This function handles Non maskable interrupt.
  */
void NMI_Handler(void)
{
  while (1)
  {
  }
}

/**
  * @brief This function handles Hard fault interrupt.
  */
void HardFault_Handler(void)
{
  while (1)
  {
  }
}

/**
  * @brief This function handles Memory management fault.
  */
void MemManage_Handler(void)
{
  while (1)
  {
  }
}

/**
  * @brief This function handles Pre-fetch fault, memory access fault.
  */
void BusFault_Handler(void)
{
  while (1)
  {
  }
}

/**
  * @brief This function handles Undefined instruction or illegal state.
  */
void UsageFault_Handler(void)
{
  while (1)
  {
  }
}

/**
  * @brief This function handles System service call via SWI instruction.
  */
void SVC_Handler(void)
{
}

/**
  * @brief This function handles Debug monitor.
  */
void DebugMon_Handler(void)
{
}

/**
  * @brief This function handles Pendable request for system service.
  */
void PendSV_Handler(void)
{
}

/**
  * @brief This function handles System tick timer.
  */
void SysTick_Handler(void)
{
  HAL_IncTick();
}

/******************************************************************************/
/* STM32H7xx Peripheral Interrupt Handlers                                    */
/******************************************************************************/

/**
  * @brief  This function handles USB OTG HS global interrupt.
  * @note   USB_OTG_HS 工作在主机模式（IMU 走 USB Host 时），
  *         所有主机事件（端口变化/通道完成）都在这里处理。
  */
void OTG_HS_IRQHandler(void)
{
  HAL_HCD_IRQHandler(&hhcd);
}

/**
  * @brief  This function handles EXTI line[9:5] interrupts.
  * @note   EXTI8 = PA8 = 编码器 Z 相（每圈一个零位脉冲）
  */
void EXTI9_5_IRQHandler(void)
{
  HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_8);
}

/************************ (C) COPYRIGHT *****END OF FILE************************/
