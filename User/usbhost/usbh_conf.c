/**
  ******************************************************************************
  * @file    usbh_conf.c
  * @brief   ST USB Host 库的底层粘合层（HAL_HCD）+ 静态内存池
  *
  *  主机库（usbh_core.c / usbh_pipes.c / usbh_ioreq.c / usbh_ctlreq.c）
  *  不直接碰寄存器，它们只调用这里的一组 USBH_LL_xxx() 函数；
  *  这里再把每个调用翻译成 HAL 的 HAL_HCD_xxx()。
  *
  *  硬件：STM32H750 的 USB_OTG_HS，用片内全速 PHY（D+/D- = PB14/PB15），
  *        对应底板上的 USB HOST 口（USB-A 大方口）。
  *        主机口的 5V 由底板电源电路硬供，MCU 不控制 VBUS 开关，
  *        所以这里不做 VBUS 驱动、也不打开 VBUS 检测。
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "usbh_conf.h"
#include "usbh_def.h"
#include "usb_host.h"

/* HCD 句柄（HAL 的 USB 主机控制器驱动），在 stm32h7xx_it.c 里被中断引用 */
HCD_HandleTypeDef hhcd;

/* ==========================================================================
 * 静态内存池：代替 libc 的 malloc/free
 *   MicroLIB 的堆只有 512 字节（startup_stm32h750xx.s 里的 Heap_Size），
 *   而主机库只在一处申请内存（CDC 类的句柄，约 100 字节），
 *   所以用一个固定数组 + 首次适配就够了，不引入堆。
 * ========================================================================== */
#define USBH_MEM_POOL_SIZE   512U
#define USBH_MEM_ALIGN       8U

static uint8_t  s_usbhPool[USBH_MEM_POOL_SIZE];
static uint32_t s_usbhPoolUsed = 0U;

void *USBH_malloc(uint32_t size)
{
  uint32_t aligned = (size + (USBH_MEM_ALIGN - 1U)) & ~(USBH_MEM_ALIGN - 1U);

  if ((aligned == 0U) || ((s_usbhPoolUsed + aligned) > USBH_MEM_POOL_SIZE))
  {
    return NULL;
  }

  {
    void *ptr = (void *)&s_usbhPool[s_usbhPoolUsed];
    s_usbhPoolUsed += aligned;
    return ptr;
  }
}

void USBH_free(void *ptr)
{
  /* 整个池只有一次申请（CDC 句柄），设备拔出时整池回收即可 */
  if ((uint8_t *)ptr == s_usbhPool)
  {
    s_usbhPoolUsed = 0U;
  }
}

/* ==========================================================================
 * 主机库要求的底层函数（HAL_HCD 版）
 * ========================================================================== */

/**
  * @brief  初始化 USB 主机控制器（主机库 USBH_Init 里调用）
  */
USBH_StatusTypeDef USBH_LL_Init(USBH_HandleTypeDef *phost)
{
  /* 主机库和 HCD 互相引用：库的句柄挂在 HCD 上，HCD 句柄挂在库的句柄上 */
  phost->pData = &hhcd;

  hhcd.Instance = USB_OTG_HS;
  hhcd.pData    = phost;

  hhcd.Init.dev_endpoints            = 9U;
  hhcd.Init.Host_channels            = 12U;
  hhcd.Init.dma_enable               = 0U;               /* 不用 DMA：CPU 读写 FIFO，免去 Cache 一致性处理 */
  hhcd.Init.speed                    = HCD_SPEED_FULL;   /* 片内 PHY 只支持全速 */
  hhcd.Init.ep0_mps                  = 64U;
  hhcd.Init.phy_itface               = HCD_PHY_EMBEDDED; /* 片内全速 PHY（PB14/PB15）*/
  hhcd.Init.Sof_enable               = 0U;
  hhcd.Init.low_power_enable         = 0U;
  hhcd.Init.lpm_enable               = 0U;
  hhcd.Init.battery_charging_enable  = 0U;
  hhcd.Init.vbus_sensing_enable      = 0U;               /* 主机口 5V 硬供，不做 VBUS 检测 */
  hhcd.Init.use_dedicated_ep1        = 0U;
  hhcd.Init.use_external_vbus        = 0U;

  if (HAL_HCD_Init(&hhcd) != HAL_OK)
  {
    return USBH_FAIL;
  }

  /* 把主机库的软件计时器和 HCD 的帧计数对齐（库用它做控制传输超时）*/
  USBH_LL_SetTimer(phost, HAL_HCD_GetCurrentFrame(&hhcd));

  return USBH_OK;
}

/**
  * @brief  反初始化主机控制器
  */
USBH_StatusTypeDef USBH_LL_DeInit(USBH_HandleTypeDef *phost)
{
  (void)phost;
  (void)HAL_HCD_DeInit(&hhcd);
  return USBH_OK;
}

/**
  * @brief  启动主机：开端口电源 + 开全局中断
  */
USBH_StatusTypeDef USBH_LL_Start(USBH_HandleTypeDef *phost)
{
  (void)phost;
  (void)HAL_HCD_Start(&hhcd);
  return USBH_OK;
}

/**
  * @brief  停止主机
  */
USBH_StatusTypeDef USBH_LL_Stop(USBH_HandleTypeDef *phost)
{
  (void)phost;
  (void)HAL_HCD_Stop(&hhcd);
  return USBH_OK;
}

/**
  * @brief  复位端口（枚举的第一步）
  */
USBH_StatusTypeDef USBH_LL_ResetPort(USBH_HandleTypeDef *phost)
{
  (void)phost;
  (void)HAL_HCD_ResetPort(&hhcd);
  return USBH_OK;
}

/**
  * @brief  当前设备速度（全速/低速）
  */
USBH_SpeedTypeDef USBH_LL_GetSpeed(USBH_HandleTypeDef *phost)
{
  USBH_SpeedTypeDef speed;

  (void)phost;

  switch (HAL_HCD_GetCurrentSpeed(&hhcd))
  {
    case 0U:  speed = USBH_SPEED_HIGH;  break;   /* 片内 PHY 不会出现高速 */
    case 1U:  speed = USBH_SPEED_FULL;  break;
    default:  speed = USBH_SPEED_LOW;   break;
  }

  return speed;
}

/**
  * @brief  主机口 VBUS 开关：本板 5V 是硬供的，这里不需要做任何事
  */
USBH_StatusTypeDef USBH_LL_DriverVBUS(USBH_HandleTypeDef *phost, uint8_t state)
{
  (void)phost;
  (void)state;
  return USBH_OK;
}

/**
  * @brief  打开一个主机通道（管道）
  */
USBH_StatusTypeDef USBH_LL_OpenPipe(USBH_HandleTypeDef *phost,
                                    uint8_t pipe,
                                    uint8_t epnum,
                                    uint8_t dev_address,
                                    uint8_t speed,
                                    uint8_t ep_type,
                                    uint16_t mps)
{
  (void)phost;

  if (HAL_HCD_HC_Init(&hhcd, pipe, epnum, dev_address, speed, ep_type, mps) != HAL_OK)
  {
    return USBH_FAIL;
  }

  return USBH_OK;
}

/**
  * @brief  关闭一个主机通道
  */
USBH_StatusTypeDef USBH_LL_ClosePipe(USBH_HandleTypeDef *phost, uint8_t pipe)
{
  (void)phost;
  (void)HAL_HCD_HC_Halt(&hhcd, pipe);
  return USBH_OK;
}

/**
  * @brief  发起一次传输（控制/批量/中断）
  */
USBH_StatusTypeDef USBH_LL_SubmitURB(USBH_HandleTypeDef *phost,
                                     uint8_t pipe,
                                     uint8_t direction,
                                     uint8_t ep_type,
                                     uint8_t token,
                                     uint8_t *pbuff,
                                     uint16_t length,
                                     uint8_t do_ping)
{
  (void)phost;

  if (HAL_HCD_HC_SubmitRequest(&hhcd, pipe, direction, ep_type, token,
                               pbuff, length, do_ping) != HAL_OK)
  {
    return USBH_FAIL;
  }

  return USBH_OK;
}

/**
  * @brief  查询一次传输的结果
  * @note   主机库的 URB 状态枚举（USBH_URB_xxx）和 HAL 的（URB_xxx）
  *         前 6 个值顺序完全一致，所以直接强制转换即可。
  */
USBH_URBStateTypeDef USBH_LL_GetURBState(USBH_HandleTypeDef *phost, uint8_t pipe)
{
  (void)phost;
  return (USBH_URBStateTypeDef)HAL_HCD_HC_GetURBState(&hhcd, pipe);
}

/**
  * @brief  取上一次传输实际传了多少字节
  */
uint32_t USBH_LL_GetLastXferSize(USBH_HandleTypeDef *phost, uint8_t pipe)
{
  (void)phost;
  return HAL_HCD_HC_GetXferCount(&hhcd, pipe);
}

/**
  * @brief  设置数据翻转位（DATA0/DATA1）
  */
USBH_StatusTypeDef USBH_LL_SetToggle(USBH_HandleTypeDef *phost, uint8_t pipe, uint8_t toggle)
{
  (void)phost;

  if (pipe >= 16U)
  {
    return USBH_FAIL;
  }

  if (hhcd.hc[pipe].ep_is_in != 0U)
  {
    hhcd.hc[pipe].toggle_in = toggle;
  }
  else
  {
    hhcd.hc[pipe].toggle_out = toggle;
  }

  return USBH_OK;
}

/**
  * @brief  读取数据翻转位
  */
uint8_t USBH_LL_GetToggle(USBH_HandleTypeDef *phost, uint8_t pipe)
{
  uint8_t toggle = 0U;

  (void)phost;

  if (pipe >= 16U)
  {
    return 0U;
  }

  if (hhcd.hc[pipe].ep_is_in != 0U)
  {
    toggle = hhcd.hc[pipe].toggle_in;
  }
  else
  {
    toggle = hhcd.hc[pipe].toggle_out;
  }

  return toggle;
}

/**
  * @brief  主机库内部用的延时
  */
void USBH_Delay(uint32_t Delay)
{
  HAL_Delay(Delay);
}

/* ==========================================================================
 * HAL HCD 回调 -> 主机库的“事件入口”
 *
 *   这一步是**必须的**，也是最容易漏的：
 *   主机库的 USBH_Process() 状态机停在 HOST_IDLE 时，只有看到
 *   phost->device.is_connected == 1 才会往下走（复位端口 -> 枚举 -> 找类），
 *   而 is_connected 只在 USBH_LL_Connect() 里被置 1；
 *   同理 PortEnabled 只在 USBH_LL_PortEnabled() 里被置 1。
 *
 *   这几个 USBH_LL_xxx 事件函数都实现在库自己的 usbh_core.c 里，
 *   库不会主动调用它们，必须由应用在 HAL 的 HCD 回调里桥接
 *   （ST 老版本的 usbh_conf.c 模板就是这几行）。
 *   少了这层桥接，屏幕会永远停在“USB 等待设备”，
 *   即使模块插得好好的、PHY 也已经看到设备。
 * ========================================================================== */

/**
  * @brief  端口检测到设备插入（HAL 在 HPRT 的 PCDET 中断里回调）
  */
void HAL_HCD_Connect_Callback(HCD_HandleTypeDef *hhcd_dev)
{
  if (hhcd_dev->pData != NULL)
  {
    (void)USBH_LL_Connect((USBH_HandleTypeDef *)hhcd_dev->pData);
  }
}

/**
  * @brief  设备被拔出
  */
void HAL_HCD_Disconnect_Callback(HCD_HandleTypeDef *hhcd_dev)
{
  if (hhcd_dev->pData != NULL)
  {
    (void)USBH_LL_Disconnect((USBH_HandleTypeDef *)hhcd_dev->pData);
  }
}

/**
  * @brief  端口使能完成（复位成功、可以发令牌了）
  */
void HAL_HCD_PortEnabled_Callback(HCD_HandleTypeDef *hhcd_dev)
{
  if (hhcd_dev->pData != NULL)
  {
    USBH_LL_PortEnabled((USBH_HandleTypeDef *)hhcd_dev->pData);
  }
}

/**
  * @brief  端口被关闭
  */
void HAL_HCD_PortDisabled_Callback(HCD_HandleTypeDef *hhcd_dev)
{
  if (hhcd_dev->pData != NULL)
  {
    USBH_LL_PortDisabled((USBH_HandleTypeDef *)hhcd_dev->pData);
  }
}

/**
  * @brief  每帧（SOF）回调：主机库用它做超时计时
  * @note   库里的控制传输超时判断用的是 phost->Timer，
  *         不递增的话超时永远不会到，出错时就会卡住。
  */
void HAL_HCD_SOF_Callback(HCD_HandleTypeDef *hhcd_dev)
{
  if (hhcd_dev->pData != NULL)
  {
    USBH_LL_IncTimer((USBH_HandleTypeDef *)hhcd_dev->pData);
  }
}

/************************ (C) COPYRIGHT *****END OF FILE************************/
