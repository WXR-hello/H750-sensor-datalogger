/**
  ******************************************************************************
  * @file    imu_sdlog.h
  * @brief   IMU 数据记录到 SD 卡（FatFs，CSV 文件）
  *
  *  用法（main.c 里）：
  *      IMULog_Init();                             // 挂载卡 + 建新文件 + 写表头
  *      IMULog_WriteSample(&sample);               // 每来一帧数据写一行
  *      IMULog_Task();                             // 主循环里周期调用：定期 f_sync / 掉卡重试
  *
  *  文件名：IMU0001.CSV、IMU0002.CSV ……（8.3 短名，不用长文件名，省 Flash）
  *  内容  ：t_ms,roll,pitch,yaw,ax,ay,az,gx,gy,gz,temp
  *          角度 0.001 度、加速度 0.0001 g、角速度 0.01 度/秒、温度 0.01 度
  ******************************************************************************
  */

#ifndef __IMU_SDLOG_H
#define __IMU_SDLOG_H

#include <stdint.h>

/* 记录状态 */
#define IMULOG_STATE_IDLE     0     /* 没在记录（没卡 / 没挂载上） */
#define IMULOG_STATE_LOGGING  1     /* 正在记录 */
#define IMULOG_STATE_ERROR    2     /* 出错停了（写失败），会自动重试 */

typedef struct
{
  uint32_t t_ms;              /* 上电后的毫秒数 */
  float    roll;              /* 横滚角 度 */
  float    pitch;             /* 俯仰角 度 */
  float    yaw;               /* 航向角 度 */
  float    ax, ay, az;        /* 加速度 g */
  float    gx, gy, gz;        /* 角速度 度/秒 */
  float    enc_deg;          /* 编码器角度（度）*/              /* 温度 度 */
  float    rpm;               /* 编码器转速（转/分）*/
  int32_t  z_cnt;             /* Z 脉冲数（编码器 Z 相计数）*/
  int32_t  force;             /* 力值（克）：已去皮、按 0~50kg 标定（PB0/ADC1_INP9）*/
} IMULogSampleTypeDef;

/* 挂载卡并在根目录建一个新文件：返回 1 = 成功，0 = 失败（没卡 / 卡不是 FAT） */
uint8_t IMULog_Init(void);
uint8_t IMULog_NewFile(void);   /* 新建下一个记录文件（开始存储时用）*/

/* 写一行数据（内部带缓存，每 10 行自动 f_sync 一次） */
void IMULog_WriteSample(const IMULogSampleTypeDef *s);

/* 主循环里周期调用（≥100ms 一次）：定期落盘、出错/掉卡自动重试 */
void IMULog_Task(void);

uint8_t     IMULog_IsReady(void);
uint8_t     IMULog_GetState(void);
uint32_t    IMULog_GetCount(void);        /* 已经记录的行数 */
const char *IMULog_GetFileName(void);     /* "IMU0001.CSV"，没建文件时是空串 */

/* ---- 记录失败时“卡在哪一步”（给 LCD 显示用，方便排查）----
 *
 *   IMULog_GetFailStage()：
 *     0 = 没失败（正在记录，或者还没试过）
 *     1 = SD_Init() 认卡这一步就失败   —— 没插好/接触不良/卡坏/没供电/总线被占（板载 WIFI）
 *     2 = 认卡成功但读扇区失败         —— 驱动/硬件读不出数据（见下面 code 的含义）
 *     3 = 建文件 / 写表头失败           —— 文件系统是好的，但写不进去
 *
 *   IMULog_GetFailCode()：
 *     第 1 步 = HAL_SD 错误码归一化后的值：1=命令CRC 2=数据CRC 4=命令无响应(卡没应答)
 *               8=数据超时 16=发送下溢 32=接收上溢 64=地址未对齐 13=上条命令CRC
 *               14=电压范围 15=HAL整体超时 11=地址越界 12=参数错
 *     第 2 步 = 三位数：百位=第几步(1=命令 2=数据 3=等卡回传输态)，后两位=上面的 HAL 错误码；
 *               例 202 = 数据阶段数据CRC错，204 = 数据阶段命令响应超时。
 *               也可能是 FatFs 的 FRESULT（13 = FR_NO_FILESYSTEM，“不是 FAT/FAT32”）。
 *     第 3 步 = FatFs 的 FRESULT
 *
 *   IMULog_GetRawSector0()：认卡成功后裸读到的 0 号扇区头 4 个字节（绕开 FatFs）。
 *     返回 1 = 读到了（FAT: EB 3C 90 / exFAT: EB 76 90 / NTFS: EB 52 90）；0 = 没读到。
 */
uint8_t IMULog_GetFailStage(void);
uint8_t IMULog_GetFailCode(void);
uint8_t IMULog_GetRawSector0(uint8_t *out4);

/* 裸读自检里 1 线 / 4 线各自的失败码（0 = 没失败；含义同 IMULog_GetFailCode）*/
uint8_t IMULog_GetBus1Code(void);
uint8_t IMULog_GetBus4Code(void);
uint8_t IMULog_GetRawStable(void);   /* 1 = 两次裸读字节一致（稳定）*/

#endif /* __IMU_SDLOG_H */
