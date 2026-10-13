/**
  ******************************************************************************
  * @file    usb_host.h
  * @brief   IMU 走 USB Host（模块是标准 CDC 虚拟串口）——应用层接口
  ******************************************************************************
  */

#ifndef __USB_HOST_H
#define __USB_HOST_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32h7xx_hal.h"
#include "usbh_core.h"

/* 主机状态（LCD 上要显示，所以单独列出 ------------------------------------ */
typedef enum
{
  USB_HOST_STATE_OFF = 0U,     /* 还没初始化 */
  USB_HOST_STATE_INIT,         /* 正在初始化主机栈 */
  USB_HOST_STATE_WAITING,      /* 等设备插入 */
  USB_HOST_STATE_ENUMERATING,  /* 设备插上了，正在枚举 / 等类驱动 */
  USB_HOST_STATE_READY,        /* CDC 类就绪，正在收发数据 */
  USB_HOST_STATE_DISCONNECTED, /* 设备被拔掉 */
  USB_HOST_STATE_ERROR         /* 初始化失败 */
} USB_HostStateTypeDef;

/* 收到模块数据时的回调（在 USB_Host_Process() 里被调用）-------------------- */
typedef void (*USB_HostRxCallback)(const uint8_t *data, uint16_t len);

/* 全局句柄：hUsbHostHS = 主机库句柄；hhcd = HAL 的 HCD 句柄（中断里要用）-- */
extern USBH_HandleTypeDef hUsbHostHS;
extern HCD_HandleTypeDef  hhcd;

/* 接口 ----------------------------------------------------------------------*/
void     USB_Host_Init(void);
void     USB_Host_Process(void);
void     USB_Host_SetRxCallback(USB_HostRxCallback cb);
void     USB_Host_SendBytes(const uint8_t *data, uint32_t len);

USB_HostStateTypeDef USB_Host_GetState(void);
uint8_t  USB_Host_IsConnected(void);
uint8_t  USB_Host_IsReady(void);
uint16_t USB_Host_GetVid(void);
uint16_t USB_Host_GetPid(void);
uint8_t  USB_Host_GetInterfaceClass(void);
uint32_t USB_Host_GetRxBytes(void);
uint32_t USB_Host_GetTxDrop(void);

#ifdef __cplusplus
}
#endif

#endif /* __USB_HOST_H */

/************************ (C) COPYRIGHT *****END OF FILE************************/
