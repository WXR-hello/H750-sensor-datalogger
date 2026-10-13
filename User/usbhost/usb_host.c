/**
  ******************************************************************************
  * @file    usb_host.c
  * @brief   IMU 走 USB Host（模块是标准 CDC 虚拟串口）——初始化 / 枚举 / 收发
  *
  *  数据流（和原来的串口版一模一样，只是把“收字节”的地方换掉了）：
  *
  *    IWT603 模块 USB
  *      --> 主机 CDC 类 批量 IN 端点
  *      --> USBH_CDC_ReceiveCallback()      （本文件，库里的 __weak 回调）
  *      --> 注册进来的回调（main.c 的 SensorUsbInput）
  *      --> WitSerialDataIn()  -->  sReg[]  -->  LCD / SD 卡 CSV
  *
  *  发送方向（用来给模块发指令，比如“设成 10Hz、回传加速度+角速度+角度”）：
  *
  *    WitSerialWriteRegister() 的回调（main.c 的 SensorUsbSend）
  *      --> USB_Host_SendBytes()  -->  发送队列
  *      --> USB_Host_Process()    -->  CDC 批量 OUT 端点
  *
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "usb_host.h"
#include "usbh_cdc.h"

#include <string.h>

/* 全局句柄 -------------------------------------------------------------- */
USBH_HandleTypeDef hUsbHostHS;      /* 主机库句柄（hhcd 在 usbh_conf.c 里）*/

/* 参数 ------------------------------------------------------------------ */
#define USB_HOST_RX_BUFSIZE     64U      /* 一次批量 IN 的缓冲（模块一帧只有 11 字节）*/
#define USB_HOST_TX_BUFSIZE     64U      /* 一次批量 OUT 最多发多少字节 */
#define USB_HOST_TX_FIFO_SIZE   128U     /* 发送队列（存还没发出去的指令）*/

/* 缓冲与状态 ------------------------------------------------------------ */
static uint8_t  s_rxBuf[USB_HOST_RX_BUFSIZE];
static uint8_t  s_txBuf[USB_HOST_TX_BUFSIZE];
static uint8_t  s_txFifo[USB_HOST_TX_FIFO_SIZE];

static volatile uint16_t s_txFifoHead = 0U;
static volatile uint16_t s_txFifoTail = 0U;
static volatile uint8_t  s_txBusy     = 0U;
static volatile uint32_t s_txDrop     = 0U;

static volatile uint32_t s_rxBytes   = 0U;    /* 一共收到多少字节（诊断用）*/
static volatile uint8_t  s_classReady = 0U;   /* 1 = CDC 类就绪，可以收发 */
static volatile uint8_t  s_rxArmed    = 0U;   /* 1 = 已经挂了接收请求 */
static volatile uint8_t  s_state      = USB_HOST_STATE_OFF;

static USB_HostRxCallback s_rxCallback = 0;

/* 内部函数声明 ---------------------------------------------------------- */
static void USB_Host_ArmReceive(void);
static void USB_Host_PumpTx(void);
static uint8_t USB_Host_PhyPortConnect(void);
static void USBH_UserProcess(USBH_HandleTypeDef *phost, uint8_t id);

/* ==========================================================================
 * 主机库的用户回调：库在枚举的不同阶段回调这里
 * ========================================================================== */
static void USBH_UserProcess(USBH_HandleTypeDef *phost, uint8_t id)
{
  switch (id)
  {
    case HOST_USER_SELECT_CONFIGURATION:
      /* 只有一个配置，库自己会用 bConfigurationValue 去 SET_CONFIGURATION，
         这里不需要做任何事（老版本库里没有 USBH_SetCfg 可以调）*/
      break;

    case HOST_USER_CLASS_SELECTED:
      /* CDC 类驱动已经打开管道，等它拿完 line coding 就能收数据 */
      s_state = USB_HOST_STATE_ENUMERATING;
      break;

    case HOST_USER_CLASS_ACTIVE:
      /* 类就绪：开始收数据 */
      s_classReady = 1U;
      s_state      = USB_HOST_STATE_READY;
      USB_Host_ArmReceive();
      break;

    case HOST_USER_CONNECTION:
      s_state    = USB_HOST_STATE_ENUMERATING;
      s_rxArmed  = 0U;
      break;

    case HOST_USER_DISCONNECTION:
      /* 设备拔了：清状态、清发送队列，等下一次插入重新枚举 */
      s_classReady = 0U;
      s_rxArmed    = 0U;
      s_txBusy     = 0U;
      s_txFifoHead = 0U;
      s_txFifoTail = 0U;
      s_state      = USB_HOST_STATE_DISCONNECTED;
      break;

    case HOST_USER_UNRECOVERED_ERROR:
      s_state = USB_HOST_STATE_ERROR;
      break;

    default:
      break;
  }

  (void)phost;
}

/* ==========================================================================
 * 接收：库的 __weak 回调，在这里被“替换”成我们自己的实现
 * ========================================================================== */
void USBH_CDC_ReceiveCallback(USBH_HandleTypeDef *phost)
{
  uint16_t len;

  s_rxArmed = 0U;      /* 这一次接收完成了，可以再挂一次 */

  len = USBH_CDC_GetLastReceivedDataSize(phost);
  if (len > USB_HOST_RX_BUFSIZE)
  {
    len = USB_HOST_RX_BUFSIZE;
  }

  if (len > 0U)
  {
    s_rxBytes += (uint32_t)len;

    if (s_rxCallback != 0)
    {
      s_rxCallback(s_rxBuf, len);       /* 交给 main.c：喂给 wit_c_sdk */
    }
  }

  USB_Host_ArmReceive();                /* 立刻挂下一次接收，保证数据不丢 */
}

/* ==========================================================================
 * 发送：库的 __weak 回调，一次批量 OUT 发完
 * ========================================================================== */
void USBH_CDC_TransmitCallback(USBH_HandleTypeDef *phost)
{
  (void)phost;
  s_txBusy = 0U;
}

/* ==========================================================================
 * 对外接口
 * ========================================================================== */

/**
  * @brief  初始化 USB 主机（在 LCD 初始化之后调用，因为 USB 时钟来自 PLL3）
  */
void USB_Host_Init(void)
{
  s_state = USB_HOST_STATE_INIT;

  if (USBH_Init(&hUsbHostHS, USBH_UserProcess, 0U) != USBH_OK)
  {
    s_state = USB_HOST_STATE_ERROR;
    return;
  }

  if (USBH_RegisterClass(&hUsbHostHS, USBH_CDC_CLASS) != USBH_OK)
  {
    s_state = USB_HOST_STATE_ERROR;
    return;
  }

  if (USBH_Start(&hUsbHostHS) != USBH_OK)
  {
    s_state = USB_HOST_STATE_ERROR;
    return;
  }

  s_state = USB_HOST_STATE_WAITING;
}

/**
  * @brief  主循环里轮询：跑主机状态机 + 发队列 + 状态维护
  */
void USB_Host_Process(void)
{
  (void)USBH_Process(&hUsbHostHS);

  USB_Host_PumpTx();
  USB_Host_ArmReceive();

  /* 状态机：类就绪 > 设备已连接（枚举中/类请求中）> 其他 */
  if (s_classReady != 0U)
  {
    s_state = USB_HOST_STATE_READY;
  }
  else if ((hUsbHostHS.device.is_connected != 0U) || (USB_Host_PhyPortConnect() != 0U))
  {
    /* 第二种情况是“PHY 已经看到设备、但库还没开始枚举”，
       屏幕上显示成“正在查找设备”，正好用来区分接线问题和枚举问题 */
    s_state = USB_HOST_STATE_ENUMERATING;
  }
  else if ((s_state == USB_HOST_STATE_READY) || (s_state == USB_HOST_STATE_ENUMERATING))
  {
    s_state = USB_HOST_STATE_DISCONNECTED;
  }
  else
  {
    /* OFF / INIT / WAITING / ERROR：保持不变 */
  }
}

/**
  * @brief  注册“收到字节”的回调（main.c 里喂给 wit_c_sdk）
  */
void USB_Host_SetRxCallback(USB_HostRxCallback cb)
{
  s_rxCallback = cb;
}

/**
  * @brief  往模块发数据（发给 CDC 批量 OUT 端点）
  * @note   先放进队列，由 USB_Host_Process() 一片一片发出去；
  *         队列满了就丢掉并计数（正常用不到）。
  */
void USB_Host_SendBytes(const uint8_t *data, uint32_t len)
{
  uint32_t i;

  if (data == 0)
  {
    return;
  }

  for (i = 0U; i < len; i++)
  {
    uint16_t next = (uint16_t)((s_txFifoHead + 1U) % USB_HOST_TX_FIFO_SIZE);

    if (next == s_txFifoTail)
    {
      s_txDrop++;                      /* 队列满：丢弃 */
      break;
    }

    s_txFifo[s_txFifoHead] = data[i];
    s_txFifoHead = next;
  }
}

USB_HostStateTypeDef USB_Host_GetState(void)
{
  return (USB_HostStateTypeDef)s_state;
}

uint8_t USB_Host_IsConnected(void)
{
  return (hUsbHostHS.device.is_connected != 0U) ? 1U : 0U;
}

uint8_t USB_Host_IsReady(void)
{
  return s_classReady;
}

uint16_t USB_Host_GetVid(void)
{
  return hUsbHostHS.device.DevDesc.idVendor;
}

uint16_t USB_Host_GetPid(void)
{
  return hUsbHostHS.device.DevDesc.idProduct;
}

uint8_t USB_Host_GetInterfaceClass(void)
{
  return hUsbHostHS.device.CfgDesc.Itf_Desc[0].bInterfaceClass;
}

uint32_t USB_Host_GetRxBytes(void)
{
  return s_rxBytes;
}

uint32_t USB_Host_GetTxDrop(void)
{
  return s_txDrop;
}

/* ==========================================================================
 * 内部函数
 * ========================================================================== */

/**
  * @brief  挂一次“批量 IN”接收请求（已经在挂了就不重复挂）
  */
static uint8_t USB_Host_PhyPortConnect(void)
{
  /* HPRT 寄存器在 USB 外设基址 + 0x440（H7 的 CMSIS 里有定义）*/
  uint32_t hprt0 = *(__IO uint32_t *)((uint32_t)USB_OTG_HS + USB_OTG_HOST_PORT_BASE);

  /* PCSTS = Port Connect Status：PHY 在 D+/D- 上看到了设备的上拉 = 插上了 */
  return ((hprt0 & USB_OTG_HPRT_PCSTS) != 0U) ? 1U : 0U;
}

/**
  * @brief  挂一次“批量 IN”接收请求（已经在挂了就不重复挂）
  */
static void USB_Host_ArmReceive(void)
{
  if ((s_classReady == 0U) || (s_rxArmed != 0U))
  {
    return;
  }

  if (USBH_CDC_Receive(&hUsbHostHS, s_rxBuf, USB_HOST_RX_BUFSIZE) == USBH_OK)
  {
    s_rxArmed = 1U;
  }
}

/**
  * @brief  把发送队列里的一批字节发出去（一次只发一批，发完靠回调再发下一批）
  */
static void USB_Host_PumpTx(void)
{
  uint16_t n = 0U;
  uint16_t tail;

  if ((s_classReady == 0U) || (s_txBusy != 0U))
  {
    return;
  }

  if (s_txFifoHead == s_txFifoTail)
  {
    return;
  }

  tail = s_txFifoTail;
  while ((n < USB_HOST_TX_BUFSIZE) && (s_txFifoHead != tail))
  {
    s_txBuf[n] = s_txFifo[tail];
    tail = (uint16_t)((tail + 1U) % USB_HOST_TX_FIFO_SIZE);
    n++;
  }

  s_txBusy = 1U;

  if (USBH_CDC_Transmit(&hUsbHostHS, s_txBuf, (uint32_t)n) != USBH_OK)
  {
    /* 类驱动此刻没空（比如上一次还没发完）：把队列读指针退回去，下次再试 */
    s_txBusy = 0U;
  }
  else
  {
    s_txFifoTail = tail;
  }
}

/************************ (C) COPYRIGHT *****END OF FILE************************/
