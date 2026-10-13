#include "iwt603.h"

const uint8_t iwt603_cmd_unlock[IWT_CMD_LEN]  = { 0xFF, 0xAA, 0x69, 0x88, 0xB5 };
const uint8_t iwt603_cmd_save[IWT_CMD_LEN]    = { 0xFF, 0xAA, 0x00, 0x00, 0x00 };
const uint8_t iwt603_cmd_restart[IWT_CMD_LEN] = { 0xFF, 0xAA, 0x00, 0xFF, 0x00 };

/* §³??????? -> ?§Ù??? 16 ¦Ë */
static int16_t le_i16(uint8_t lo, uint8_t hi)
{
    return (int16_t)((uint16_t)hi << 8 | (uint16_t)lo);
}

void iwt603_init(iwt603_t *d)
{
    uint16_t i;
    uint8_t *p = (uint8_t *)d;

    for (i = 0; i < sizeof(iwt603_t); i++) {
        p[i] = 0;
    }
}

void iwt603_feed(iwt603_t *d, uint8_t byte)
{
    uint8_t sum = 0;
    uint8_t i;
    int16_t x, y, z, w;

    /* 1) ???? 0x55 */
    if (!d->syncing) {
        if (byte == 0x55) {
            d->frame[0] = byte;
            d->idx      = 1;
            d->syncing  = 1;
        }
        return;
    }

    /* 2) ??? 11 ??? */
    d->frame[d->idx++] = byte;
    if (d->idx < 11) {
        return;
    }
    d->syncing = 0;
    d->idx     = 0;

    /* 3) §µ????????????????????????????????? 0x55?? */
    for (i = 0; i < 10; i++) {
        sum = (uint8_t)(sum + d->frame[i]);
    }
    if (sum != d->frame[10]) {
        return;
    }

    /* 4) ?¨®????????????? 4 ?????????? */
    x = le_i16(d->frame[2], d->frame[3]);
    y = le_i16(d->frame[4], d->frame[5]);
    z = le_i16(d->frame[6], d->frame[7]);
    w = le_i16(d->frame[8], d->frame[9]);

    /* 5) ?????????? */
    switch (d->frame[1]) {
    case 0x51:                                  /* ????? + ??? */
        d->ax   = x / 32768.0f * 16.0f;
        d->ay   = y / 32768.0f * 16.0f;
        d->az   = z / 32768.0f * 16.0f;
        d->temp = w / 100.0f;
        d->updated |= IWT_UPD_ACC;
        break;

    case 0x52:                                  /* ????? + ??? */
        d->gx = x / 32768.0f * 2000.0f;
        d->gy = y / 32768.0f * 2000.0f;
        d->gz = z / 32768.0f * 2000.0f;
        d->voltage = (uint16_t)w;
        d->updated |= IWT_UPD_GYRO;
        break;

    case 0x53:                                  /* ??? + ?·Ú?? */
        d->roll  = x / 32768.0f * 180.0f;
        d->pitch = y / 32768.0f * 180.0f;
        d->yaw   = z / 32768.0f * 180.0f;
        d->version = (uint16_t)w;
        d->updated |= IWT_UPD_ANGLE;
        break;

    case 0x54:                                  /* ??? + ??? */
        d->mx   = (float)x;
        d->my   = (float)y;
        d->mz   = (float)z;
        d->temp = w / 100.0f;
        d->updated |= IWT_UPD_MAG;
        break;

    case 0x59:                                  /* ????? q0..q3 */
        d->q0 = x / 32768.0f;
        d->q1 = y / 32768.0f;
        d->q2 = z / 32768.0f;
        d->q3 = w / 32768.0f;
        d->updated |= IWT_UPD_QUAT;
        break;

    default:
        break;
    }
}

void iwt603_make_cmd(uint8_t addr, uint16_t data, uint8_t out[IWT_CMD_LEN])
{
    out[0] = 0xFF;
    out[1] = 0xAA;
    out[2] = addr;
    out[3] = (uint8_t)(data & 0xFF);         /* ???????? */
    out[4] = (uint8_t)((data >> 8) & 0xFF);
}

/* ---------- ?????? printf ????????? ---------- */

static char *put_uint(char *p, uint32_t whole, uint32_t frac)
{
    char tmp[12];
    int  n = 0;

    if (whole == 0u) {
        tmp[n++] = '0';
    }
    while (whole > 0u) {
        tmp[n++] = (char)('0' + (whole % 10u));
        whole /= 10u;
    }
    while (n > 0) {
        *p++ = tmp[--n];
    }
    *p++ = '.';
    *p++ = (char)('0' + (frac / 10u) % 10u);
    *p++ = (char)('0' + frac % 10u);
    return p;
}

static char *fmt_fixed2(char *p, float x)
{
    int32_t v;
    uint32_t a;

    /* ???????? 0.01 */
    v = (x >= 0.0f) ? (int32_t)(x * 100.0f + 0.5f)
                    : (int32_t)(x * 100.0f - 0.5f);
    if (v < 0) {
        *p++ = '-';
        a = (uint32_t)(-v);
    } else {
        a = (uint32_t)v;
    }
    return put_uint(p, a / 100u, a % 100u);
}

int iwt603_format_angle_csv(const iwt603_t *d, char *buf, int bufsize)
{
    char *p = buf;

    /* "-180.00,-180.00,-180.00\n" ?? 27 ??????? '\0' */
    if (bufsize < 33) {
        return 0;
    }
    p = fmt_fixed2(p, d->roll);   *p++ = ',';
    p = fmt_fixed2(p, d->pitch);  *p++ = ',';
    p = fmt_fixed2(p, d->yaw);    *p++ = '\n';
    *p = '\0';
    return (int)(p - buf);
}
