/**
  ******************************************************************************
  * @file    imu_sdlog.c
  * @brief   IMU 数据记录到 SD 卡（FatFs + SDMMC1）
  ******************************************************************************
  */

#include "./sd/imu_sdlog.h"
#include "./sd/bsp_sd.h"
#include "ff.h"

#include <string.h>

/* ---------------- 可调参数 ---------------- */
#define IMULOG_SYNC_LINES   200U      /* 每写 20 行 f_sync 一次（20Hz 时约 1 秒）*/
#define IMULOG_SYNC_MS      2000U    /* 或最多 2 秒落盘一次，掉电最多丢 2 秒 */
#define IMULOG_RETRY_MS     2000U    /* 没卡 / 出错时，每 2 秒重试一次 */
#define IMULOG_MAX_INDEX    9999U    /* 文件名序号上限：IMU0001 ~ IMU9999 */
#define IMULOG_LINE_MAX     160U     /* 一行最长多少个字节 */

/* ---------------- 内部变量 ----------------
 *
 *  为什么这三块缓冲区要放 .sd_dma 段（AXI SRAM）：
 *    * 本工程的 SD 读写用的是 HAL_SD_ReadBlocks()/HAL_SD_WriteBlocks() 的
 *      **轮询方式**（CPU 自己读写 SDMMC FIFO，不走 SDMMC 内部 IDMA）；
 *    * 但本工程同时还跑 USB Host，它和 SDMMC 都会自己搬数据，而 AXI SRAM 里
 *      放着程序全部全局变量和栈 —— 所以 Src/main.c 在开 D-Cache 之前，把整块
 *      AXI SRAM 配成 non-cacheable，让 CPU 和外设永远看到同一份数据；
 *    * FATFS（含 512 字节读写窗口 win[]）、FIL（含 buf[]）和写文件的行缓冲
 *      s_line 都放这一段里，保证缓冲区 / 卡驱动 / 文件系统三者一致。
 *   （2026-10-05 更正：以前这里写"IDMA 访问不到 DTCM 所以读卡失败"是错的。
 *    当时屏幕上的 202 = 数据 CRC 错，根因是数据线（DAT0~DAT3）的电气问题，
 *    跟缓冲区放哪、走不走 DMA 无关 —— 详细过程见 README_SD卡记录说明.md 第 8 节。）
 */
#define SD_DMA_SECTION   __attribute__((section(".sd_dma")))

SD_DMA_SECTION static FATFS  s_fs;
SD_DMA_SECTION static FIL    s_file;
static char   s_name[16];
SD_DMA_SECTION static char   s_line[IMULOG_LINE_MAX];

static uint8_t  s_state     = IMULOG_STATE_IDLE;
static uint8_t  s_mounted   = 0;
static uint8_t  s_opened    = 0;
static uint8_t  s_syncLines = 0;
static uint32_t s_count     = 0;
static uint32_t s_lastSync  = 0;
static uint32_t s_lastTry   = 0;

/* 失败诊断：卡在哪一步 + 对应的错误码（给 LCD 显示用）*/
static uint8_t  s_failStage = 0;
static uint8_t  s_failCode  = 0;
static FRESULT  s_openFr    = FR_OK;
static uint8_t  s_rawOk     = 0;         /* 1 = s_raw 有效（裸读成功/失败都记下前 4 字节）*/
static uint8_t  s_raw[4]    = {0, 0, 0, 0};  /* 裸读到的扇区头 4 个字节（给 LCD 看）*/
static uint8_t  s_bus1Code  = 0;         /* 1 线裸读失败码（0 = 没失败）*/
static uint8_t  s_bus4Code  = 0;         /* 4 线裸读失败码（0 = 没失败 / 没试）*/
static uint8_t  s_stable    = 0;          /* 1 = 两次裸读收到的字节一样（稳定）*/

/* ---------------- 字符串拼装（不用 printf，microlib 下也稳当） ---------------- */

static char *put_u32(char *p, uint32_t v)
{
  char    tmp[10];
  uint8_t n = 0;

  if (v == 0U)
  {
    *p++ = '0';
    return p;
  }

  while (v > 0U)
  {
    tmp[n++] = (char)('0' + (v % 10U));
    v /= 10U;
  }
  while (n > 0U)
  {
    *p++ = tmp[--n];
  }
  return p;
}

/* 有符号整数 -> 字符串（编码器计数用，可能为负）*/
static char *put_i32(char *p, int32_t v)
{
  uint32_t a;
  if (v < 0)
  {
    *p++ = '-';
    a = (uint32_t)(-v);
  }
  else
  {
    a = (uint32_t)v;
  }
  return put_u32(p, a);
}

/* 定点数：把 float 按小数点后 digits 位（1~4）写成字符串 */
static char *put_fixed(char *p, float v, uint8_t digits)
{
  static const uint32_t scaleTab[5] = {1U, 10U, 100U, 1000U, 10000U};
  uint32_t scale;
  uint32_t frac;
  uint32_t div;
  int32_t  t;

  if (digits > 4U)
  {
    digits = 4U;
  }
  scale = scaleTab[digits];

  t = (int32_t)((v >= 0.0f) ? (v * (float)scale + 0.5f)
                            : (v * (float)scale - 0.5f));
  if (t < 0)
  {
    *p++ = '-';
    frac = (uint32_t)(-t);
  }
  else
  {
    frac = (uint32_t)t;
  }

  p    = put_u32(p, frac / scale);
  frac = frac % scale;

  if (digits > 0U)
  {
    *p++ = '.';
    div  = scale / 10U;
    while (div > 0U)
    {
      *p++ = (char)('0' + ((frac / div) % 10U));
      div /= 10U;
    }
  }
  return p;
}

/* ---------------- 文件名 / 开关文件 ---------------- */

/* 生成 "IMU0001.CSV"（8.3 短名，不需要长文件名支持） */
static void MakeFileName(uint16_t idx)
{
  s_name[0]  = 'I';
  s_name[1]  = 'M';
  s_name[2]  = 'U';
  s_name[3]  = (char)('0' + ((idx / 1000U) % 10U));
  s_name[4]  = (char)('0' + ((idx / 100U) % 10U));
  s_name[5]  = (char)('0' + ((idx / 10U) % 10U));
  s_name[6]  = (char)('0' + (idx % 10U));
  s_name[7]  = '.';
  s_name[8]  = 'C';
  s_name[9]  = 'S';
  s_name[10] = 'V';
  s_name[11] = '\0';
}

/**
  * @brief  在根目录找一个没用过的文件名，新建并写表头
  * @retval 1 = 成功，0 = 失败
  */
static uint8_t OpenNewFile(void)
{
  static const char hdr[] = "t_ms,roll,pitch,yaw,ax,ay,az,gx,gy,gz,enc_deg,rpm,z_cnt,force_g\r\n";
  const UINT hdrLen = (UINT)(sizeof(hdr) - 1U);
  FILINFO fno;
  FRESULT fr;
  UINT    bw;
  uint16_t i;

  for (i = 1U; i <= IMULOG_MAX_INDEX; i++)
  {
    MakeFileName(i);

    fr = f_stat(s_name, &fno);
    if (fr == FR_NO_FILE)
    {
      fr = f_open(&s_file, s_name, FA_CREATE_NEW | FA_WRITE);
      if (fr != FR_OK)
      {
        s_openFr = fr;
        s_name[0] = '\0';
        return 0;
      }
      s_opened = 1;

      if ((f_write(&s_file, hdr, hdrLen, &bw) != FR_OK) ||
          (bw != hdrLen) ||
          (f_sync(&s_file) != FR_OK))
      {
        s_openFr = FR_DISK_ERR;          /* 表头都写不进去：算磁盘错 */
        (void)f_close(&s_file);
        s_opened  = 0;
        s_name[0] = '\0';
        return 0;
      }
      return 1;
    }

    if (fr != FR_OK)
    {
      s_openFr = fr;                     /* f_stat 都失败了 */
      break;            /* 卡出问题了，不要在这里死循环 */
    }
  }

  s_name[0] = '\0';
  return 0;
}

/* ---- “开始存储”时调用：把当前文件收好，再新建下一个 IMUnnnn.CSV ---- */
uint8_t IMULog_NewFile(void)
{
  if (s_opened != 0)
  {
    (void)f_sync(&s_file);           /* 先把上一段刷到卡上 */
    (void)f_close(&s_file);
    s_opened  = 0;
    s_name[0] = '\0';
  }

  if (OpenNewFile() == 0U)           /* 建不出来（没卡/写保护/已满）*/
  {
    s_failStage = 3U;
    s_failCode  = (uint8_t)s_openFr;
    s_state     = IMULOG_STATE_ERROR;
    return 0U;
  }

  s_count     = 0U;                  /* 新文件：帧数从 0 算 */
  s_syncLines = 0U;
  s_state     = IMULOG_STATE_LOGGING;
  return 1U;
}

/* 关文件 + 卸载，出错重试前用 */
static void CloseFile(void)
{
  if (s_opened != 0)
  {
    (void)f_close(&s_file);
    s_opened = 0;
  }
  if (s_mounted != 0)
  {
    (void)f_mount(NULL, "0:", 0);
    s_mounted = 0;
  }
  s_name[0] = '\0';
}

/* 把 FatFs 缓存里的数据真正写进卡 */
static void SyncNow(void)
{
  s_syncLines = 0;
  s_lastSync  = HAL_GetTick();

  if (s_opened != 0)
  {
    if (f_sync(&s_file) != FR_OK)
    {
      s_state = IMULOG_STATE_ERROR;
    }
  }
}

/* ---------------- 对外接口 ---------------- */

uint8_t IMULog_Init(void)
{
  FRESULT fr;

  s_count     = 0;
  s_syncLines = 0;
  s_lastSync  = HAL_GetTick();
  s_lastTry   = HAL_GetTick();
  s_name[0]   = '\0';
  s_state     = IMULOG_STATE_IDLE;
  s_opened    = 0;
  s_failStage = 0;
  s_failCode  = 0;
  s_openFr    = FR_OK;
  s_rawOk     = 0U;
  s_raw[0]    = 0U;
  s_raw[1]    = 0U;
  s_raw[2]    = 0U;
  s_raw[3]    = 0U;
  s_bus1Code  = 0U;
  s_bus4Code  = 0U;
  s_stable    = 0U;

  if (SD_Init() != 0U)
  {
    /* 第 1 步就失败：卡没认到（没插卡 / 接触不良 / 卡坏 / 没供电）
       错误码要从 SD_GetFailErr() 拿：它在 HAL_SD_DeInit() 清 ErrorCode 之前就存好了 */
    s_failStage = 1U;
    s_failCode  = SD_GetFailErr();
    return 0;
  }

  /* ---- 裸读自检 + 总线宽度自愈（1 线 / 4 线 自动互切）----
   *
   *   1) 先用当前总线宽度（1 线）裸读 0 号扇区（绕开 FatFs）；
   *   2) 读失败就手动切到 4 线再读一次 —— 老固件曾经把卡切到 4 线，而 HAL 里
   *      “把卡切回 1 线”这一步要先读 SCR（一次数据读），数据读一失败 ACMD6 就发不
   *      出去，卡就永远停在 4 线、主机还是 1 线 → 数据必然 CRC 错（屏幕 202）。
   *      所以这里用 SD_TryBusWide() 绕开 SCR 手动切；
   *   3) 哪种能读就用哪种（4 线能读就继续用 4 线）；两种都不行才报错，并把两次的
   *      错误码 + 收到的前 4 个字节留给 LCD 显示。*/
  s_rawOk = 0U;
  {
    uint8_t probeFail = SD_ProbeRawSector0(s_raw);       /* 1 线（当前总线宽度）*/

    if (probeFail != 0U)
    {
      uint8_t step = SD_GetReadStep();
      uint8_t raw4[4];

      if (step == 0U) { step = 2U; }
      s_bus1Code = (uint8_t)((uint16_t)step * 100U + (uint16_t)SD_GetReadErr());

      SD_TryBusWide(1U);                                 /* 手动切 4 线再试 */
      probeFail = SD_ProbeRawSector0(raw4);

      if (probeFail == 0U)
      {
        s_raw[0] = raw4[0];                              /* 4 线能读 → 就继续用 4 线 */
        s_raw[1] = raw4[1];
        s_raw[2] = raw4[2];
        s_raw[3] = raw4[3];
        s_bus4Code = 0U;
      }
      else
      {
        step = SD_GetReadStep();
        if (step == 0U) { step = 2U; }
        s_bus4Code = (uint8_t)((uint16_t)step * 100U + (uint16_t)SD_GetReadErr());
        SD_TryBusWide(0U);                               /* 都不行：切回 1 线 */

        /* 再读一次，比较两次收到的前 4 字节：
           一样（稳定）→ 系统性错位（卡/总线状态问题）
           不一样      → 接触不良 / 干扰这类模拟问题 */
        {
          uint8_t raw2[4];

          if ((SD_ProbeRawSector0(raw2) == 0U) ||
              (raw2[0] != s_raw[0]) || (raw2[1] != s_raw[1]) || (raw2[2] != s_raw[2]))
          {
            s_stable = 0U;
          }
          else
          {
            s_stable = 1U;
          }
        }
        s_rawOk = 1U;                                    /* 失败时也把 1 线的字节留给 LCD */

        s_failStage = 2U;
        s_failCode  = s_bus1Code;
        return 0;
      }
    }
  }
  s_rawOk = 1U;                                          /* s_raw 有效（给 LCD 显示扇区头）*/

  /* opt = 1：立即挂载，顺便就能判断卡里是不是 FAT 文件系统 */
  fr = f_mount(&s_fs, "0:", 1);
  if (fr != FR_OK)
  {
    /* 第 2 步失败：卡认到了，但挂不上（最常见是卡不是 FAT/FAT32）*/
    s_failStage = 2U;
    /* FR_DISK_ERR(1) = 读扇区失败：用“百位(步骤) + 后两位(HAL 错误码)”表示，
       例：202 = 数据阶段(2) + 数据 CRC 错(02)，204 = 数据阶段 + 命令响应超时(04)，
           104 = 命令阶段 + 命令响应超时。1xx/2xx/3xx 都是驱动/硬件层的问题。*/
    if ((fr == FR_DISK_ERR) && (SD_GetReadErr() != 0U))
    {
      {
        uint8_t step = SD_GetReadStep();
        if (step == 0U) { step = 2U; }
        s_failCode = (uint8_t)((uint16_t)step * 100U + (uint16_t)SD_GetReadErr());
      }
    }
    else
    {
      s_failCode = (uint8_t)fr;
    }
    s_mounted = 0;
    return 0;
  }
  s_mounted = 1;

  if (OpenNewFile() == 0U)
  {
    /* 第 3 步失败：文件系统没问题，但建文件/写表头不成功 */
    s_failStage = 3U;
    s_failCode  = (uint8_t)s_openFr;
    return 0;
  }

  s_state = IMULOG_STATE_LOGGING;
  return 1;
}

void IMULog_WriteSample(const IMULogSampleTypeDef *s)
{
  char *p = s_line;
  UINT  len;
  UINT  bw;

  if ((s == NULL) || (s_state != IMULOG_STATE_LOGGING) || (s_opened == 0))
  {
    return;
  }

  /* 顺序：t_ms, roll, pitch, yaw, ax, ay, az, gx, gy, gz, temp */
  p = put_u32(p, s->t_ms);        *p++ = ',';
  p = put_fixed(p, s->roll, 3U);  *p++ = ',';
  p = put_fixed(p, s->pitch, 3U); *p++ = ',';
  p = put_fixed(p, s->yaw, 3U);   *p++ = ',';
  p = put_fixed(p, s->ax, 4U);    *p++ = ',';
  p = put_fixed(p, s->ay, 4U);    *p++ = ',';
  p = put_fixed(p, s->az, 4U);    *p++ = ',';
  p = put_fixed(p, s->gx, 2U);    *p++ = ',';
  p = put_fixed(p, s->gy, 2U);    *p++ = ',';
  p = put_fixed(p, s->gz, 2U);    *p++ = ',';
  p = put_fixed(p, s->enc_deg, 2U);  *p++ = ',';   /* 编码器角度（度）*/
  p = put_fixed(p, s->rpm, 1U);      *p++ = ',';   /* 转速（转/分）*/
  p = put_i32(p, s->z_cnt);         /* Z 脉冲数 */
  *p++ = ',';
  p = put_i32(p, s->force);        /* 力传感器原始 ADC 值 */
  *p++ = '\r';
  *p++ = '\n';
  *p   = '\0';

  len = (UINT)(p - s_line);

  if ((f_write(&s_file, s_line, len, &bw) != FR_OK) || (bw != len))
  {
    s_state = IMULOG_STATE_ERROR;      /* 写不进去（掉卡 / 卡写保护） */
    return;
  }

  s_count++;

  if (++s_syncLines >= IMULOG_SYNC_LINES)
  {
    SyncNow();
  }
}

void IMULog_Task(void)
{
  uint32_t now = HAL_GetTick();

  if (s_state == IMULOG_STATE_LOGGING)
  {
    /* 缓存里还有数据，且距上次落盘超过 1 秒 -> 落盘 */
    if ((s_syncLines != 0U) && ((now - s_lastSync) >= IMULOG_SYNC_MS))
    {
      SyncNow();
    }
    return;
  }

  /* 没卡 / 写失败：每 2 秒重试一次（插上卡后会自动开始记录） */
  if ((now - s_lastTry) < IMULOG_RETRY_MS)
  {
    return;
  }
  s_lastTry = now;

  if (s_state == IMULOG_STATE_ERROR)
  {
    CloseFile();
  }

  (void)IMULog_Init();
}

uint8_t IMULog_IsReady(void)
{
  return (s_state == IMULOG_STATE_LOGGING) ? 1U : 0U;
}

uint8_t IMULog_GetState(void)
{
  return s_state;
}

uint32_t IMULog_GetCount(void)
{
  return s_count;
}

const char *IMULog_GetFileName(void)
{
  return s_name;
}

uint8_t IMULog_GetFailStage(void)
{
  return s_failStage;
}

uint8_t IMULog_GetFailCode(void)
{
  return s_failCode;
}

/**
  * @brief  认卡后裸读到的 0 号扇区头 4 个字节（绕开 FatFs 读的）
  * @param  out4 输出：4 个字节（可为 0）
  * @retval 1 = 有数据（读成功或失败都记了前 4 字节）；0 = 没有
  */
uint8_t IMULog_GetRawSector0(uint8_t *out4)
{
  uint8_t i;

  if (s_rawOk == 0U)
  {
    return 0U;
  }

  if (out4 != 0)
  {
    for (i = 0U; i < 4U; i++)
    {
      out4[i] = s_raw[i];
    }
  }
  return 1U;
}

/* 裸读自检里 1 线 / 4 线各自的失败码（0 = 没失败），给 LCD 显示用 */
uint8_t IMULog_GetBus1Code(void)
{
  return s_bus1Code;
}

uint8_t IMULog_GetBus4Code(void)
{
  return s_bus4Code;
}

/* 两次裸读收到的字节是否一致：1 = 一致（稳定），0 = 不一致（接触/干扰）*/
uint8_t IMULog_GetRawStable(void)
{
  return s_stable;
}

/************************ (C) COPYRIGHT *****END OF FILE************************/
