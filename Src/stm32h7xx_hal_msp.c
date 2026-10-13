/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    stm32h7xx_hal_msp.c
  * @brief   HAL 底层初始化：USB_OTG_HS（IMU 走 USB Host）。
  *
  *   注意：SD 卡（SDMMC1）的底层初始化 HAL_SD_MspInit()/HAL_SD_MspDeInit()
  *         放在 User/sd/bsp_sd.c 里，和 SD 驱动放在一起，方便移植。
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "main.h"

/**
  * @brief  Initializes the Global MSP.
  */
void HAL_MspInit(void)
{
  /* 打开 SYSCFG 时钟：HAL 里做外部中断（EXTI）和 IO 重映射要用 */
  __HAL_RCC_SYSCFG_CLK_ENABLE();
}

/**
  * @brief  初始化 USB 主机的底层硬件（USB_OTG_HS，IMU 走 USB 时用到）
  * @param  hhcd HCD 句柄（HAL_HCD_Init() 里自动回调到这里）
  *
  *   引脚：PB14 = USB_OTG_HS_DM（D-），PB15 = USB_OTG_HS_DP（D+），都用 AF12。
  *         这两个脚接到底板的 USB HOST 口（USB-A 大方口）。
  *
  *   特别注意：STM32H7 用片内全速 PHY 时，必须打开 USB 3.3V 电压检测器
  *             （HAL_PWREx_EnableUSBVoltageDetector，即 PWR_CR3 的 USB33DEN），
  *             否则 PHY 不工作、设备插上也枚举不了。
  */
void HAL_HCD_MspInit(HCD_HandleTypeDef *hhcd)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  if (hhcd->Instance == USB_OTG_HS)
  {
    /* 1) 打开 OTG_HS 外设时钟，并复位一次（上电时 USB 内核可能留着旧状态） */
    __HAL_RCC_USB1_OTG_HS_CLK_ENABLE();
    __HAL_RCC_USB1_OTG_HS_FORCE_RESET();
    HAL_Delay(1U);
    __HAL_RCC_USB1_OTG_HS_RELEASE_RESET();
    HAL_Delay(1U);

    /* 2) 片内全速 PHY 必须使能 3.3V 电压检测器 */
    HAL_PWREx_EnableUSBVoltageDetector();

    /* 3) PB14/PB15 -> USB_OTG_HS 的 D-/D+ */
    __HAL_RCC_GPIOB_CLK_ENABLE();

    GPIO_InitStruct.Pin       = GPIO_PIN_14 | GPIO_PIN_15;
    GPIO_InitStruct.Mode      = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull      = GPIO_NOPULL;
    GPIO_InitStruct.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF12_OTG1_FS;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    /* 4) USB 主机全局中断（处理函数在 stm32h7xx_it.c 里） */
    HAL_NVIC_SetPriority(OTG_HS_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(OTG_HS_IRQn);
  }
}

/**
  * @brief  反初始化 USB 主机的底层硬件
  */
void HAL_HCD_MspDeInit(HCD_HandleTypeDef *hhcd)
{
  if (hhcd->Instance == USB_OTG_HS)
  {
    __HAL_RCC_USB1_OTG_HS_CLK_DISABLE();

    HAL_GPIO_DeInit(GPIOB, GPIO_PIN_14 | GPIO_PIN_15);

    HAL_NVIC_DisableIRQ(OTG_HS_IRQn);
  }
}

/************************ (C) COPYRIGHT *****END OF FILE************************/
