/**
  ******************************************************************************
  * @file    imu.c
  * @brief   IMU（IWT603，维特 wit_c_sdk）初始化与轮询，走 USB Host
  *
  *   模块的 USB 是标准 CDC 虚拟串口：
  *     收字节：CDC 批量 IN 端点 -> SensorUsbInput() -> SensorRxByte() -> wit_c_sdk 组帧
  *     发指令：WitReadReg()/WitSetOutputRate() -> SensorUsbSend() -> CDC 批量 OUT 端点
  *
  *   对外接口：Imu_Init() 开机调用一次（必须在 LCD_Init() 之后，
  *             因为 USB 的 48MHz 时钟由 LCD 配好的 PLL3Q 提供）；
  *             Imu_Task() 主循环里周期调用。
  ******************************************************************************
  */

#include "main.h"
#include "imu.h"
#include "wit_c_sdk.h"
#include "./usbhost/usb_host.h"   /* IMU 走 USB Host（模块的 USB 是标准 CDC 虚拟串口）*/


/* 参数 ----------------------------------------------------------------*/
#define IMU_SETUP_ON_BOOT    1         /* 1 = 找到模块后把回传率/内容设成固定值 */
#define USB_PROBE_MS         3000U     /* USB 版：这么久没收到数据，就发一条“读加速度”探一下 */


/* wit_c_sdk 的数据更新标志位（和官方例程一致） */
#define ACC_UPDATE      0x01
#define GYRO_UPDATE     0x02
#define ANGLE_UPDATE    0x04
#define MAG_UPDATE      0x08
#define READ_UPDATE     0x80


/* 全局变量 ------------------------------------------------------------*/

static volatile char s_cDataUpdate = 0;

volatile uint32_t g_rxBytes = 0U; /* 模块口收到的字节数（诊断用）*/
volatile uint32_t g_frames   = 0U; /* IMU 帧计数（每收到一帧角度数据 +1）*/
volatile uint32_t g_lastFrame = 0U; /* 最近一帧 IMU 数据的时刻(ms) */
uint32_t g_fps      = 0U;          /* 实际帧率（帧/秒）*/
uint32_t g_ageMs    = 0U;          /* 画屏时最新数据有多旧(ms) */
uint32_t g_gapMaxMs = 0U;          /* 上一秒内相邻两帧最大间隔(ms) */
uint32_t g_rxBps    = 0U;          /* 每秒收到的字节数 */
static uint8_t  g_modCfgOk = 0U;          /* bit0/bit1 = 已回读到回传率/回传内容 */
uint8_t  g_modRate  = 0U;          /* 模块里实际的“回传率”寄存器值 */
uint8_t  g_modRsw   = 0U;          /* 模块里实际的“回传内容”寄存器值 */
static uint8_t  g_cfgFix   = 0U;          /* 已经尝试重设参数几次 */
static uint32_t g_cfgTick  = 0U;          /* 上次回读/校验模块参数的时刻 */
static uint32_t g_lastRxTick  = 0U;      /* 最后一次收到字节的时刻 */

static uint8_t  g_usbReady    = 0U;      /* 1 = USB 的 CDC 类就绪（可以收数据）*/
static uint32_t g_usbProbeTick = 0U;     /* 上一次发探测指令的时刻 */


/* ---- 找到模块后把回传率和回传内容固定下来，保证记录格式不变 ---- */
static void ImuConfigure(void)
{
  (void)WitSetOutputRate(RRATE_100HZ);                    /* 20Hz 回传*/
  (void)WitSetContent(RSW_ACC | RSW_GYRO | RSW_ANGLE);   /* 加速度 + 角速度 + 角度 */
}

/* ---- 收到一个字节：两条链路都从这里进（计数 + 喂给 wit_c_sdk）---- */
static void SensorRxByte(uint8_t b)
{
  g_rxBytes++;                             /* 诊断用：模块那边一共收到多少字节 */
  g_lastRxTick = HAL_GetTick();
  WitSerialDataIn(b);                      /* 交给 wit_c_sdk 组帧、解析 */
}

/* ---- USB 版：主机回调送来的数据（在 USB_Host_Process() 里被调用）---- */
static void SensorUsbInput(const uint8_t *data, uint16_t len)
{
  uint16_t i;

  for (i = 0; i < len; i++)
  {
    SensorRxByte(data[i]);
  }
}

/* ---- USB 版：wit_c_sdk 要发指令时，塞进 CDC 的发送队列 ---- */
static void SensorUsbSend(uint8_t *p_data, uint32_t uiSize)
{
  USB_Host_SendBytes(p_data, uiSize);
}

static void SensorDataUpdata(uint32_t uiReg, uint32_t uiRegNum)
{
  uint32_t i;

  for (i = 0; i < uiRegNum; i++)
  {
    switch (uiReg)
    {
      case AZ: s_cDataUpdate |= ACC_UPDATE;     break;
      case GZ: s_cDataUpdate |= GYRO_UPDATE;    break;
      case HZ: s_cDataUpdate |= MAG_UPDATE;     break;
      case RRATE: s_cDataUpdate |= READ_UPDATE;
                  g_modRate = (uint8_t)(sReg[RRATE] & 0x00FF);
                  g_modCfgOk |= 0x01U;
                  break;
      case RSW:   s_cDataUpdate |= READ_UPDATE;
                  g_modRsw = (uint8_t)(sReg[RSW] & 0x00FF);
                  g_modCfgOk |= 0x02U;
                  break;
      case Yaw: s_cDataUpdate |= ANGLE_UPDATE;
                g_frames++;
                g_lastFrame = HAL_GetTick();
                break;
      default: s_cDataUpdate |= READ_UPDATE;    break;
    }
    uiReg++;
  }
}

static void Delayms(uint16_t ucMs)
{
  HAL_Delay(ucMs);                                             /* 对应官方 delay_ms() */
}


/**
  * @brief  IMU 初始化：把 wit_c_sdk 和 USB Host 接起来。开机调用一次。
  *         注意：USB 的 48MHz 时钟由 LCD_Init() 里的 PLL3 提供，
  *               所以必须先初始化 LCD，再调用本函数。
  */
void Imu_Init(void)
{
  /* ---- wit_c_sdk 初始化，和官方例程顺序一致 ---- */
  WitInit(WIT_PROTOCOL_NORMAL, 0x50);
  WitSerialWriteRegister(SensorUsbSend);      /* 给模块发指令 -> CDC 批量 OUT 端点 */
  USB_Host_SetRxCallback(SensorUsbInput);     /* 收到模块数据 -> 这里喂给 SDK */
  WitRegisterCallBack(SensorDataUpdata);
  WitDelayMsRegister(Delayms);

  /* ---- USB 内核时钟必须是 48MHz：PLL3 已经由 LCD_Init() 配好（432/9=48MHz），
          这里只做两件事：打开 PLL3 的 Q 输出、把 USB 外设的时钟源指到 PLL3Q。
          注意不要在这里重新配置 PLL3（那会让 LTDC 闪一下）。---- */
  __HAL_RCC_PLL3CLKOUT_ENABLE(RCC_PLL3_DIVQ);
  __HAL_RCC_USB_CONFIG(RCC_USBCLKSOURCE_PLL3);

  /* ---- 启动 USB 主机：初始化主机栈 + 注册 CDC 类 + 开始等设备插入 ---- */
  USB_Host_Init();
}

/**
  * @brief  IMU 轮询：主循环里周期调用，跑 USB 主机状态机
  *         （枚举 / 收数据 / 发指令都在里面）。
  *         收到的数据由 wit_c_sdk 在回调里写进 sReg[]。
  */
void Imu_Task(void)
{
  /* ---- 跑 USB 主机状态机 ---- */
  USB_Host_Process();

  if (USB_Host_IsReady() != 0U)
  {
    if (g_usbReady == 0U)
    {
      /* 刚枚举好：把回传率和回传内容设成固定值，保证 CSV 格式固定 */
      g_usbReady     = 1U;
      g_usbProbeTick = HAL_GetTick();
#if IMU_SETUP_ON_BOOT
      ImuConfigure();
#endif
    }

    /* ---- 每 5 秒回读一次模块的“回传率/回传内容”：
      回读到不是 20Hz（RRATE_100HZ = 0x07）说明设置命令没生效，就重设一次（最多 3 次）。
      注意：只重设、不写模块 Flash，避免动模块里的设置。*/
    if ((HAL_GetTick() - g_cfgTick) >= 5000U)
    {
      g_cfgTick = HAL_GetTick();

      if (((g_modCfgOk & 0x01U) != 0U) && (g_modRate != RRATE_100HZ) && (g_cfgFix < 3U))
      {
        g_cfgFix++;
        ImuConfigure();               /* 只重新设 20Hz + 回传内容，不写模块 Flash */
      }

      (void)WitReadReg(RRATE, 1);     /* 回读回传率 */
      (void)WitReadReg(RSW, 1);       /* 回读回传内容 */
    }

    /* 3 秒收不到数据就发一条“读加速度”探一下：
       既能确认模块还在，也能把可能停掉的回传叫醒 */
    if (((HAL_GetTick() - g_lastRxTick) >= USB_PROBE_MS) &&
        ((HAL_GetTick() - g_usbProbeTick) >= USB_PROBE_MS))
    {
      g_usbProbeTick = HAL_GetTick();
      WitReadReg(AX, 3);
    }
  }
  else
  {
    g_usbReady = 0U;      /* 拔了就回到未就绪，插上后重新枚举 */
  }

  s_cDataUpdate = 0;           /* 清掉本轮的“数据已更新”标志 */
}

/************************ (C) COPYRIGHT *****END OF FILE************************/
