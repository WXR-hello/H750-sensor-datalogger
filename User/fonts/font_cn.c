/**
  ******************************************************************************
  * @file    font_cn.c
  * @brief   LCD 文本显示层：中文点阵 + ASCII 放大 + 屏幕旋转
  *
  *   1) 中文：野火例程的中文是从外部 QSPI Flash / SD 卡里的 GB2312 字库取模的，
  *      本工程没有这些外部存储器，所以把用到的汉字提前取模后直接放进内部 Flash，
  *      点阵在 font_cn_data.c（由 make_cn_font.ps1 生成），按 GBK 编码查表。
  *
  *   2) 字号：LCD_TextSetScale(1..4) 整倍数放大，汉字和 ASCII 一起放大。
  *
  *   3) 旋转：LCD_TextSetRotation(0/90/180/270)。LTDC 的时序始终是 800x480（面板不改接线），
  *      旋转是在写像素时做坐标变换：
  *          0 度  : (x, y)          -> (x, y)              逻辑画布 800x480
  *          180 度: (x, y)          -> (799-x, 479-y)      逻辑画布 800x480
  *          90 度 : (x, y)          -> (799-y, x)          逻辑画布 480x800（竖屏）
  *          270 度: (x, y)          -> (y, 479-x)          逻辑画布 480x800（竖屏）
  ******************************************************************************
  */

#include "./fonts/font_cn.h"
#include "./lcd/bsp_lcd.h"

static uint8_t  s_textScale    = 1;
static uint16_t s_textRotation = 0;

/* ---------------- 逻辑坐标 -> 屏幕物理坐标 ---------------- */
static void LCD_TextPutPixel(uint16_t x, uint16_t y, uint32_t color)
{
  uint16_t physW = (uint16_t)LCD_GetXSize();      /* 物理宽 800 */
  uint16_t physH = (uint16_t)LCD_GetYSize();      /* 物理高 480 */
  uint16_t X, Y;

  switch (s_textRotation)
  {
    case 90:
      X = (uint16_t)(physW - 1 - y);
      Y = x;
      break;
    case 180:
      X = (uint16_t)(physW - 1 - x);
      Y = (uint16_t)(physH - 1 - y);
      break;
    case 270:
      X = y;
      Y = (uint16_t)(physH - 1 - x);
      break;
    default:
      X = x;
      Y = y;
      break;
  }

  if ((X < physW) && (Y < physH))
  {
    LCD_DrawPixel(X, Y, color);
  }
}

/* ---------------- 字号 / 旋转设置 ---------------- */
void LCD_TextSetScale(uint8_t scale)
{
  if (scale < LCD_TEXT_SCALE_MIN) { scale = LCD_TEXT_SCALE_MIN; }
  if (scale > LCD_TEXT_SCALE_MAX) { scale = LCD_TEXT_SCALE_MAX; }
  s_textScale = scale;
}

uint8_t LCD_TextGetScale(void)
{
  return s_textScale;
}

void LCD_TextSetRotation(uint16_t degree)
{
  if ((degree != 90) && (degree != 180) && (degree != 270))
  {
    degree = 0;
  }
  s_textRotation = degree;
}

uint16_t LCD_TextGetRotation(void)
{
  return s_textRotation;
}

uint16_t LCD_TextHeight(void)
{
  uint16_t asciiH = ((sFONT *)LCD_GetFont())->Height;
  uint16_t h = (asciiH > CN_FONT_HEIGHT) ? asciiH : CN_FONT_HEIGHT;

  return (uint16_t)(h * s_textScale);
}

uint16_t LCD_TextCanvasWidth(void)
{
  if ((s_textRotation == 90) || (s_textRotation == 270))
  {
    return (uint16_t)LCD_GetYSize();          /* 竖屏：逻辑宽 = 物理高 480 */
  }
  return (uint16_t)LCD_GetXSize();
}

uint16_t LCD_TextCanvasHeight(void)
{
  if ((s_textRotation == 90) || (s_textRotation == 270))
  {
    return (uint16_t)LCD_GetXSize();          /* 竖屏：逻辑高 = 物理宽 800 */
  }
  return (uint16_t)LCD_GetYSize();
}

/* ---------------- 查字模 ---------------- */
const uint8_t *CN_GetGlyph(uint16_t gbk)
{
  uint16_t i;

  for (i = 0; i < g_cnGlyphCount; i++)
  {
    if (g_cnGlyphTable[i].gbk == gbk)
    {
      return g_cnGlyphTable[i].data;
    }
  }
  return 0;
}

/* ---------------- 画汉字（按当前放大倍数） ---------------- */
static void CN_DrawGlyphScaled(uint16_t x, uint16_t y, const uint8_t *data)
{
  uint16_t row, col, dx, dy;
  uint32_t textColor = LCD_GetTextColor();
  uint32_t backColor = LCD_GetBackColor();

  for (row = 0; row < CN_FONT_HEIGHT; row++)
  {
    for (col = 0; col < CN_FONT_WIDTH; col++)
    {
      uint32_t color;

      if ((data[row * CN_FONT_BYTES_PER_ROW + (col >> 3)] & (0x80U >> (col & 7))) != 0)
      {
        color = textColor;
      }
      else
      {
        color = backColor;
      }

      for (dy = 0; dy < s_textScale; dy++)
      {
        for (dx = 0; dx < s_textScale; dx++)
        {
          LCD_TextPutPixel((uint16_t)(x + col * s_textScale + dx),
                           (uint16_t)(y + row * s_textScale + dy), color);
        }
      }
    }
  }
}

/* ---------------- 画 ASCII（用 LCD 自带字库，按当前放大倍数） ---------------- */
static void ASCII_DrawCharScaled(uint16_t x, uint16_t y, uint8_t ch)
{
  sFONT          *font = (sFONT *)LCD_GetFont();
  const uint8_t  *pchar;
  uint16_t        row, col, dx, dy;
  uint16_t        width = font->Width;
  uint16_t        height = font->Height;
  uint8_t         bytesPerRow = (uint8_t)((width + 7) / 8);
  uint8_t         offset = (uint8_t)(8 * bytesPerRow - width);
  uint32_t        textColor = LCD_GetTextColor();
  uint32_t        backColor = LCD_GetBackColor();

  if ((ch < 0x20U) || (ch > 0x7EU))
  {
    return;
  }

  pchar = &font->table[(uint32_t)(ch - 0x20U) * height * bytesPerRow];

  for (row = 0; row < height; row++)
  {
    const uint8_t *prow = pchar + (uint32_t)bytesPerRow * row;
    uint32_t line;

    switch (bytesPerRow)
    {
      case 1:
        line = prow[0];
        break;
      case 2:
        line = ((uint32_t)prow[0] << 8) | prow[1];
        break;
      default:
        line = ((uint32_t)prow[0] << 16) | ((uint32_t)prow[1] << 8) | prow[2];
        break;
    }

    for (col = 0; col < width; col++)
    {
      uint32_t color;

      if ((line & (1UL << (width - col + offset - 1))) != 0)
      {
        color = textColor;
      }
      else
      {
        color = backColor;
      }

      for (dy = 0; dy < s_textScale; dy++)
      {
        for (dx = 0; dx < s_textScale; dx++)
        {
          LCD_TextPutPixel((uint16_t)(x + col * s_textScale + dx),
                           (uint16_t)(y + row * s_textScale + dy), color);
        }
      }
    }
  }
}

/* ---------------- 对外接口 ---------------- */
uint16_t CN_StrWidth(const char *str)
{
  const uint8_t *p = (const uint8_t *)str;
  uint16_t width = 0;
  uint16_t asciiWidth = (uint16_t)(((sFONT *)LCD_GetFont())->Width * s_textScale);

  while (*p != 0)
  {
    if (*p < 0x80U)
    {
      width += asciiWidth;
      p++;
    }
    else
    {
      width += (uint16_t)(CN_FONT_WIDTH * s_textScale);
      p += 2;
    }
  }
  return width;
}

uint16_t LCD_ShowCNString(uint16_t x, uint16_t y, const char *str)
{
  const uint8_t *p = (const uint8_t *)str;
  const uint8_t *glyph;
  uint16_t asciiWidth = (uint16_t)(((sFONT *)LCD_GetFont())->Width * s_textScale);

  while (*p != 0)
  {
    if (*p < 0x80U)
    {
      ASCII_DrawCharScaled(x, y, *p);
      x += asciiWidth;
      p++;
    }
    else
    {
      glyph = CN_GetGlyph((uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]));
      if (glyph != 0)
      {
        CN_DrawGlyphScaled(x, y, glyph);
      }
      x += (uint16_t)(CN_FONT_WIDTH * s_textScale);   /* 表里没有的字留空，不会乱码 */
      p += 2;
    }
  }
  return x;
}

void LCD_TextFillRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint32_t color)
{
  uint16_t i, j;

  for (j = 0; j < h; j++)
  {
    for (i = 0; i < w; i++)
    {
      LCD_TextPutPixel((uint16_t)(x + i), (uint16_t)(y + j), color);
    }
  }
}

/************************ (C) COPYRIGHT *****END OF FILE************************/
