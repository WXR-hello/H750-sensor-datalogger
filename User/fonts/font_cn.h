#ifndef __FONT_CN_H
#define __FONT_CN_H

#include <stdint.h>   /* uint8_t / uint16_t */

/* 24x24 汉字点阵
   点阵数据在 font_cn_data.c 里，由 User/fonts/make_cn_font.ps1 取模生成；
   要增加汉字时改脚本的 -Chars 参数后重新运行即可（不依赖外部字库/文件系统）。
   想换更大的汉字点阵：脚本用 -Cell 32/40/48 生成，同时把下面三个宏改成对应数值。 */
#define CN_FONT_WIDTH          ((uint16_t)24)
#define CN_FONT_HEIGHT         ((uint16_t)24)
#define CN_FONT_BYTES_PER_ROW  ((uint16_t)3)

typedef struct
{
  uint16_t       gbk;    /* 该汉字的 GBK 双字节编码 */
  const uint8_t *data;   /* 行优先，每行 CN_FONT_BYTES_PER_ROW 字节，MSB 在左 */
} CN_GlyphTypeDef;

extern const CN_GlyphTypeDef g_cnGlyphTable[];
extern const uint16_t        g_cnGlyphCount;

/* ---------------- 字号放大 / 屏幕旋转 ---------------- */
#define LCD_TEXT_SCALE_MIN     (1)
#define LCD_TEXT_SCALE_MAX     (4)

void     LCD_TextSetScale(uint8_t scale);          /* 整倍数放大：1/2/3/4 */
uint8_t  LCD_TextGetScale(void);
void     LCD_TextSetRotation(uint16_t degree);     /* 屏幕旋转：0/90/180/270 */
uint16_t LCD_TextGetRotation(void);

uint16_t LCD_TextHeight(void);                     /* 一行文字的高度（像素） */
uint16_t LCD_TextCanvasWidth(void);                /* 逻辑画布宽：0/180 度=800，90/270 度=480 */
uint16_t LCD_TextCanvasHeight(void);               /* 逻辑画布高：0/180 度=480，90/270 度=800 */

const uint8_t *CN_GetGlyph(uint16_t gbk);          /* 按 GBK 编码查字模，找不到返回 0 */
uint16_t       CN_StrWidth(const char *str);       /* 中英混排字符串的像素宽度 */
uint16_t       LCD_ShowCNString(uint16_t x, uint16_t y, const char *str);  /* 显示中英混排字符串 */
void           LCD_TextFillRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint32_t color);

#endif /* __FONT_CN_H */
