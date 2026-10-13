/**
  ******************************************************************************
  * @file    bsp_sd.h
  * @brief   SD 卡（SDMMC1）底层驱动接口
  *
  *  硬件：野火 STM32H750X/H743X 底板 TF 卡座（J28），走 SDMMC1 四线
  *        PC8  = SDMMC1_D0      PC9  = SDMMC1_D1
  *        PC10 = SDMMC1_D2      PC11 = SDMMC1_D3
  *        PC12 = SDMMC1_CK      PD2  = SDMMC1_CMD
  *        （这几根脚和板载 WIFI 复用 SDIO，板上不装 WIFI 模块就能直接用 TF 卡）
  ******************************************************************************
  */

#ifndef __BSP_SD_H
#define __BSP_SD_H

#include "stm32h7xx_hal.h"

/* SDMMC1 句柄：diskio.c 读写扇区时会用到 */
extern SD_HandleTypeDef hsd1;

/* 返回值：0 = 成功，其它 = 失败（一般是没插卡 / 卡不认） */
uint8_t SD_Init(void);
void    SD_DeInit(void);

/* 卡是否已初始化好 */
uint8_t SD_IsReady(void);

/* 当前用的是几线模式：0 = 没初始化成功，1 = 1 线模式，4 = 4 线模式 */
uint8_t SD_GetBusMode(void);
uint8_t SD_GetFailStep(void);        /* 0 = 没失败，1 = 认卡本身失败 */
uint8_t SD_GetFailErr(void);         /* HAL 真实错误码（3 = 命令响应超时 = 卡没应答）*/
uint8_t SD_GetNoCardStatus(void);

/* 读/写失败的诊断（关键）：错误码能直接指出失败原因 */
uint8_t   SD_GetReadStep(void);   /* 1=命令阶段 2=数据阶段 3=等卡回传输态 */
uint8_t   SD_GetReadErr(void);    /* HAL 错误码：1=命令CRC 2=数据CRC 3=命令无响应 4=数据超时 */
uint32_t  SD_GetReadSta(void);    /* 失败时 SDMMC1->STA 的值 */
    /* 1 = 这张卡不支持读 SD status（不影响使用）*/

/* 卡容量（单位：512 字节扇区） */
uint32_t SD_GetSectorCount(void);

/* 扇区读写：返回 HAL_OK / HAL_ERROR / HAL_TIMEOUT */
HAL_StatusTypeDef SD_ReadSectors(uint8_t *buf, uint32_t sector, uint32_t count);
HAL_StatusTypeDef SD_WriteSectors(const uint8_t *buf, uint32_t sector, uint32_t count);

/* 裸读 0 号扇区（引导扇区）自检：绕开 FatFs，直接验证 SD 驱动/硬件能不能读出数据。
   返回 0 = 成功，1 = 失败（原因看 SD_GetReadStep()/SD_GetReadErr()）；
   out4 可传 0，否则回填扇区头 4 个字节（FAT: EB 3C 90 / exFAT: EB 76 90）。*/
uint8_t SD_ProbeRawSector0(uint8_t *out4);

/* 手动切换总线宽度：1 = 4 线，0 = 1 线（绕开 HAL 的 SCR 读，见 bsp_sd.c 里的说明）*/
void    SD_TryBusWide(uint8_t fourBit);

#endif /* __BSP_SD_H */
