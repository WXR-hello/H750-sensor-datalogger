/**
  ******************************************************************************
  * @file    imu_pack.c
  * @brief   IWT603 ?????????????? imu_pack.h??
  ******************************************************************************
  */

#include "imu_pack.h"

#include <string.h>

/* float -> 4 ???§³???Cortex-M ????????§³????????????¨À?
   ?? memcpy ??????????????????????????????????µµ */
static void put_f32(uint8_t *p, float v)
{
  memcpy(p, &v, 4);
}

uint16_t imu_pack_build(const iwt603_t *d, uint16_t seq, uint8_t *out)
{
  uint8_t  sum = 0;
  uint16_t i;

  out[0] = 0xAA;
  out[1] = 0x55;
  out[2] = IMU_FRAME_LENBYTE;

  out[3] = (uint8_t)(seq & 0xFF);
  out[4] = (uint8_t)(seq >> 8);

  put_f32(&out[5],  d->roll);
  put_f32(&out[9],  d->pitch);
  put_f32(&out[13], d->yaw);

  put_f32(&out[17], d->ax);
  put_f32(&out[21], d->ay);
  put_f32(&out[25], d->az);

  put_f32(&out[29], d->gx);
  put_f32(&out[33], d->gy);
  put_f32(&out[37], d->gz);

  put_f32(&out[41], d->temp);

  /* §µ?ï“?????????????? temp ?????????????? */
  for (i = 2; i < (IMU_FRAME_LEN - 1); i++)
  {
    sum = (uint8_t)(sum + out[i]);
  }
  out[IMU_FRAME_LEN - 1] = sum;

  return IMU_FRAME_LEN;
}
