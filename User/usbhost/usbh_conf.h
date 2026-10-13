/**
  ******************************************************************************
  * @file    usbh_conf.h
  * @brief   ST USB Host 库（2015 版，USBH_Init 三参数）的平台配置
  *
  *  本文件只负责“和平台相关”的配置，串口/枚举逻辑在 usb_host.c 里：
  *    1) 主机库的容量：端点/接口/类数量、配置描述符和控制传输缓冲大小；
  *    2) 内存接口：本工程用 MicroLIB，堆只有 512 字节，所以不用 libc 的
  *       malloc/free，改成 usbh_conf.c 里的静态内存池；
  *    3) 调试日志：USBH_DEBUG_LEVEL = 0，全部编译成空语句（本工程没有
  *       调试串口，打印不出去，还白占 Flash）。
  ******************************************************************************
  */

#ifndef __USBH_CONF_H
#define __USBH_CONF_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include <string.h>
#include "stm32h7xx_hal.h"

/* 主机库容量配置 -------------------------------------------------------------
 * IWT603 模块的 USB 是标准 CDC（虚拟串口）：
 *   通信接口（class 0x02）：1 个中断端点（通知用）
 *   数据接口（class 0x0A）：2 个批量端点（IN 收数据 / OUT 发数据）
 * 所以接口数至少要 2，每个接口的端点数至少要 2。
 */
#define USBH_MAX_NUM_ENDPOINTS                2U
#define USBH_MAX_NUM_INTERFACES               2U
#define USBH_MAX_NUM_CONFIGURATION            1U
#define USBH_MAX_NUM_SUPPORTED_CLASS          1U      /* 只注册 CDC 一个类 */

#define USBH_MAX_SIZE_CONFIGURATION           0x200U  /* 512B：配置描述符原始缓冲 */
#define USBH_MAX_DATA_BUFFER                  0x200U  /* 512B：控制传输数据缓冲   */

#define USBH_DEBUG_LEVEL                      0U      /* 0 = 关日志（省 Flash） */
#define USBH_USE_OS                           0U      /* 0 = 裸机，主循环里轮询 USBH_Process */

/* 内存接口：改用静态池，实现在 usbh_conf.c ------------------------------- */
void  *USBH_malloc(uint32_t size);
void   USBH_free(void *ptr);
#define USBH_memset                           memset
#define USBH_memcpy                           memcpy

/* 日志宏（USBH_DEBUG_LEVEL = 0 时全部是空语句）-------------------------- */
#if (USBH_DEBUG_LEVEL > 0U)
#define USBH_UsrLog(...)   do { printf(__VA_ARGS__); printf("\n"); } while (0)
#define USBH_ErrLog(...)   do { printf("ERROR: "); printf(__VA_ARGS__); printf("\n"); } while (0)
#define USBH_DbgLog(...)   do { printf("DBG: "); printf(__VA_ARGS__); printf("\n"); } while (0)
#else
#define USBH_UsrLog(...)   do { } while (0)
#define USBH_ErrLog(...)   do { } while (0)
#define USBH_DbgLog(...)   do { } while (0)
#endif

#ifdef __cplusplus
}
#endif

#endif /* __USBH_CONF_H */

/************************ (C) COPYRIGHT *****END OF FILE************************/
