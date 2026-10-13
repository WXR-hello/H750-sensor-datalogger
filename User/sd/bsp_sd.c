/**
  ******************************************************************************
  * @file    bsp_sd.c
  * @brief   SD 卡（SDMMC1）底层驱动
  *
  *  时钟：SDMMC1 内核时钟取 PLL1Q = 25MHz / 5 * 160 / 4 = 200MHz
  *        SDMMC_CK = 内核时钟 / (2 * ClockDiv)，默认 ClockDiv = 4 -> 25MHz
  *        （认卡阶段 HAL 自己会用 400kHz 慢时钟，认完再切到 25MHz）
  *
  *  读写方式：HAL_SD_ReadBlocks() / HAL_SD_WriteBlocks() 轮询方式，
  *            CPU 直接搬 SDMMC 的 FIFO，不用 SDMMC 内部 IDMA，
  *            因此不牵扯 D-Cache 一致性问题（缓冲区不需要特殊对齐或维护）。
  ******************************************************************************
  */

#include "./sd/bsp_sd.h"
#include "./mpu/bsp_mpu.h"       /* MPU 配置在 main.c 的 LCD_DisplayInit() 里做，这里只保留包含 */

/* SDMMC 转移时钟 = 内核时钟(PLL1Q=200MHz) / (2 * SD_SDMMC_CLKDIV)：
     16 -> 6.25MHz   ← 默认（比原来一直跑的 200kHz 快 30 倍，时序裕量仍然很大）
      4 -> 25MHz     ← 卡和板子都正常之后可以改成这个（SD 默认速度上限）
    500 -> 200kHz    ← 2026-09-25 用过的最慢档，只用来验证 DAT0 的电气余量
   认卡阶段（CMD0/CMD8/ACMD41...）HAL 自己用 ~400kHz，这个宏只影响认卡之后的读写。
   注意：真正的“读卡失败”不是靠降速能解决的，降速只影响快慢和一点时序裕量。*/
#define SD_SDMMC_CLKDIV      12U
/* 读写失败重试次数：数据 CRC 错多是偶发的（接触/干扰），重发几次通常就过了 */
#define SD_OP_RETRY          5U

/* 是否使用 4 线模式（SD_USE_4BIT = 1 才开）。
   2026-09-25 默认改成 0（只用 1 线）：4 线要用 DAT1/DAT2/DAT3 三根线，
   只要其中一根接触不好，就会出现"卡认到了、但读卡失败"。
   本工程 10Hz、每秒几十字节的数据量，1 线 6.25MHz 完全够用。 */
#define SD_USE_4BIT          0U
#define SD_OP_TIMEOUT        5000U     /* 单次读写超时(ms) */
#define SD_STATE_TIMEOUT     1000U     /* 等卡回到传输态的超时(ms) */

SD_HandleTypeDef hsd1;

/* 裸读自检用的 512 字节缓冲区：和 FatFs 的缓冲区一样放 .sd_dma（AXI SRAM），
   开机时已由 Src/main.c 的 LCD_DisplayInit() 整块配成 non-cacheable */
#define SD_DMA_SECTION   __attribute__((section(".sd_dma")))
SD_DMA_SECTION static uint8_t s_probeBuf[512];

static uint8_t s_sdReady = 0;
static uint8_t s_sdBusMode = 0;      /* 0 = 还没成功，1 = 1 线模式，4 = 4 线模式 */
static uint8_t  s_failStep = 0;       /* 认卡流程失败在第几步（1 = 认卡本身）*/
static uint8_t  s_failErr  = 0;       /* HAL 报的真实错误码（在 DeInit 清零前抓的）*/
static uint8_t  s_failCntStatus = 0;  /* 1 = 这张卡不支持读 SD status（不影响使用）*/
/* 把 HAL 的 32 位 ErrorCode 归一化成好显示的小码（0~99）。
 *
 *  对照表（低位几个最常见，和 stm32h7xx_ll_sdmmc.h 里的 SDMMC_ERROR_xxx 一致）：
 *     1 = CMD_CRC_FAIL       命令响应 CRC 错（干扰 / 接触不良）
 *     2 = DATA_CRC_FAIL      数据 CRC 错（DAT0 信号 / 接触 / 干扰）
 *     4 = CMD_RSP_TIMEOUT    命令没有响应（卡没应答 —— 认卡失败最常见）
 *     8 = DATA_TIMEOUT       数据阶段超时（卡收不到 / 发不出数据）
 *    16 = TX_UNDERRUN        发送 FIFO 下溢
 *    32 = RX_OVERRUN         接收 FIFO 上溢（CPU 取数据太慢 / 中断占用太久）
 *    64 = ADDR_MISALIGNED    地址未对齐
 *    13 = COM_CRC_FAILED     卡回了“上一条命令 CRC 错”(0x1000)
 *    14 = INVALID_VOLTRANGE  电压范围不对(0x01000000)
 *    15 = TIMEOUT            HAL 层整体超时(0x80000000)，等卡回传输态失败也用它
 *    12 = INVALID_PARAMETER  11 = ADDR_OUT_OF_RANGE  10 = REQUEST_NOT_APPLICABLE
 *     9 = UNSUPPORTED_FEATURE  17 = BUSY            90 = 其它（再看 SD_GetReadSta()）
 *  注意：0x100 以上的错误码不能只看低 8 位（低 8 位会是 0，看起来像“没有错误”）。*/
static uint8_t SD_ErrToCode(uint32_t e)
{
  if (e == 0U)                 { return 0U;  }
  if ((e & 0x00000001U) != 0U) { return 1U;  }   /* CMD_CRC_FAIL      */
  if ((e & 0x00000002U) != 0U) { return 2U;  }   /* DATA_CRC_FAIL     */
  if ((e & 0x00000004U) != 0U) { return 4U;  }   /* CMD_RSP_TIMEOUT   */
  if ((e & 0x00000008U) != 0U) { return 8U;  }   /* DATA_TIMEOUT      */
  if ((e & 0x00000010U) != 0U) { return 16U; }   /* TX_UNDERRUN       */
  if ((e & 0x00000020U) != 0U) { return 32U; }   /* RX_OVERRUN        */
  if ((e & 0x00000040U) != 0U) { return 64U; }   /* ADDR_MISALIGNED   */
  if ((e & 0x00001000U) != 0U) { return 13U; }   /* COM_CRC_FAILED    */
  if ((e & 0x01000000U) != 0U) { return 14U; }   /* INVALID_VOLTRANGE */
  if ((e & 0x02000000U) != 0U) { return 11U; }   /* ADDR_OUT_OF_RANGE */
  if ((e & 0x04000000U) != 0U) { return 10U; }   /* REQUEST_NOT_APPLI */
  if ((e & 0x08000000U) != 0U) { return 12U; }   /* INVALID_PARAMETER */
  if ((e & 0x10000000U) != 0U) { return 9U;  }   /* UNSUPPORTED_FEAT  */
  if ((e & 0x20000000U) != 0U) { return 17U; }   /* BUSY              */
  if ((e & 0x80000000U) != 0U) { return 15U; }   /* TIMEOUT           */
  return 90U;
}

static uint8_t  s_fallEdge = 0;        /* 0 = 上升沿采样(标准), 1 = 下降沿采样(试验) */
static uint8_t  s_rdStep = 0;          /* 最近一次读写失败在第几步: 1=命令 2=数据 3=等状态 */
static uint8_t  s_rdErr  = 0;          /* 最近一次读写失败 HAL 的真实错误码 */
static uint32_t s_rdSta  = 0;          /* 读失败时 SDMMC1->STA 的值 */

/**
  * @brief  SDMMC1 底层初始化（HAL_SD_Init() 内部回调：时钟 + 引脚）
  */
void HAL_SD_MspInit(SD_HandleTypeDef *hsd)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  if (hsd->Instance != SDMMC1)
  {
    return;
  }

  __HAL_RCC_SDMMC1_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();

  /* SDMMC1 内核时钟源选 PLL1Q */
  __HAL_RCC_SDMMC_CONFIG(RCC_SDMMCCLKSOURCE_PLL);

  /* PC8/PC9/PC10/PC11/PC12 -> SDMMC1  D0/D1/D2/D3/CK */
  GPIO_InitStruct.Pin       = GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10 |
                              GPIO_PIN_11 | GPIO_PIN_12;
  GPIO_InitStruct.Mode      = GPIO_MODE_AF_PP;
  /* 2026-09-25：NOPULL 改成 PULLUP。
     底板上 CMD/DAT 本来就有 10K 外部上拉，再叠一个 MCU 内部弱上拉（约 40K）
     只会让"插卡被识别"更稳（认卡就是靠 CMD 线上的电平），没有副作用。 */
  GPIO_InitStruct.Pull      = GPIO_PULLUP;
  GPIO_InitStruct.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
  GPIO_InitStruct.Alternate = GPIO_AF12_SDMMC1;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  /* PD2 -> SDMMC1 CMD */
  GPIO_InitStruct.Pin = GPIO_PIN_2;
  HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);

  /* PC2 = 板载 WIFI 模块（AP6181）的 WL_REG_ON。TF 卡座和 WIFI 模块共用同一组
     SDIO 线（原理图原文：“与TF卡共用SDIO接口”），只要 WIFI 芯片在总线上加载/驱动，
     命令还能通、数据一定 CRC 错（屏幕 202）。这里把它拉低，让 WIFI 芯片一直处于
     复位状态（引脚高阻），PC2 在本工程没有别的用途。*/
  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_2, GPIO_PIN_RESET);
  GPIO_InitStruct.Pin   = GPIO_PIN_2;
  GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull  = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);
}

/**
  * @brief  SDMMC1 底层反初始化
  */
void HAL_SD_MspDeInit(SD_HandleTypeDef *hsd)
{
  if (hsd->Instance != SDMMC1)
  {
    return;
  }

  __HAL_RCC_SDMMC1_CLK_DISABLE();

  HAL_GPIO_DeInit(GPIOC, GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10 |
                         GPIO_PIN_11 | GPIO_PIN_12);
  HAL_GPIO_DeInit(GPIOD, GPIO_PIN_2);
}

/**
  * @brief  初始化 SD 卡
  * @retval 0 = 成功，1 = 失败
  *
  *   流程（2026-09-25 改）：
  *     1) 先用 **1 线模式**做初始化：认卡（CMD0/CMD8/ACMD41）和后续的总线切换
  *        在 1 线下最简单，只要 CMD/CLK 两根线通就行；
  *     2) 认到卡以后再试着升到 **4 线模式**（HAL_SD_ConfigWideBusOperation）；
  *        升不上去就保持 1 线 —— 1 线 25MHz 也有约 3MB/s，本工程 10Hz 的数据量
  *        （每秒几十字节）完全够用。
  *
  *   为什么这么改：原来是直接按 4 线模式 HAL_SD_Init()，只要 4 线切换那一步
  *   失败（DAT2/DAT3 接触不良、线长、卡座虚焊、卡不支持等），HAL_SD_Init 就整体
  *   返回错误，屏幕上看就是"SD 认不到卡"，而实际上卡是认出来的。
  */
uint8_t SD_Init(void)
{
  HAL_StatusTypeDef st;
  HAL_SD_CardStatusTypeDef cardStatus;
  HAL_SD_CardInfoTypeDef   cardInfo;
  uint32_t tickstart;

  s_sdReady   = 0;
  s_sdBusMode = 0;
  s_failStep  = 0;
  s_failErr   = 0;
  s_rdStep = 0;
  s_rdErr  = 0;
  s_rdSta  = 0;
  s_failCntStatus = 0;

  /* ---- MPU / Cache 统一在开机时配一次（见 Src/main.c 的 LCD_DisplayInit）----
   *
   *   以前是“每次 SD_Init 都把 AXI SRAM 头 32KB 改成 non-cacheable”，有两个问题：
   *     1) SD_Init() 每次重试（每 2 秒）都会 HAL_MPU_Disable()/HAL_MPU_Enable() 一次，
   *        运行中反复改 MPU 是不安全的做法；
   *     2) 按本工程的散列文件，全部 RW/ZI 数据（连 1KB 栈）都落在 AXI SRAM 头部，
   *        那块区域被改成 non-cacheable 后，整个程序的数据都不走 Cache 了 ——
   *        副作用远大于“只影响 SD 缓冲区”。
   *   现在改成：开机时一次性把整块 AXI SRAM（0x24000000，512KB）配成 non-cacheable，
   *   SD_Init() 里不再碰 MPU，也不用做 cache 维护。*/

  hsd1.Instance                 = SDMMC1;
  hsd1.Init.ClockEdge           = SDMMC_CLOCK_EDGE_RISING;
  hsd1.Init.ClockPowerSave      = SDMMC_CLOCK_POWER_SAVE_DISABLE;
  hsd1.Init.BusWide             = SDMMC_BUS_WIDE_1B;
  hsd1.Init.HardwareFlowControl = SDMMC_HARDWARE_FLOW_CONTROL_DISABLE;
  hsd1.Init.ClockDiv            = SD_SDMMC_CLKDIV;

  /* 自己做 MspInit（时钟 + 引脚），因为下面已经不调用 HAL_SD_Init() 了 */
  hsd1.State = HAL_SD_STATE_RESET;
  HAL_SD_MspInit(&hsd1);
  hsd1.State = HAL_SD_STATE_PROGRAMMING;

  /* ---- 第 1 步：认卡（CMD0 / CMD8 / ACMD41，400kHz、1 线）----
     只有这一步失败才是真正的“认不到卡”。
     注意：错误码必须在 HAL_SD_DeInit() 之前抓下来，
     因为 HAL_SD_DeInit() 会把 hsd1.ErrorCode 清零（这就是之前一直显示 0 的原因）。 */
  st = HAL_SD_InitCard(&hsd1);
  if (st != HAL_OK)
  {
    s_failStep = 1U;
    s_failErr  = SD_ErrToCode(hsd1.ErrorCode);
    (void)HAL_SD_DeInit(&hsd1);
    return 1;
  }

  /* ---- 第 2 步：读 SD status（只为拿速度等级；有些卡不支持，失败就重来一次、不影响使用）---- */
  if (HAL_SD_GetCardStatus(&hsd1, &cardStatus) != HAL_OK)
  {
    (void)HAL_SD_DeInit(&hsd1);
    hsd1.State = HAL_SD_STATE_RESET;
    HAL_SD_MspInit(&hsd1);
    hsd1.State = HAL_SD_STATE_PROGRAMMING;
    if (HAL_SD_InitCard(&hsd1) != HAL_OK)
    {
      s_failStep = 1U;
      s_failErr  = SD_ErrToCode(hsd1.ErrorCode);
      (void)HAL_SD_DeInit(&hsd1);
      return 1;
    }
    s_failCntStatus = 1U;
  }

  /* ---- 第 3 步：先按 1 线工作（最稳），再试着升 4 线（升不上去就退回 1 线）----
   *
   *   HAL_SD_ConfigWideBusOperation() 内部会自己发 CMD55 + ACMD6（先读一次卡的
   *   SCR，再设总线宽度），所以这里不用（也不该）再手动发一遍：手动发一遍等于
   *   多一条可能失败的路径，失败了卡还可能停在“卡 4 线 / 主机 1 线”的错位上。
   *   它同时把 SDMMC 的时钟从认卡的 ~400kHz 换成 SD_SDMMC_CLKDIV 算出的转移
   *   时钟 —— 只跑 1 线时也要调用一次，否则读写会一直停在 400kHz。*/
  s_sdBusMode = 1U;                  /* 默认就用 1 线 */
#if (SD_USE_4BIT == 1U)
  if (HAL_SD_ConfigWideBusOperation(&hsd1, SDMMC_BUS_WIDE_4B) == HAL_OK)
  {
    s_sdBusMode = 4U;
  }
  else
  {
    (void)HAL_SD_ConfigWideBusOperation(&hsd1, SDMMC_BUS_WIDE_1B);   /* 4 线没成，退回 1 线 */
  }
#else
  (void)HAL_SD_ConfigWideBusOperation(&hsd1, SDMMC_BUS_WIDE_1B);
#endif

  /* ---- 第 4 步：等卡回到传输态（超时也先用着）---- */
  tickstart = HAL_GetTick();
  while (HAL_SD_GetCardState(&hsd1) != HAL_SD_CARD_TRANSFER)
  {
    if ((HAL_GetTick() - tickstart) >= 1000U)
    {
      break;
    }
  }

  /* ---- 第 5 步：读 CSD（容量等信息，顺带填上 SdCard.LogBlockNbr）---- */
  if (HAL_SD_GetCardInfo(&hsd1, &cardInfo) != HAL_OK)
  {
    s_failStep = 5U;
    s_failErr  = SD_ErrToCode(hsd1.ErrorCode);
  }

  hsd1.State = HAL_SD_STATE_READY;
  s_sdReady  = 1U;
  return 0;
}

/**
  * @brief  反初始化 SD 卡
  */
void SD_DeInit(void)
{
  s_sdReady = 0;
  s_sdBusMode = 0;
  (void)HAL_SD_DeInit(&hsd1);
}

uint8_t SD_IsReady(void)
{
  return s_sdReady;
}

/**
  * @brief  当前用的是几线模式（1 或 4），0 = 没初始化成功
  */
uint8_t SD_GetBusMode(void)
{
  return s_sdBusMode;
}

/**
  * @brief  最近一次认卡失败在哪一步（0 = 没失败，1 = 认卡本身失败）
  */
uint8_t SD_GetFailStep(void)
{
  return s_failStep;
}

/**
  * @brief  最近一次认卡失败的真实 HAL 错误码
  *         3 = 命令响应超时（卡没应答）；1/2 = CRC 错（信号/接触问题）；4 = 数据超时
  */
uint8_t SD_GetFailErr(void)
{
  return s_failErr;
}

/**
  * @brief  卡是否不支持读 SD status（1 = 不支持，只影响速度等级判断，不影响读写）
  */
uint8_t SD_GetNoCardStatus(void)
{
  return s_failCntStatus;
}

/**
  * @brief  最近一次读/写失败在第几步：1=命令阶段 2=数据阶段 3=等卡回传输态
  */
uint8_t SD_GetReadStep(void)
{
  return s_rdStep;
}

/**
  * @brief  最近一次读/写失败时 HAL 的真实错误码（关键诊断）
  *         1=命令CRC 2=数据CRC 3=命令无响应 4=数据超时 0x10=接收溢出 0x20=地址越界
  */
uint8_t SD_GetReadErr(void)
{
  return s_rdErr;
}

/**
  * @brief  最近一次读/写失败时 SDMMC1->STA 寄存器的值
  */
uint32_t SD_GetReadSta(void)
{
  return s_rdSta;
}

/**
  * @brief 当前采样沿：0 = 上升沿(标准)，1 = 下降沿(试验)
  */
uint8_t SD_GetEdge(void)
{
  return s_fallEdge;
}

uint32_t SD_GetSectorCount(void)
{
  return (uint32_t)hsd1.SdCard.LogBlockNbr;
}

/**
  * @brief  读扇区
  * @param  buf    数据缓冲区（512 字节 * count）
  * @param  sector 起始扇区（1 扇区 = 512 字节）
  * @param  count  扇区数
  */
HAL_StatusTypeDef SD_ReadSectors(uint8_t *buf, uint32_t sector, uint32_t count)
{
  uint32_t tickstart;
  uint8_t  attempt;

  if (s_sdReady == 0)
  {
    return HAL_ERROR;
  }

  for (attempt = 0U; attempt < SD_OP_RETRY; attempt++)
  {
    if (HAL_SD_ReadBlocks(&hsd1, buf, sector, count, SD_OP_TIMEOUT) == HAL_OK)
    {
      break;
    }

    /* 把失败原因记下来：HAL 的错误码才是关键（原来都被丢掉了）*/
    s_rdErr = SD_ErrToCode(hsd1.ErrorCode);
    s_rdSta = SDMMC1->STA;
    if (hsd1.ErrorCode == HAL_SD_ERROR_ADDR_OUT_OF_RANGE)
    {
      s_rdStep = 1U;              /* 地址范围检查就没过 */
    }
    else if (hsd1.ErrorCode == HAL_SD_ERROR_CMD_RSP_TIMEOUT)
    {
      s_rdStep = 1U;              /* 读命令没响应 */
    }
    else
    {
      s_rdStep = 2U;              /* 数据阶段（超时/CRC/溢出）*/
    }

    HAL_Delay(2U);                /* 缓一下再重试 */
  }

  if (attempt >= SD_OP_RETRY)
  {
    return HAL_ERROR;
  }

  /* 等卡回到传输态（卡中途被拔掉时这里会超时退出，不会死等） */
  tickstart = HAL_GetTick();
  while (HAL_SD_GetCardState(&hsd1) != HAL_SD_CARD_TRANSFER)
  {
    if ((HAL_GetTick() - tickstart) > SD_STATE_TIMEOUT)
    {
      s_rdStep = 3U;
      s_rdErr  = SD_ErrToCode(hsd1.ErrorCode);
      s_rdSta  = SDMMC1->STA;
      return HAL_TIMEOUT;
    }
  }

  return HAL_OK;
}

/**
  * @brief  写扇区
  */
HAL_StatusTypeDef SD_WriteSectors(const uint8_t *buf, uint32_t sector, uint32_t count)
{
  uint32_t tickstart;

  if (s_sdReady == 0)
  {
    return HAL_ERROR;
  }

  if (HAL_SD_WriteBlocks(&hsd1, (uint8_t *)buf, sector, count, SD_OP_TIMEOUT) != HAL_OK)
  {
    return HAL_ERROR;
  }

  tickstart = HAL_GetTick();
  while (HAL_SD_GetCardState(&hsd1) != HAL_SD_CARD_TRANSFER)
  {
    if ((HAL_GetTick() - tickstart) > SD_STATE_TIMEOUT)
    {
      s_rdStep = 3U;
      s_rdErr  = SD_ErrToCode(hsd1.ErrorCode);
      s_rdSta  = SDMMC1->STA;
      return HAL_TIMEOUT;
    }
  }

  return HAL_OK;
}

/**
  * @brief  裸读 0 号扇区（引导扇区）自检：绕开 FatFs，直接看 SD 驱动 + 硬件能不能读
  * @param  out4 输出：扇区头 4 个字节（可为 0）
  * @retval 0 = 读成功；1 = 读失败（原因看 SD_GetReadStep() / SD_GetReadErr()）
  *
  *   用途：把“SD 驱动/硬件读不出来”和“文件系统不对”分开：
  *     FAT16/32 的引导扇区以 EB 3C 90 开头（也有 E9 xx xx），exFAT 是 EB 76 90，
  *     NTFS 是 EB 52 90；如果这一步就读失败，先别去看文件系统。
  */
uint8_t SD_ProbeRawSector0(uint8_t *out4)
{
  uint8_t  i;
  uint8_t  fail;
  HAL_StatusTypeDef st;

  /* 先把缓冲区填成 0xA5 记号：如果这次连数据阶段都没进去（命令阶段就失败），
     屏幕上会看到 A5A5A5，就能和“收到全 FF / 全 00 / 其它数据”区分开 */
  for (i = 0U; i < 4U; i++)
  {
    s_probeBuf[i] = 0xA5U;
  }

  st   = SD_ReadSectors(s_probeBuf, 0U, 1U);
  fail = (st == HAL_OK) ? 0U : 1U;

  /* 不管成功还是失败，都把“收到的前 4 个字节”回填出去（失败时这几个字节最能
     说明问题，LCD 上会显示前 3 个）：
       FF FF FF = DAT0 一直被拉高 —— 线断 / 卡座接触不良 / 被别的器件拉载
       00 00 00 = 一直被拉低
       A5 A5 A5 = 这次根本没进到数据阶段（命令阶段就失败了）
       其它     = 收到数据了，但内容或 CRC 不对（信号质量 / 位对齐问题）*/
  if (out4 != 0)
  {
    for (i = 0U; i < 4U; i++)
    {
      out4[i] = s_probeBuf[i];
    }
  }

  return fail;
}


/**
  * @brief  手动切换 SD 总线宽度（1 线 / 4 线）—— 绕开 HAL 的 SCR 读
  * @param  fourBit 1 = 切到 4 线，0 = 切回 1 线
  *
  *   为什么不直接用 HAL_SD_ConfigWideBusOperation()：
  *     它内部第一步是读卡的 SCR（一次 8 字节的数据读）。数据读一旦失败（比如屏幕
  *     上的 202 = 数据 CRC 错），它就直接返回错误，后面的 CMD55 + ACMD6 根本不会
  *     发出去 —— 于是“卡停在 4 线、主机还是 1 线”，数据永远 CRC 错，怎么重试都没用。
  *     这里直接发 CMD55 + ACMD6 并改 CLKCR.WIDBUS，绕开 SCR 这一步。
  *   （ACMD6 必须在还处于 1 线时发出去，所以顺序是：先 ACMD6，再改 CLKCR。）
  */
void SD_TryBusWide(uint8_t fourBit)
{
  uint32_t rca = ((uint32_t)hsd1.SdCard.RelCardAdd) << 16U;

  (void)SDMMC_CmdAppCommand(hsd1.Instance, rca);                       /* CMD55 */
  (void)SDMMC_CmdBusWidth(hsd1.Instance, (fourBit != 0U) ? 2U : 0U);   /* ACMD6 */

  if (fourBit != 0U)
  {
    MODIFY_REG(hsd1.Instance->CLKCR, SDMMC_CLKCR_WIDBUS, SDMMC_CLKCR_WIDBUS_0);  /* 01 = 4 线 */
    s_sdBusMode = 4U;
  }
  else
  {
    CLEAR_BIT(hsd1.Instance->CLKCR, SDMMC_CLKCR_WIDBUS);                         /* 00 = 1 线 */
    s_sdBusMode = 1U;
  }
}

/************************ (C) COPYRIGHT *****END OF FILE************************/
