/**
  ******************************************************************************
  * @file    main.c
  * @brief   STM32H750XB + IWT603（维特官方 wit_c_sdk）+ 数据记录到 SD 卡
  *
  *
  *  IMU 走 USB Host（模块的 USB 是标准 CDC 虚拟串口）
  *      接线：模块 Type-C --（A 转 C 数据线）--> 底板 USB HOST 口；板子 DC 供电
  *      MCU ：USB_OTG_HS 的片内全速 PHY（D+/D- = PB14/PB15），
  *            48MHz 内核时钟来自 PLL3Q（PLL3 同时给 LTDC 供 27MHz，见 bsp_lcd.c）
  *      收字节：CDC 批量 IN 端点 -> USBH_CDC_ReceiveCallback() -> SensorUsbInput()
  *      发指令：SensorUsbSend() -> USB_Host_SendBytes() -> CDC 批量 OUT 端点
  *
  *
  *  数据换算（和官方例程一样）：
  *    加速度 = sReg[AX..AZ]/32768*16 (g)
  *    角速度 = sReg[GX..GZ]/32768*2000 (度/秒)
  *    角度   = sReg[Roll..Yaw]/32768*180 (度)
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "wit_c_sdk.h"
#include "./mpu/bsp_mpu.h"
#include "./lcd/bsp_lcd.h"     /* LCD 显示驱动（只移植了显示部分，没触摸） */
#include "./fonts/font_cn.h"   /* 中文点阵显示（字库在内部 Flash） */
#include "./sd/imu_sdlog.h"    /* IMU 数据记录到 SD 卡（FatFs） */
#include "./encoder/bsp_encoder.h"  /* 增量式编码器（A/B 硬件编码器模式 + Z 相 EXTI，引脚见该头文件）*/
#include "relay.h"               /* 按键 KEY1（PA0）→ 继电器（IN = PB4）*/
#include "./touch/bsp_i2c_touch.h"   /* 电容触摸屏：软件 I2C（PH4/PH5）*/
#include "./touch/bsp_touch_gtxx.h"  /* GT1151QM 触摸驱动 */
#include "ff.h"                       /* 数据显示页：列 SD 卡里的记录文件 */
#include "force.h"
#include "imu.h"                /* IMU（USB Host）初始化与轮询，见 imu.c */
#include "./usbhost/usb_host.h"   /* IMU 走 USB Host（模块的 USB 是标准 CDC 虚拟串口）*/

#include <stdio.h>             /* LCD 数值格式化里用到 sprintf */
#include <string.h>

static uint8_t g_lcdForceRedraw = 0U;   /* 1 = 让数据表整幅重画一次（切页时用）*/


/* 参数 ----------------------------------------------------------------*/
#define LOG_PERIOD_MS        50U       /* 最快 50ms 记一条（20Hz）*/


static void FillSample(IMULogSampleTypeDef *s, uint32_t t_ms);
void SystemClock_Config(void);


/* LCD 显示部分函数 */
static void LCD_DisplayInit(void);
static void LCD_UpdateImu(void);
static void LCD_ShowStatus(const char *text);
static void LCD_ShowSdStatus(void);
static void LCD_ShowTitle(const char *text);
static void FmtUInt(char *p, uint32_t v);
static void FmtInt(char *p, int32_t v);
static void FmtHex2(char *p, uint8_t v);
static void LCD_ShowInfo(const char *text);
static void LCD_UpdateUsbStatus(void);
static void FmtHex4(char *p, uint16_t v);

/**
  * @brief  main
  */


/* ==========================================================================
 * 触摸交互界面（竖屏 480 x 800）
 *   菜单页：开始存储 / 停止存储 / 数据显示
 *   数据页：列出 SD 卡上已记录的文件 + 左下角"返回"
 * ========================================================================== */
#define UI_PAGE_MENU     0U
#define UI_PAGE_DATA     1U

#define UI_BTN_X         40
#define UI_BTN_W         400
#define UI_BTN_H         110
#define UI_BTN1_Y        210
#define UI_BTN2_Y        350
#define UI_BTN3_Y        490
#define UI_BACK_X        0          /* 返回按钮：屏幕最底部整条，不压数据、外面不留白 */
#define UI_BACK_Y        752
#define UI_BACK_W        480
#define UI_BACK_H        48

/* 提示框：480x800 的正中 */
#define UI_POP_W         360
#define UI_POP_H         110
#define UI_POP_X         ((480 - UI_POP_W) / 2)
#define UI_POP_Y         ((800 - UI_POP_H) / 2)

/* 触摸坐标 -> 界面坐标：
   界面是 90° 旋转显示的（逻辑画布 480x800，物理面板 800x480），
   字体模块的映射是 (逻辑x,逻辑y) -> (物理x = 799-逻辑y, 物理y = 逻辑x)，
   所以反解回来：逻辑x = 触摸y，逻辑y = 799 - 触摸x。
   ★ 若发现左右/上下反了，只改下面这两行即可。*/
#define UI_TP2X(py)   ((int32_t)(py))
#define UI_TP2Y(px)   ((int32_t)(799 - (px)))

static uint8_t  g_uiPage    = UI_PAGE_MENU;
static uint8_t  g_recOn     = 0U;
static uint32_t g_recT0     = 0U;            /* 开始存储那一刻的 tick（CSV 时间戳从 0 算）*/
static uint8_t  g_lcdShown  = 0U;            /* 当前显示的是哪个图层（双缓冲）*/
static uint8_t  g_lcdDraw   = 0U;            /* 当前正在往哪个图层画（DMA2D 填充要用）*/
static uint8_t  g_recFirst  = 0U;            /* 1 = 本段还没写过第一行（第一行写 0）*/
static uint8_t  g_popup     = 0U;            /* 1 = 正在显示“请插入SD卡”提示框 */            /* 1 = 正在往 SD 卡记录 */
static uint32_t g_recFrames = 0U;            /* 本次已记录帧数 */
static uint8_t  g_uiRedraw  = 1U;            /* 1 = 需要整页重画 */

/* 触摸取"按下沿"：返回 1 表示这一刻刚按下（带坐标）*/
static uint8_t Ui_GetPress(int32_t *cx, int32_t *cy)
{
  static uint8_t s_last = 0U;
  int     x = -1, y = -1;
  int     r = GTP_Execu(&x, &y);
  uint8_t now = ((r != 0) && (x >= 0) && (y >= 0)) ? 1U : 0U;
  uint8_t hit = 0U;

  if ((now != 0U) && (s_last == 0U))
  {
    *cx = UI_TP2X(y);
    *cy = UI_TP2Y(x);
    hit = 1U;
  }
  s_last = now;
  return hit;
}

static uint8_t Ui_In(int32_t x, int32_t y, int32_t bx, int32_t by, int32_t bw, int32_t bh)
{
  return ((x >= bx) && (x < (bx + bw)) && (y >= by) && (y < (by + bh))) ? 1U : 0U;
}

/* ---- 用 DMA2D 填一块矩形：比逐像素快几百倍 ---- */
static void Ui_FillRectFast(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint32_t color)
{
  /* v85 的做法：普通逐像素填充（不用自组 DMA2D，避免像素格式搞错导致花屏）*/
  LCD_TextFillRect(x, y, w, h, color);
}

static void Ui_Button(int32_t x, int32_t y, int32_t w, int32_t h, const char *txt, uint32_t bg)
{
  uint16_t tw = CN_StrWidth(txt);

  Ui_FillRectFast((uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h, bg);
  LCD_SetColors(LCD_COLOR_WHITE, bg);
  LCD_ShowCNString((uint16_t)(x + ((w - (int32_t)tw) / 2)),
                   (uint16_t)(y + ((h - (int32_t)LCD_TextHeight()) / 2)), txt);
  LCD_SetColors(LCD_COLOR_WHITE, LCD_COLOR_BLACK);
}

/* 菜单页的状态区（重量 / ADC / 记录状态），单独重画 */
static void Ui_DrawStatus(void)
{
  char b[40];

  LCD_SetColors(LCD_COLOR_WHITE, LCD_COLOR_BLACK);
  LCD_TextFillRect(20, 70, 440, 100, LCD_COLOR_BLACK);

  sprintf(b, "重量 %ld.%02ld kg", (long)(g_forceG / 1000), (long)((g_forceG % 1000) / 10));
  LCD_ShowCNString(20, 70, b);

  sprintf(b, "ADC  %ld", (long)g_forceAdc);
  LCD_ShowCNString(20, 120, b);

  LCD_TextFillRect(20, 640, 440, 40, LCD_COLOR_BLACK);
  if (g_recOn != 0U) { sprintf(b, "记录中 %lu 帧", (unsigned long)g_recFrames); }
  else               { sprintf(b, "未记录"); }
  LCD_ShowCNString(20, 640, b);
}

/* 屏幕中心的提示框（无卡时点开始存储弹出来，点任意处关闭）*/
static void Ui_DrawPopup(void)
{
  uint16_t w = UI_POP_W, h = UI_POP_H;
  uint16_t x = UI_POP_X;
  uint16_t y = UI_POP_Y;                  /* 屏幕正中 */

  Ui_FillRectFast(x, y, w, h, LCD_COLOR_DARKGRAY);            /* 方框（DMA2D）*/
  LCD_SetColors(LCD_COLOR_WHITE, LCD_COLOR_DARKGRAY);
  LCD_ShowCNString((uint16_t)(x + 44), (uint16_t)(y + 44), "请插入SD卡");
  LCD_SetColors(LCD_COLOR_WHITE, LCD_COLOR_BLACK);
}

/* 菜单页底部：没插 SD 卡时显示提示 */
static char     g_cardLast[40] = "";          /* 上一次画的状态文字（没变就不重画）*/
static uint32_t g_cardLastCol  = 0xFFFFFFFFU;

/* 整屏重画后调用：让状态行强制重画一次（否则会被判成“没变化”而不画）*/
static void Ui_CardLineInvalidate(void)
{
  g_cardLast[0]  = 0;
  g_cardLastCol  = 0xFFFFFFFFU;
}

static void Ui_DrawCardLine(void)
{
  char     b[40];
  uint32_t col;

  if (IMULog_IsReady() == 0U)                     /* 没插卡 */
  {
    strcpy(b, "无卡插入");      col = LCD_COLOR_RED;
  }
  else if (g_recOn != 0U)                         /* 正在记录（带文件名）*/
  {
    sprintf(b, "正在记录 %s", IMULog_GetFileName());
    col = LCD_COLOR_GREEN;
  }
  else                                            /* 没在记录 */
  {
    strcpy(b, "已停止记录");    col = LCD_COLOR_YELLOW;
  }

  if ((strcmp(b, g_cardLast) == 0) && (col == g_cardLastCol)) { return; }  /* 没变就不重画（不闪）*/
  (void)strcpy(g_cardLast, b);
  g_cardLastCol = col;

  Ui_FillRectFast(0, 748, 480, 52, LCD_COLOR_BLACK);
  LCD_SetColors(col, LCD_COLOR_BLACK);
  LCD_ShowCNString(10, 762, b);
  LCD_SetColors(LCD_COLOR_WHITE, LCD_COLOR_BLACK);
}

static void Ui_DrawMenu(void)
{
  LCD_Clear(LCD_COLOR_BLACK);            /* DMA2D 硬件清屏（野火原生，已验证）*/
  LCD_SetColors(LCD_COLOR_WHITE, LCD_COLOR_BLACK);
	LCD_TextSetScale(2);
  LCD_ShowCNString(190, 60, "菜单");
	LCD_TextSetScale(1);

  Ui_Button(UI_BTN_X, UI_BTN1_Y, UI_BTN_W, UI_BTN_H, "开始存储", LCD_COLOR_DARKGREEN);
  Ui_Button(UI_BTN_X, UI_BTN2_Y, UI_BTN_W, UI_BTN_H, "停止存储", LCD_COLOR_DARKRED);
  Ui_Button(UI_BTN_X, UI_BTN3_Y, UI_BTN_W, UI_BTN_H, "数据显示", LCD_COLOR_DARKBLUE);

  Ui_CardLineInvalidate();              /* 整屏刚重画过，状态行要重画 */
  Ui_DrawCardLine();
}

static void Ui_DrawData(void)
{
  /* 实时数值页：数据行由 LCD_UpdateImu() 负责画（每 50ms 刷新），
     这里只清屏、请求整幅重画一次、画左下角的返回按钮。*/
  LCD_Clear(LCD_COLOR_BLACK);            /* DMA2D 硬件清屏（野火原生，已验证）*/
  LCD_SetColors(LCD_COLOR_WHITE, LCD_COLOR_BLACK);

  LCD_DisplayInit();                        /* 重画中文标签/单位（清屏后标签会丢）*/
  g_lcdForceRedraw = 1U;                    /* 让数据表整幅重画一次 */

  Ui_Button(UI_BACK_X, UI_BACK_Y, UI_BACK_W, UI_BACK_H, "返回", LCD_COLOR_DARKGRAY);
}

/* 触摸 + 页面刷新，主循环里每 30ms 调一次 */
static void Ui_Task(void)
{
  int32_t tx = 0, ty = 0;

  if (Ui_GetPress(&tx, &ty) != 0U)
  {
    if (g_popup != 0U)                              /* 弹框在：点任意处关掉它 */
    {
      g_popup = 0U;
      Ui_FillRectFast(UI_POP_X, UI_POP_Y, UI_POP_W, UI_POP_H, LCD_COLOR_BLACK);  /* 只擦框（DMA2D）*/
      if (g_uiPage == UI_PAGE_MENU)   /* 框在正中，会盖住中间的按钮，补画回来 */
      {
        Ui_Button(UI_BTN_X, UI_BTN2_Y, UI_BTN_W, UI_BTN_H, "停止存储", LCD_COLOR_DARKRED);
      }
    }
    else if (g_uiPage == UI_PAGE_MENU)
    {
      if (Ui_In(tx, ty, UI_BTN_X, UI_BTN1_Y, UI_BTN_W, UI_BTN_H) != 0U)
      {
        if (IMULog_IsReady() == 0U)                   /* 没插卡：弹提示框 */
        {
          g_popup = 1U;
          Ui_DrawPopup();                             /* 直接把框画上去，不整页重画 */
        }
        else if (g_recOn == 0U)                       /* 有卡，且当前没在记录 */
        {
          /* 上电那个文件还没写过数据 -> 继续用它；<br>写过数据 -> 新建一个文件，不覆盖旧的 */
          if ((IMULog_GetCount() == 0U) || (IMULog_NewFile() != 0U))
          {
            g_recOn     = 1U;
            g_recT0     = HAL_GetTick();              /* 时间戳从 0 开始 */
            g_recFrames = 0U;
            g_recFirst  = 1U;                         /* 第一行时间戳写 0 */
          }
          else
          {
            g_popup = 1U;                             /* 建不了文件：提示插卡 */
            LCD_TextFillRect(UI_POP_X, UI_POP_Y, UI_POP_W, UI_POP_H, LCD_COLOR_BLACK);
            Ui_DrawPopup();
          }
        }
        Ui_DrawCardLine();                            /* 只刷底部状态行（不整页重画）*/
      }
      else if (Ui_In(tx, ty, UI_BTN_X, UI_BTN2_Y, UI_BTN_W, UI_BTN_H) != 0U)
      {
        g_recOn = 0U;                                 /* 停止存储 */
        Ui_DrawCardLine();                            /* 只刷底部状态行 */
      }
      else if (Ui_In(tx, ty, UI_BTN_X, UI_BTN3_Y, UI_BTN_W, UI_BTN_H) != 0U)
      {
        g_uiPage = UI_PAGE_DATA;  g_uiRedraw = 1U;    /* 数据显示 */
      }
    }
    else
    {
      if (Ui_In(tx, ty, UI_BACK_X, UI_BACK_Y, UI_BACK_W, UI_BACK_H) != 0U)
      {
        g_uiPage = UI_PAGE_MENU;  g_uiRedraw = 1U;    /* 返回 */
      }
    }
  }

  if (g_uiRedraw != 0U)
  {
    uint8_t hidden = (uint8_t)(1U - g_lcdShown);        /* 看不见的那一层 */

    g_uiRedraw = 0U;
    LCD_SelectLayer(hidden);                            /* 先画到隐藏层 */
    g_lcdDraw = hidden;                                 /* DMA2D 也画这一层 */
    if (g_uiPage == UI_PAGE_MENU) { Ui_DrawMenu(); }
    else                          { Ui_DrawData(); }
    if (g_popup != 0U)            { Ui_DrawPopup(); }

    LCD_SetTransparency(hidden, 255U);                  /* 一次切换：新层显示 */
    LCD_SetTransparency(g_lcdShown, 0U);                /* 旧层透明 */
    g_lcdShown = hidden;
    LCD_SelectLayer(g_lcdShown);                        /* 之后的局部刷新都画这层 */
    g_lcdDraw = g_lcdShown;
  }
  else if (g_uiPage == UI_PAGE_MENU)
  {
    static uint32_t s_cardTick = 0U;
    if ((HAL_GetTick() - s_cardTick) >= 500U)      /* 每 0.5 秒看一次卡状态 */
    {
      s_cardTick = HAL_GetTick();
      Ui_DrawCardLine();
    }
  }
  else if (g_uiPage == UI_PAGE_DATA)                /* 只有“数据显示”页才刷实时数值 */
  {
    LCD_UpdateImu();
  }
}

int main(void)
{
  uint32_t lastLogTick = 0U;

  HAL_Init();
  SystemClock_Config();

  /* 初始化 LCD（显示功能移植自野火 LTDC 例程，带 SDRAM 显存 / MPU / Cache）。
     注意：LCD_Init() 里同时把 PLL3 配好了 —— PLL3R=432/16=27MHz 给 LTDC，
     PLL3Q=432/9=48MHz 留给 USB（见 User/lcd/bsp_lcd.c 的 LCD_ClockConfig）。*/
  LCD_DisplayInit();

  /* ---- 电容触摸屏初始化（I2C: PH4/PH5，RST: PG7，INT: PG3）---- */
  /* ---- 双缓冲：第二层显存紧跟第一层（800*480*4 字节），先设透明 ---- */
  LCD_LayerInit(1, LCD_FB_START_ADDRESS + (800U * 480U * 4U), RGB888);
  LCD_SetTransparency(0, 255U);      /* 第 0 层不透明（显示中）*/
  LCD_SetTransparency(1, 0U);        /* 第 1 层透明（看不见）*/
  LCD_SelectLayer(0);

  I2C_Touch_Init();
  (void)GTP_Init_Panel();

  /* ---- 增量编码器：A/B 相走定时器硬件编码器模式（引脚见 bsp_encoder.h 的 ENCODER_PIN_SET），
         Z 相走 PA8 外部中断 ----*/
         
  Encoder_Init();



  /* ---- 继电器 + KEY1：按住吸合，松开释放（COM-NC 常闭）---- */
  Relay_Init();

  /* 开机先显示这一行（1~2 秒后会被实时状态覆盖）：
     用来一眼确认“现在跑的是哪一版固件、IMU 走哪条链路”。
     如果开机屏幕上看不到这一行，说明烧的不是这个固件（比如烧成了上一版的 hex）。 */
  LCD_ShowTitle("USBLOG v96");

  /* ---- 传感器初始化：力传感器 ADC + IMU(USB Host)，代码全部在 force.c 里 ---- */
  LCD_ShowStatus("USB 开始连接");
  Force_Init();             /* 力传感器 */
  Imu_Init();               /* IMU（USB Host）*/

  /* ---- SD 卡 + 新建记录文件（没插卡也能继续跑，插上后会自动重试）---- */
  (void)IMULog_Init();

  while (1)
  {
    /* ---- 定时记录：严格 50ms 一行 ----
       主循环偶尔被卡住也不丢拍：把落后的那一拍用最新数据补齐，
       所以 CSV 里 t_ms 永远是 50,100,150,200... 这样整齐的 50ms 网格。*/
    {
      uint32_t now = HAL_GetTick();
      uint8_t  k;

      for (k = 0U; (k < 10U) && ((now - lastLogTick) >= LOG_PERIOD_MS); k++)
      {
        IMULogSampleTypeDef sample;

        lastLogTick += LOG_PERIOD_MS;        /* 严格踩 50ms 网格 */
        if (g_recOn != 0U)                   /* 只有点了“开始存储”才写 SD */
        {
          if (g_recFirst != 0U)                     /* 本段第一行：固定写 0 */
          {
            g_recFirst = 0U;
            lastLogTick = g_recT0;                /* grid restarts at record start */
            FillSample(&sample, 0U);
          }
          else                                      /* 之后按 50ms 网格：50,100,150... */
          {
            FillSample(&sample, lastLogTick - g_recT0);
          }
          IMULog_WriteSample(&sample);
          g_recFrames++;
        }
      }

    }

    /* card pulled while recording: auto-stop + popup; already-flushed data is kept */
    if ((g_recOn != 0U) && (IMULog_IsReady() == 0U))
    {
      g_recOn    = 0U;
      g_popup    = 1U;
      g_uiRedraw = 1U;
    }
//编码器初始化
    /* 编码器：把定时器里的增量并进累计计数（Z 相到来时自动以零位为准），
       顺便算一次转速（函数内部自己限速，20ms 才更新一次）*/
    (void)Encoder_ReadDelta();
    Encoder_UpdateSpeed();


    IMULog_Task();          /* 定期落盘；没卡/写失败时每 2 秒自动重试 */
    Relay_Task();           /* 读 KEY1（PA0）并驱动继电器：按住吸合，松开释放 */

    Force_Task();            /* 力传感器采样（代码在 force.c 里）*/
    Imu_Task();              /* IMU：跑 USB 主机状态机（代码在 imu.c 里）*/

    /* ---- 触摸界面（菜单页 / 数据显示页），30ms 处理一次 ---- */
    {
      static uint32_t s_uiTick = 0U;
      if ((HAL_GetTick() - s_uiTick) >= 30U)
      {
        s_uiTick = HAL_GetTick();
        Ui_Task();
      }
    }
  }
}

/* ---- 把 sReg[] 里的最新数据整理成一条记录 ---- */
static void FillSample(IMULogSampleTypeDef *s, uint32_t t_ms)
{
  s->t_ms  = t_ms;

  s->roll  = sReg[Roll]  / 32768.0f * 180.0f;
  s->pitch = sReg[Pitch] / 32768.0f * 180.0f;
  s->yaw   = sReg[Yaw]   / 32768.0f * 180.0f;

  s->ax    = sReg[AX] / 32768.0f * 16.0f;
  s->ay    = sReg[AY] / 32768.0f * 16.0f;
  s->az    = sReg[AZ] / 32768.0f * 16.0f;

  s->gx    = sReg[GX] / 32768.0f * 2000.0f;
  s->gy    = sReg[GY] / 32768.0f * 2000.0f;
  s->gz    = sReg[GZ] / 32768.0f * 2000.0f;

  s->enc_deg = Encoder_GetAngleDeg();     /* 编码器角度（度）*/

  s->rpm     = (float)Encoder_GetSpeedRpm();  /* 转速（转/分）*/
  s->z_cnt   = Encoder_GetZCount();        /* Z 脉冲数 */
  s->force   = g_forceG;              /* 力值（克）：已去皮、已按 0~50kg 标定 */
}


/* ---- 系统时钟：HSE 25MHz + PLL1 -> SYSCLK 400MHz -----------------------*/
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);
  while (!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}

  RCC_OscInitStruct.OscillatorType      = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState            = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState        = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource       = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM            = 5;
  RCC_OscInitStruct.PLL.PLLN            = 160;
  RCC_OscInitStruct.PLL.PLLP            = 2;
  RCC_OscInitStruct.PLL.PLLQ            = 4;
  RCC_OscInitStruct.PLL.PLLR            = 2;
  RCC_OscInitStruct.PLL.PLLRGE          = RCC_PLL1VCIRANGE_2;
  RCC_OscInitStruct.PLL.PLLVCOSEL       = RCC_PLL1VCOWIDE;
  RCC_OscInitStruct.PLL.PLLFRACN        = 0;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  RCC_ClkInitStruct.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                                     RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2 |
                                     RCC_CLOCKTYPE_D3PCLK1 | RCC_CLOCKTYPE_D1PCLK1;
  RCC_ClkInitStruct.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.SYSCLKDivider  = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.AHBCLKDivider  = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV2;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV2;
  RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV2;
  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
  {
    Error_Handler();
  }
}

/* ==========================================================================
 * LCD 显示部分
 *   显示部分移植自野火 H750「LTDC 电容触摸屏—触摸画板」例程的 User\lcd + User\sdram + User\mpu，
 *   只含显示相关代码（LTDC + DMA2D + SDRAM 显存 + 字体），
 *   触摸、调色板、画板等和显示无关的内容一律没有移植。
 *
 *   中文显示：野火例程的中文是从外部 QSPI Flash（或 SD 卡）的 GB2312 字库取模的，
 *             本工程没有那些外部存储器，所以改成把用到的汉字取模后直接放进内部 Flash，
 *             见 User/fonts/font_cn.c 与 font_cn_data.c（由 make_cn_font.ps1 生成）。
 *
 *   本版 LCD 的用法：
 *     * 标题行（第 0 槽）显示正在记录的 CSV 文件名（如 IMU0001.CSV）；
 *     * 中间是 IMU 各通道的实时数值；
 *     * 最底下一行是状态：正在查找传感器 / 无传感器 / 无SD / SD读写错误 / 已存 N 帧。
 * ========================================================================== */

/* ======================= 想改显示，只改下面两行 =======================
 *   LCD_UI_SCALE    : 字号放大倍数，1 / 2 / 3 / 4（汉字和英文一起放大）
 *   LCD_UI_ROTATION : 屏幕旋转角度，0 / 90 / 180 / 270
 *
 *   说明：
 *     * 字号调大后每行更高，能放下的行数变少，程序会自动按优先级少显示几行
 *       （顺序：横滚/俯仰/航向 → 加速度 XYZ → 角速度 XYZ → 温度）。
 *       想让大字体显示更多内容，就用 90 或 270 度（竖屏，逻辑画布变成 480x800）。
 *     * 旋转只是把画面内容转过来，LTDC 时序、屏幕接线都不用改。
 *       如果实际装屏方向反了，把 90 和 270 对调即可；180 度用于屏装倒了的情况。
 *     * 想要更大更漂亮的汉字（而不是把 24x24 放大），可以用
 *       User/fonts/make_cn_font.ps1 -Cell 32 重新取模，
 *       并把 font_cn.h 里的 CN_FONT_WIDTH/HEIGHT 改成 32、CN_FONT_BYTES_PER_ROW 改成 4。
 * ===================================================================== */
#define LCD_UI_SCALE      1
#define LCD_UI_ROTATION   90

#define LCD_UI_TITLE_TEXT ""      /* 标题行固定文字（"" = 留给运行时显示文件名，不写死内容）*/
#define LCD_ROW_MAX       14          /* 10 行 IMU + 角度 + 3 行编码器诊断 */          /* 数据行种类数 */
#define LCD_VALUE_CHARS   8           /* 数值列固定宽度（字符数），保证单位列对齐、擦掉残留 */

/* 一行数据：中文标签 + 单位 + 取自 sReg[] 哪个寄存器 + 换算系数 + 显示小数位 */
typedef struct
{
  const char *label;
  const char *unit;
  uint8_t     reg;
  float       k;                      /* 物理量 = 原始值 * k */
  int32_t     mul;                    /* 显示放大倍数：1000/100/10 */
  uint8_t     digit;                  /* 小数位数：3/2/1 */
} LCD_RowTypeDef;

static const LCD_RowTypeDef s_lcdRowTable[LCD_ROW_MAX] =
{
  /* ===== IMU 各通道（保持原来的显示，未改动）===== */
  { "横滚：",     "度",     Roll,   180.0f / 32768.0f,  100,  2 },
  { "俯仰：",     "度",     Pitch,  180.0f / 32768.0f,  100,  2 },
  { "航向：",     "度",     Yaw,    180.0f / 32768.0f,  100,  2 },
  { "加速度 X：", "g",      AX,      16.0f / 32768.0f, 1000,  3 },
  { "加速度 Y：", "g",      AY,      16.0f / 32768.0f, 1000,  3 },
  { "加速度 Z：", "g",      AZ,      16.0f / 32768.0f, 1000,  3 },
  { "角速度 X：", "度/秒",   GX,    2000.0f / 32768.0f,  10,  1 },
  { "角速度 Y：", "度/秒",   GY,    2000.0f / 32768.0f,  10,  1 },
  { "角速度 Z：", "度/秒",   GZ,    2000.0f / 32768.0f,  10,  1 },

  /* ===== 编码器部分：只显示一个角度（自 Z 零位起算 0~360 度）=====
   *   reg = 0xF0~0xF4 是"编码器数据"的特殊标记（不走 sReg[]）：
   *     0xF0 累计计数 / 0xF1 角度 / 0xF2 转速 / 0xF3 位置 / 0xF4 Z 脉冲数
   *   想看编码器其它量：把下面 #if 0 里的行挪上来，并把 LCD_ROW_MAX 加几行即可。
   */
  { "角度：",     "度",     0xF1,     1.0f,              1,  0 },
#if (ENCODER_PIN_TEST == 1)
  /* ---- 引脚观测模式：直接显示 A/B 相（当前方案的引脚）和 PA8(Z) 的电平 ---- */
  { "A 相：",     "",       0xF6,     1.0f,              1,  0 },
  { "B 相：",     "",       0xF7,     1.0f,              1,  0 },
  { "Z 相：",     "",       0xF8,     1.0f,              1,  0 },
#else
  /* ---- 正常模式：排查用的诊断显示（调好后可删掉）---- */
  { "重量：",     "kg",     0xFA,     1.0f,              1,  0 },
  { "ADC：",      "",       0xFB,     1.0f,              1,  0 },  /* 力传感器原始码值（0~65535）*/
#endif

#if 0
  { "编码器计数：", "",      0xF0,     1.0f,              1,  0 },
  { "编码器转速：", "转/分",  0xF2,     1.0f,              1,  0 },
  { "编码器位置：", "",      0xF3,     1.0f,              1,  0 },
  { "编码器零位：", "",      0xF4,     1.0f,              1,  0 },
  { "磁场 X：",   "",       HX,       1.0f,              1,  0 },
  { "磁场 Y：",   "",       HY,       1.0f,              1,  0 },
  { "磁场 Z：",   "",       HZ,       1.0f,              1,  0 },
  { "电压：",     "",       BATVAL,   1.0f,              1,  0 },
  { "版本：",     "",       VERSION,  1.0f,              1,  0 },
#endif
};

/* 实际要显示的行：表里空着的行（label 为 0 或空串）会自动跳过，方便以后加减行 */
static uint16_t s_rowIndex[LCD_ROW_MAX];
static uint16_t s_rowCount = 0;

/* 排版结果（LCD_DisplayInit 里算好，后面直接用） */
static uint16_t s_rowShown  = 0;      /* 实际显示的数据行数 */
static uint16_t s_titleY    = 0;
static uint16_t s_slotH     = 0;      /* 每个槽的高度（把标题/数据行/状态行平均铺满整屏） */
static uint16_t s_dataPad   = 0;      /* 数据行在槽内的上边距（垂直居中） */
static uint16_t s_textPad   = 0;      /* 标题/状态行在槽内的上边距 */
static uint16_t s_dataSlot0 = 0;      /* 第一行数据所在槽号 */
static uint16_t s_statusY   = 0;
static uint16_t s_valueX    = 0;      /* 数值列起点 */
static uint16_t s_unitX     = 0;      /* 单位列起点 */
static uint8_t  s_hasTitle  = 0;
static uint8_t  s_hasStatus = 0;
static uint8_t  s_hasInfo   = 0;      /* USB 版：标题下面多一行“USB 状态” */
static uint16_t s_infoY     = 0;
static uint8_t  s_showUnit  = 1;
static uint8_t  s_twoLine   = 0;      /* 1 = 标签一行、数值一行（竖屏或大字号放不下时自动启用） */
static uint16_t s_rowSpan   = 1;      /* 每行数据占几行文字 */
static uint16_t s_valueChars = 8;     /* 数值列实际宽度（字符），按可用宽度自动收窄 */

static char s_textBuf[16];

/**
  * @brief  浮点转 "+123.45" 字符串（不用 %f，兼容 microlib）
  * @param  p     输出缓冲区
  * @param  v     数据
  * @param  scale 放大倍数：1000/100/10
  * @param  digit 小数位数：3/2/1
  */
static void FmtFloat(char *p, float v, int32_t scale, int digit)
{
  int32_t t = (int32_t)(v * (float)scale);
  int32_t a = (t < 0) ? -t : t;
  char    s = (t < 0) ? '-' : '+';

  if (digit == 3)
  {
    sprintf(p, "%c%ld.%03ld", s, (long)(a / 1000), (long)(a % 1000));
  }
  else if (digit == 2)
  {
    sprintf(p, "%c%ld.%02ld", s, (long)(a / 100), (long)(a % 100));
  }
  else
  {
    sprintf(p, "%c%ld.%01ld", s, (long)(a / 10), (long)(a % 10));
  }
}

/**
  * @brief  无符号整数转字符串（不用 %u，兼容 microlib）
  */
static void FmtUInt(char *p, uint32_t v)
{
  char    tmp[12];
  uint8_t n = 0;

  if (v == 0U)
  {
    *p++ = '0';
    *p   = 0;
    return;
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
  *p = 0;
}

/**
  * @brief  4 位十六进制转字符串（显示 USB 的 VID/PID 用，不用 %X，兼容 microlib）
  * @note   例：0x7523 -> "7523"
  */
static void FmtHex4(char *p, uint16_t v)
{
  const char *hex = "0123456789ABCDEF";
  int8_t i;

  for (i = 3; i >= 0; i--)
  {
    p[i] = hex[v & 0x0FU];
    v = (uint16_t)(v >> 4);
  }
  p[4] = 0;
}

/**
  * @brief  有符号整数转字符串（编码器计数用）
  */
static void FmtInt(char *p, int32_t v)
{
  char     tmp[12];
  uint8_t  n = 0;
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

  if (a == 0U)
  {
    *p++ = '0';
    *p = 0;
    return;
  }

  while (a > 0U)
  {
    tmp[n++] = (char)('0' + (a % 10U));
    a /= 10U;
  }
  while (n > 0U)
  {
    *p++ = tmp[--n];
  }
  *p = 0;
}

/**
  * @brief  2 位十六进制转字符串（显示 0 号扇区头字节用；FmtHex4 只在 USB 版编译，所以这里单独写一个）
  * @note   例：0x3C -> "3C"
  */
static void FmtHex2(char *p, uint8_t v)
{
  const char *hex = "0123456789ABCDEF";

  p[0] = hex[(v >> 4) & 0x0FU];
  p[1] = hex[v & 0x0FU];
  p[2] = 0;
}

/**
  * @brief  把字符串右侧补空格到固定长度（擦掉上一帧残留的字符）
  */
static void LCD_PadText(char *p, uint16_t chars)
{
  uint16_t n = 0;

  while ((p[n] != 0) && (n < (chars - 1)))
  {
    n++;
  }
  while (n < chars)
  {
    p[n++] = ' ';
  }
  p[chars] = 0;
}

/**
  * @brief  整行擦掉后显示一行中文状态（屏幕最底下那行）
  */
static void LCD_ShowStatus(const char *text)
{
  if (s_hasStatus == 0)
  {
    return;
  }
  LCD_TextFillRect(0, s_statusY, LCD_TextCanvasWidth(), LCD_TextHeight(), LCD_GetBackColor());
  LCD_ShowCNString(0, s_statusY, text);
}

/**
  * @brief  标题行（最上面一行）：用来显示当前记录的 CSV 文件名
  */
static void LCD_ShowTitle(const char *text)
{
  if (s_hasTitle == 0)
  {
    return;
  }
  LCD_TextFillRect(0, s_titleY, LCD_TextCanvasWidth(), LCD_TextHeight(), LCD_GetBackColor());
  LCD_ShowCNString(0, s_titleY, text);
}

/**
  * @brief  信息行（标题下面那一行，只有 IMU 走 USB 时才用）：
  *         显示 USB 状态（等待设备/正在查找设备/CDC 正常/设备已断开）和 RX 计数
  */
static void LCD_ShowInfo(const char *text)
{
  if (s_hasInfo == 0)
  {
    return;
  }
  LCD_TextFillRect(0, s_infoY, LCD_TextCanvasWidth(), LCD_TextHeight(), LCD_GetBackColor());
  LCD_ShowCNString(0, s_infoY, text);
}

/**
  * @brief  没在记录时，把“SD 卡卡在哪一步”的原因显示在底部状态行
  *
  *   显示规则（配合 User/sd/imu_sdlog.c 的 IMULog_GetFailStage/Code）：
  *     "SD 认不到卡 n"        认卡（SD_Init）就失败：没插好/接触不良/卡坏/没供电/总线被占（板载 WIFI）
  *                             n = HAL_SD 错误码：4 = 命令没响应（卡没应答，最常见）
  *     "SD 卡不是 FAT32"      卡认到了、0 号扇区也读到了，但挂载失败（FR_NO_FILESYSTEM=13，
  *                             多数是卡被格成了 NTFS/GPT 分区，或不是 FAT/exFAT）
  *     "SD 读卡失败 n"        认卡成功但读扇区失败：n = 百位(步骤) + 后两位(HAL 错误码)，
  *                             例 202 = 数据阶段数据 CRC 错，204 = 数据阶段命令响应超时
  *     "SD 写入失败 n"        文件系统没问题，但建文件/写表头失败，n = FRESULT
  *     "无SD请检查连接"       还没试过 / 其它情况
  *   上面几条后面带 " 0:xxxxxx" = 认卡后裸读 0 号扇区收到的前 3 个字节（读失败时也显示）：
  *   EB3C90=FAT16/32、EB7690=exFAT、000000=全0、FFFFFF=全1(DAT0断/被拉载)、A5A5A5=没收到数据
  */
static void LCD_ShowSdStatus(void)
{
  static char s_sdBuf[40];
  char    num[12];
  char    hex[4];
  uint8_t stage = IMULog_GetFailStage();
  uint8_t code  = IMULog_GetFailCode();
  uint8_t raw[4];
  uint8_t i;

  if (stage == 1U)
  {
    strcpy(s_sdBuf, "SD 认不到卡 ");
    FmtUInt(num, code);
    strcat(s_sdBuf, num);
  }
  else if (stage == 2U)
  {
    if (code == 13U)
    {
      strcpy(s_sdBuf, "SD 卡不是 FAT32");
    }
    else
    {
      /* 1 线和 4 线都试过读 0 号扇区：把两个错误码都显示出来
         （例：SD 1B202 4B202 0:000002）*/
      strcpy(s_sdBuf, "SD 1B");
      FmtUInt(num, IMULog_GetBus1Code());
      strcat(s_sdBuf, num);
      strcat(s_sdBuf, " 4B");
      FmtUInt(num, IMULog_GetBus4Code());
      strcat(s_sdBuf, num);
    }

    /* 认卡成功后先裸读 0 号扇区：把读到的前 3 个字节附在后面，一眼看出是
       “卡是好的、只是文件系统不对”还是“根本没读到数据”：
         EB3C90 = FAT16/32   EB7690 = exFAT   EB5290 = NTFS   000000 = 空/坏扇区 */
    if (IMULog_GetRawSector0(raw) != 0U)
    {
      strcat(s_sdBuf, " 0:");
      for (i = 0U; i < 3U; i++)
      {
        FmtHex2(hex, raw[i]);
        strcat(s_sdBuf, hex);
      }
    }

    strcat(s_sdBuf, (IMULog_GetRawStable() != 0U) ? "=" : "*");   /* = 两次读数一样，* 不一样 */
  }
  else if (stage == 3U)
  {
    strcpy(s_sdBuf, "SD 写入失败 ");
    FmtUInt(num, code);
    strcat(s_sdBuf, num);
  }
  else
  {
    strcpy(s_sdBuf, "无SD请检查连接");
  }

  LCD_ShowStatus(s_sdBuf);
}

/**
  * @brief  初始化 LCD：MPU + Cache + LTDC + SDRAM 显存 + 背光 + 界面框架
  */
static void LCD_DisplayInit(void)
{
  uint16_t textH, rowsTotal, dataLines, dataRows, used, i, w;
  uint16_t slotCount, groupW, unitGap, avail;
  uint16_t maxLabelW = 0, maxUnitW = 0, asciiW;

  /* SDRAM 显存配成写透(write-through)：CPU 写的屏幕数据立即进 SDRAM，
     LTDC 才能取到最新画面，否则会花屏或画面不刷新 */
  Board_MPU_Config(0, MPU_Normal_WT, 0xD0000000, MPU_32MB);

  /* AXI SRAM（0x24000000，512KB）整块配成 non-cacheable —— 必须在下面 SCB_EnableDCache() 之前配好。
   *
   *   为什么要整块不缓存：
   *     * 本工程的 SD 缓冲区（User/sd/imu_sdlog.c 的 .sd_dma：FATFS / FIL / 行缓冲）
   *       和 USB 主机栈的缓冲区都落在 AXI SRAM 里；SDMMC 和 USB_OTG 都会自己搬数据
   *       （内部 DMA / FIFO），AXI SRAM 一旦可缓存就会出现“设备写进内存、CPU 从缓存读到
   *       旧数据”（或者反过来），必须额外做 Clean/Invalidate 维护；
   *     * 本工程数据量很小（10Hz 记录，屏幕显存在 SDRAM），不缓存对性能没有实际影响，
   *       换来的是“CPU 和 SDMMC / USB 永远看到同一份数据”。
   *
   *   以前是 SD_Init() 每次重试都把这块的头 32KB 改成 non-cacheable：那会在运行中反复
   *   HAL_MPU_Disable()/Enable()，而且连栈和全部全局变量一起卷进去。现在只在这里配一次。*/
  Board_MPU_Config(1, MPU_Normal_NonCache, 0x24000000, MPU_512KB);

  SCB_EnableICache();
  SCB_EnableDCache();

  /* LCD_Init() 内部已完成：LTDC 时钟(PLL3 -> 27MHz)、LTDC 引脚、SDRAM 初始化 */
  LCD_Init();

  LCD_LayerInit(0, LCD_FB_START_ADDRESS, RGB888);   /* 只用一层，RGB888 */
  LCD_DisplayOn();                                  /* 开 LTDC + 背光 */
  LCD_SelectLayer(0);

  LCD_SetFont(&Font24);                             /* ASCII 用 Font24，与 24x24 汉字同高 */
  LCD_SetColors(LCD_COLOR_WHITE, LCD_COLOR_BLACK);

  /* 字号 + 旋转（必须在画任何东西之前设置） */
  LCD_TextSetScale(LCD_UI_SCALE);
  LCD_TextSetRotation(LCD_UI_ROTATION);

  LCD_Clear(LCD_COLOR_BLACK);            /* DMA2D 硬件清屏（野火原生，已验证）*/

  /* ---------------- 自动排版 ---------------- */
  asciiW    = (uint16_t)(((sFONT *)LCD_GetFont())->Width * LCD_TextGetScale());
  textH     = LCD_TextHeight();
  rowsTotal = (uint16_t)(LCD_TextCanvasHeight() / textH);

  s_hasTitle  = (rowsTotal >= 6) ? 1 : 0;   /* 行数够就留一行给“文件名/标题” */
  s_hasStatus = (rowsTotal >= 4) ? 1 : 0;
  s_hasInfo   = 0;                            /* 不用第二行（USB 信息行），那一槽让给数据行 */
  used      = (uint16_t)(s_hasTitle + s_hasInfo + s_hasStatus);
  dataLines = (rowsTotal > used) ? (uint16_t)(rowsTotal - used) : 0;

  /* 收集真正要显示的行（表里空着的行自动跳过，方便以后加减行） */
  s_rowCount = 0;
  for (i = 0; i < LCD_ROW_MAX; i++)
  {
    if ((s_lcdRowTable[i].label != 0) && (s_lcdRowTable[i].label[0] != 0))
    {
      s_rowIndex[s_rowCount] = i;
      s_rowCount++;
    }
  }

  for (i = 0; i < s_rowCount; i++)
  {
    w = CN_StrWidth(s_lcdRowTable[s_rowIndex[i]].label);
    if (w > maxLabelW) { maxLabelW = w; }
    w = CN_StrWidth(s_lcdRowTable[s_rowIndex[i]].unit);
    if (w > maxUnitW)  { maxUnitW  = w; }
  }

  /* 一行放不下“标签 + 数值 + 单位”时（竖屏或大字号），自动改成标签一行、数值一行 */
  if (((uint32_t)maxLabelW + ((uint32_t)2 + LCD_VALUE_CHARS) * asciiW + maxUnitW + asciiW) >
      (uint32_t)LCD_TextCanvasWidth())
  {
    s_twoLine = 1;
  }
  s_rowSpan = (s_twoLine != 0) ? 2 : 1;

  dataRows = (uint16_t)(dataLines / s_rowSpan);
  if (dataRows > s_rowCount)
  {
    dataRows = s_rowCount;
  }
  s_rowShown = dataRows;

  /* ---------------- 铺满整屏：标题 / 数据行 / 状态行 平均分配到整个画布高度 ---------------- */
  slotCount = (uint16_t)(s_rowShown + s_hasTitle + s_hasInfo + s_hasStatus);
  if (slotCount == 0U)
  {
    slotCount = 1U;
  }
  s_slotH = (uint16_t)(LCD_TextCanvasHeight() / slotCount);

  /* 字号太大、一行放不下时少显示几行，保证绝不重叠 */
  while ((s_rowShown > 0U) && (s_slotH < (uint16_t)(s_rowSpan * textH)))
  {
    s_rowShown--;
    slotCount = (uint16_t)(s_rowShown + s_hasTitle + s_hasInfo + s_hasStatus);
    s_slotH = (uint16_t)(LCD_TextCanvasHeight() / slotCount);
  }

  s_dataPad   = (uint16_t)((s_slotH - (uint16_t)(s_rowSpan * textH)) / 2);   /* 数据行在自己槽里垂直居中 */
  s_textPad   = (uint16_t)((s_slotH - textH) / 2);                          /* 标题/状态行同上 */
  s_dataSlot0 = (uint16_t)(s_hasTitle + s_hasInfo);                         /* 标题（+USB 信息行）占前面几槽，数据从下一槽开始 */
  s_titleY    = s_textPad;
  s_infoY     = (uint16_t)(s_slotH + s_textPad);                            /* USB 信息行：标题的下一槽 */
  s_statusY   = (uint16_t)((slotCount - 1) * s_slotH + s_textPad);           /* 状态行在最后一槽（屏幕底部） */

  /* ---------------- 横向：把“数值 + 单位”整组推到屏幕右边，画面也占满宽度 ---------------- */
  s_valueChars = LCD_VALUE_CHARS;                                           /* 先按最大宽度算 */
  /* 单位列占宽：单行式 = 1 个空格 + 单位宽；两行式 = 单位宽（紧跟在数值后面） */
  unitGap      = (maxUnitW != 0U) ? (uint16_t)(maxUnitW + ((s_twoLine != 0) ? 0U : asciiW)) : 0U;
  groupW       = (uint16_t)(s_valueChars * asciiW + unitGap);

  if (s_twoLine != 0)
  {
    s_valueX = (uint16_t)(2 * asciiW);                                      /* 两行式：数值缩进在标签下面 */
  }
  else if (((uint32_t)maxLabelW + asciiW + groupW) <= (uint32_t)LCD_TextCanvasWidth())
{
    s_valueX = (uint16_t)(LCD_TextCanvasWidth() - groupW);                  /* 标签靠左、数值靠右 */
  }
  else
  {
    s_valueX = (uint16_t)(maxLabelW + 2 * asciiW);                          /* 空间不够就紧挨标签 */
  }

  /* 数值列实际能用几个字符（不够就自动收窄，不至于切掉半个字） */
  avail = ((uint32_t)LCD_TextCanvasWidth() > ((uint32_t)s_valueX + unitGap)) ? (uint16_t)(LCD_TextCanvasWidth() - s_valueX - unitGap) : 0U;
  s_valueChars = (uint16_t)(avail / asciiW);
  if (s_valueChars > LCD_VALUE_CHARS) { s_valueChars = LCD_VALUE_CHARS; }
  if (s_valueChars < 4U)              { s_valueChars = 4U; }

  s_unitX  = (uint16_t)(s_valueX + ((s_twoLine != 0) ? s_valueChars : (s_valueChars + 1)) * asciiW);
  s_showUnit = (((uint32_t)s_unitX + maxUnitW) <= (uint32_t)LCD_TextCanvasWidth()) ? 1 : 0;
  if (maxUnitW == 0U)
  {
    s_showUnit = 0;
  }

  /* ---------------- 静态内容只画一次：标题 / 标签 / 单位 ---------------- */
  if (s_hasTitle != 0)
  {
    LCD_ShowCNString(0, s_titleY, LCD_UI_TITLE_TEXT);
  }
  for (i = 0; i < s_rowShown; i++)
  {
    uint16_t y = (uint16_t)((s_dataSlot0 + i) * s_slotH + s_dataPad);

    uint16_t r = s_rowIndex[i];

    LCD_ShowCNString(0, y, s_lcdRowTable[r].label);
    if (s_showUnit != 0)
    {
      LCD_ShowCNString(s_unitX, (uint16_t)(y + ((s_twoLine != 0) ? textH : 0)), s_lcdRowTable[r].unit);
    }
  }
}

/**
  * @brief  把 IMU 数据刷到 LCD（100ms 一次，只读 sReg[]，不影响串口收发）
  */
static void LCD_UpdateImu(void)
{
  static uint32_t s_lcd_tick = 0;
  static char     s_lastVal[LCD_ROW_MAX][10];   /* 上一帧画过的数值文本 */
  static uint8_t  s_hasVal[LCD_ROW_MAX];        /* 1 = 这一行已经画过 */
  uint16_t i, y, r;

  if ((HAL_GetTick() - s_lcd_tick) < 50U)     /* 50ms 刷一次数值列，和模块 20Hz 对齐 */
  {
    return;
  }
  s_lcd_tick = HAL_GetTick();

  for (i = 0; i < s_rowShown; i++)
  {
    r = s_rowIndex[i];
    switch (s_lcdRowTable[r].reg)
    {
      case 0xF0:   /* 编码器累计计数（带符号整数）*/
        FmtInt(s_textBuf, Encoder_GetCount());
        break;

      case 0xF1:   /* 编码器角度（自 Z 零位起，0~360 度）*/
        FmtFloat(s_textBuf, Encoder_GetAngleDeg(), 100, 2);
        break;

      case 0xF2:   /* 编码器转速（转/分）*/
        FmtInt(s_textBuf, (int32_t)Encoder_GetSpeedRpm());
        break;

      case 0xF3:   /* 编码器位置（自 Z 零位起的计数，0~10000）*/
        FmtInt(s_textBuf, Encoder_GetPos());
        break;

      case 0xF4:   /* 收到的 Z 脉冲个数（每圈一个）*/
        FmtInt(s_textBuf, Encoder_GetZCount());
        break;

      case 0xFA:   /* 力值：去皮后的净重，kg，2 位小数 */
      {
        int32_t g = g_forceG;

        if (g < 0) { g = 0; }
        sprintf(s_textBuf, "%ld.%02ld", (long)(g / 1000), (long)((g % 1000) / 10));
        break;
      }

      case 0xFB:   /* 力传感器原始 ADC 码值（0~65535，已经是 64 次平均后的值）*/
        FmtInt(s_textBuf, g_forceAdc);
        break;

      case 0xFC:   /* 去皮零点（码值）*/
        FmtInt(s_textBuf, g_forceZero);
        break;

      case 0xF5:   /* 每圈计数：固定显示固件实际使用的值（角度就是按它算的）*/
        FmtInt(s_textBuf, (int32_t)ENCODER_CPR);
        break;

      case 0xF6:   /* A 相电平（仅引脚观测模式有意义）*/
        FmtInt(s_textBuf, (int32_t)(Encoder_ReadPins() & 0x01U));
        break;

      case 0xF7:   /* B 相电平 */
        FmtInt(s_textBuf, (int32_t)((Encoder_ReadPins() >> 1) & 0x01U));
        break;

      case 0xF8:   /* Z 相电平 */
        FmtInt(s_textBuf, (int32_t)((Encoder_ReadPins() >> 2) & 0x01U));
        break;

      default:     /* 其它行还是取 sReg[] 里的 IMU 数据 */
        FmtFloat(s_textBuf, (float)sReg[s_lcdRowTable[r].reg] * s_lcdRowTable[r].k,
                 s_lcdRowTable[r].mul, s_lcdRowTable[r].digit);
        break;
    }
    LCD_PadText(s_textBuf, s_valueChars);

    /* 只有文本变了才重画这一行：屏幕上有十几行，如果每 50ms 全部重画一遍，
       光往 SDRAM 写像素就要吃掉不少 CPU，会挤压主循环（看起来更“钝”）。*/
    if ((s_hasVal[i] == 0U) || (g_lcdForceRedraw != 0U) ||
          (strcmp(s_lastVal[i], s_textBuf) != 0))
    {
      uint8_t k;

      for (k = 0U; (k < (sizeof(s_lastVal[0]) - 1U)) && (s_textBuf[k] != '\0'); k++)
      {
        s_lastVal[i][k] = s_textBuf[k];
      }
      s_lastVal[i][k] = '\0';
      s_hasVal[i]     = 1U;

      y = (uint16_t)((s_dataSlot0 + i) * s_slotH + s_dataPad + ((s_twoLine != 0) ? LCD_TextHeight() : 0));
      LCD_ShowCNString(s_valueX, y, s_textBuf);
    }
  }

  g_lcdForceRedraw = 0U;   /* 本轮已经整幅重画过 */
}

/**
  * @brief  USB 版：刷新“USB 信息行”（标题下面那一行）和底部状态行
  *
  *   信息行（USB 状态 + 收到的字节数）：
  *     "USB 正在连接"        正在初始化主机栈、准备当主机
  *     "USB 等待设备"        主机就绪，等模块插到 USB HOST 口
  *     "USB 正在查找设备"    模块插上了，正在枚举 / 等 CDC 类
  *     "USB CDC 正常 RX n"   类就绪、正在收数据（n = 收到的字节数）
  *     "USB 设备已断开"      刚才还在，现在被拔了
  *     "USB 连接错误"        主机栈初始化失败
  *
  *   状态行（屏幕最底下）和串口版一样：已存 N 帧 / 无SD请检查连接 / SD读写错误
  *   标题行（最上面）：记录中显示 CSV 文件名，其它情况显示模块的 VID:PID
  */
static void LCD_UpdateUsbStatus(void)
{
  static uint8_t  s_lastUsbSt  = 0xFFU;
  static uint8_t  s_lastLogSt  = 0xFFU;
  static uint32_t s_lastRx     = 0xFFFFFFFFU;
  static uint32_t s_lastRxTick = 0U;
  static uint8_t  s_lastSdCode = 0xFFU;
  static uint16_t s_lastVid    = 0xFFFFU;
  static uint16_t s_lastPid    = 0xFFFFU;
  static char     s_buf[40];
  char    num[12];
  char    hex[8];
  uint8_t usbSt;
  uint8_t st;

  usbSt = (uint8_t)USB_Host_GetState();
  st    = IMULog_GetState();

  /* ---- 帧率 / 数据龄 / 最大帧间隔：用来判断“延迟”到底出在哪一环 ----
   *   g_fps      : 模块实际回传帧率（设了 20Hz 就应该是 20 左右）
   *   g_ageMs    : 画屏时最新数据已经多旧（理想值 ≤ 一帧的时间，20Hz 时约 50ms）
   *   g_gapMaxMs : 上一秒内相邻两帧的最大间隔（USB/主循环有没有“卡一下”）*/
  {
    static uint32_t s_statTick   = 0U;
    static uint32_t s_statFrames = 0U;
    static uint32_t s_lastSeen   = 0U;
    static uint32_t s_gapMax     = 0U;
    static uint32_t s_statRx     = 0U;
    uint32_t now = HAL_GetTick();
    uint32_t lf  = g_lastFrame;

    if (lf != s_lastSeen)
    {
      if ((s_lastSeen != 0U) && ((lf - s_lastSeen) > s_gapMax))
      {
        s_gapMax = lf - s_lastSeen;
      }
      s_lastSeen = lf;
    }

    if ((now - s_statTick) >= 1000U)
    {
      if (s_statTick != 0U)
      {
        g_fps      = (uint32_t)((g_frames - s_statFrames) * 1000U / (now - s_statTick));
        g_rxBps    = (uint32_t)((g_rxBytes - s_statRx) * 1000U / (now - s_statTick));
        g_gapMaxMs = s_gapMax;
      }
      s_statFrames = g_frames;
      s_statRx     = g_rxBytes;
      s_statTick   = now;
      s_gapMax     = 0U;
    }

    g_ageMs = (lf != 0U) ? (now - lf) : 0U;
  }

  /* ---------------- USB 信息行 ---------------- */
  if ((s_hasInfo != 0) &&
      ((usbSt != s_lastUsbSt) ||
       ((g_rxBytes != s_lastRx) && ((HAL_GetTick() - s_lastRxTick) >= 200U))))
  {
    s_lastUsbSt  = usbSt;
    s_lastRx     = g_rxBytes;
    s_lastRxTick = HAL_GetTick();

    switch (usbSt)
    {
      case USB_HOST_STATE_OFF:
      case USB_HOST_STATE_INIT:
        strcpy(s_buf, "USB 正在连接");
        break;

      case USB_HOST_STATE_WAITING:
        strcpy(s_buf, "USB 等待设备");
        break;

      case USB_HOST_STATE_ENUMERATING:
        strcpy(s_buf, "USB 正在查找设备");
        break;

      case USB_HOST_STATE_READY:
        /* 例："7/s 235B 134ms R6 C14"
           = 每秒解析到 7 帧角度、每秒收到 235 字节、画屏时数据 134ms 旧、
             模块里回读到的 回传率寄存器=6(0x06=10Hz,0x07=20Hz)、回传内容=14 */
        FmtUInt(num, g_fps);
        strcpy(s_buf, num);
        strcat(s_buf, "/s ");
        FmtUInt(num, g_rxBps);
        strcat(s_buf, num);
        strcat(s_buf, "B ");
        FmtUInt(num, g_ageMs);
        strcat(s_buf, num);
        strcat(s_buf, "ms R");
        FmtUInt(num, g_modRate);
        strcat(s_buf, num);
        strcat(s_buf, " C");
        FmtUInt(num, g_modRsw);
        strcat(s_buf, num);
        break;

      case USB_HOST_STATE_DISCONNECTED:
        strcpy(s_buf, "USB 设备已断开");
        break;

      default:
        strcpy(s_buf, "USB 连接错误");
        break;
    }

    LCD_ShowInfo(s_buf);
  }

  /* ---------------- 底部状态行 ----------------
   *   优先显示“刚刚发生的事件”，各显示 3 秒：
   *     USB 类就绪        -> USB 连接成功
   *     USB 设备被拔掉     -> USB 已断开
   *     SD 挂载成功开始记录 -> 读卡成功
   *   事件消息过期后：出错/没卡就显示原因，正常记录时那一行留空。*/
  {
    static char     s_shown[24];         /* 状态行现在画的是什么 */
    static char     s_msg[24];           /* 最近一条事件消息 */
    static uint32_t s_msgTick = 0U;      /* 事件发生时刻 */
    static uint8_t  s_prevUsb = 0xFFU;
    static uint8_t  s_prevLog = 0xFFU;
    static uint8_t  s_shownSdErr = 0U;
    char     want[24];
    uint32_t now = HAL_GetTick();

    /* ---- 事件检测（只在状态“变化”的那一刻记一条消息）---- */
    if ((usbSt == USB_HOST_STATE_READY) && (s_prevUsb != USB_HOST_STATE_READY))
    {
      strcpy(s_msg, "USB 连接成功");
      s_msgTick = now;
    }
    else if ((usbSt == USB_HOST_STATE_DISCONNECTED) && (s_prevUsb != USB_HOST_STATE_DISCONNECTED))
    {
      strcpy(s_msg, "USB 已断开");
      s_msgTick = now;
    }
    s_prevUsb = usbSt;

    if ((st == IMULOG_STATE_LOGGING) && (s_prevLog != IMULOG_STATE_LOGGING))
    {
      strcpy(s_msg, "读卡成功");
      s_msgTick = now;
    }
    s_prevLog = st;

    /* ---- 决定这一行显示什么 ---- */
    want[0] = '\0';
    if ((s_msgTick != 0U) && ((now - s_msgTick) < 3000U))
    {
      strcpy(want, s_msg);               /* 最近 3 秒：显示事件消息 */
    }
    else if (st == IMULOG_STATE_ERROR)
    {
      strcpy(want, "SD读写错误");
    }
    else if (st != IMULOG_STATE_LOGGING)
    {
      /* 没卡 / 认不到卡 / 读卡失败 / 写入失败：沿用原来的分步原因 */
      if ((st != s_lastLogSt) || (IMULog_GetFailCode() != s_lastSdCode) || (s_shownSdErr == 0U))
      {
        s_lastLogSt  = st;
        s_lastSdCode = IMULog_GetFailCode();
        LCD_ShowSdStatus();
        s_shownSdErr = 1U;
        s_shown[0]   = '\0';
      }
      want[0] = 0x01;                    /* 标记“这一轮不用管 want” */
    }

    if ((want[0] != 0x01) && (s_shownSdErr != 0U))
    {
      s_shownSdErr = 0U;                 /* 从“出错原因”切回正常内容 */
      s_shown[0]   = 0x01;               /* 强制重画一次 */
    }

    if ((want[0] != 0x01) && (strcmp(s_shown, want) != 0))
    {
      strcpy(s_shown, want);
      LCD_ShowStatus(want);              /* want 可能是空串：把这一行清空 */
    }
  }

  /* ---------------- 标题行：文件名 / VID:PID ---------------- */
  if (st == IMULOG_STATE_LOGGING)
  {
    static char s_lastTitle[20];
    const char *fn = IMULog_GetFileName();

    if (strcmp(s_lastTitle, fn) != 0)      /* 只在文件名变化时重画（原来每圈都整行重画，很吃 CPU）*/
    {
      strcpy(s_lastTitle, fn);
      LCD_ShowTitle(fn);
    }
  }
  else if ((USB_Host_IsConnected() != 0U) &&
           ((USB_Host_GetVid() != s_lastVid) || (USB_Host_GetPid() != s_lastPid)))
  {
    s_lastVid = USB_Host_GetVid();
    s_lastPid = USB_Host_GetPid();

    FmtHex4(hex, s_lastVid);
    strcpy(s_buf, hex);
    strcat(s_buf, ":");
    FmtHex4(hex, s_lastPid);
    strcat(s_buf, hex);
    LCD_ShowTitle(s_buf);
  }
  else
  {
    /* 还没枚举出来 / 已经在显示文件名：标题保持原样 */
  }
}

void Error_Handler(void)
{
  __disable_irq();
  while (1)
  {
  }
}

#ifdef  USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
  (void)file;
  (void)line;
}
#endif

/************************ (C) COPYRIGHT *****END OF FILE************************/
