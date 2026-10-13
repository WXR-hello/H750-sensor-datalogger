# LCD 显示功能移植说明（firmware_witsdk）

> **注意（后续版本已改动）**：本文件里的串口描述（USART1 上报电脑 / UART4 接模块）是旧版的。
> 现在 IMU 接 **USART1（PA9/PA10）**、UART4 已删除，数据记录到 **SD 卡**（LCD 的标题行显示
> 记录文件名，最底下一行显示记录状态）。
> 请看 **[README_SD卡记录说明.md](README_SD卡记录说明.md)**。

把野火 `28-电容触摸屏—触摸画板（兼容屏幕版本）` 例程里的 **LCD 显示功能** 移植到本工程，
**本工程原有的 IMU 采集 / 串口功能一行未改**。

## 一、移植了什么，没移植什么

移植（只有显示相关）：

* LTDC 显示驱动（`User/lcd/bsp_lcd.c/.h`）
* DMA2D 图形加速（清屏填充用）
* SDRAM（FMC）显存驱动（`User/sdram/bsp_sdram.c/.h`）
* MPU + Cache 配置（`User/mpu/bsp_mpu.c/.h`）
* 字库（`User/fonts/`：font8/12/16/20/24）

**没有**移植：电容触摸（`bsp_i2c_touch.c`、`bsp_touch_gtxx.c`）、调色板/画板（`palette.c`）。
这些文件在本工程里根本不存在，也不会被编译。

原有功能保持原样：IWT603 采集、`wit_c_sdk`、USART1 上报（文字 / 46 字节二进制帧）、
UART4 命令（`a m e R r B b C c h`）、`AutoScanSensor()` 自动扫波特率 —— 全部和原来一致。

## 二、新增 / 修改的文件

新增目录 `firmware_witsdk/User/`（源码从野火例程原样拷入）：

```
User/lcd/bsp_lcd.c  bsp_lcd.h
User/sdram/bsp_sdram.c  bsp_sdram.h
User/mpu/bsp_mpu.c  bsp_mpu.h
User/fonts/font8.c font12.c font16.c font20.c font24.c fonts.h
```

修改的原有文件（3 个）：

| 文件 | 改动 |
| --- | --- |
| `Src/main.c` | 只做“插入”：新增 `LCD_DisplayInit()`、`LCD_UpdateImu()` 及调用，原代码一行没删没改 |
| `Inc/stm32h7xx_hal_conf.h` | 打开 `HAL_LTDC_MODULE_ENABLED`、`HAL_DMA2D_MODULE_ENABLED`、`HAL_SDRAM_MODULE_ENABLED`、`HAL_MDMA_MODULE_ENABLED` |
| `MDK-ARM/Project.uvprojx` | 新增分组 `User/LCD`；HAL 组加入 `stm32h7xx_hal_ltdc.c`、`stm32h7xx_hal_dma2d.c`、`stm32h7xx_hal_sdram.c`、`stm32h7xx_hal_mdma.c`、`stm32h7xx_ll_fmc.c`；头文件路径加入 `..\User` |

## 三、为兼容本工程的新版 HAL，驱动里改了 4 处宏名

| 原（野火老版 HAL） | 现（本工程 HAL） | 位置 |
| --- | --- | --- |
| `__GPIOx_CLK_ENABLE()` | `__HAL_RCC_GPIOx_CLK_ENABLE()` | `User/lcd/bsp_lcd.h` |
| `__HAL_LTDC_RELOAD_CONFIG()` | `__HAL_LTDC_RELOAD_IMMEDIATE_CONFIG()` | `User/lcd/bsp_lcd.c` |
| `__FMC_CLK_ENABLE()` | `__HAL_RCC_FMC_CLK_ENABLE()` | `User/sdram/bsp_sdram.c` |
| `FMC_Bank2_SDRAM` | `FMC_SDRAM_BANK2` | `User/sdram/bsp_sdram.h` |

其余代码（含中文注释、GBK 编码）保持原样。

## 四、屏幕型号怎么选

打开 `User/lcd/bsp_lcd.c`，改这一行（默认 5 寸）：

```c
LCD_TypeDef cur_lcd = INCH_5;     /* 5 寸 */
/* LCD_TypeDef cur_lcd = INCH_7;     7 寸 */
/* LCD_TypeDef cur_lcd = INCH_4_3;   4.3 寸 */
```

三种屏幕的行场参数、分辨率都在同文件的 `lcd_param[]` 表里，不用改别的地方。

## 五、编译结果

Keil 打开 `MDK-ARM/Project.uvprojx`，直接 Build（本工程是 microlib）：

```
Program Size: Code=36664  RO-data=16924  RW-data=124  ZI-data=2572
"Objects\H750_UART.axf" - 0 Error(s), 0 Warning(s).
```

## 六、屏幕显示什么

| 行 | 内容 |
| --- | --- |
| 0 | 标题 `IWT603 IMU + STM32H750XB` |
| 2 / 3 / 4 | Roll / Pitch / Yaw（角度，±180.00°） |
| 6 | 三轴加速度（g） |
| 7 | 三轴角速度（°/s） |
| 8 | 温度（℃） |
| 10 / 11 / 12 | USART1 / UART4 / LCD 配置信息 |

数据 100 ms 刷新一次；显示只读 `sReg[]`，不占用串口，不影响 10Hz 上报和电脑命令。
数值格式化没有使用 `%f`（`FmtFloat()` 自己拼字符串），兼容 microlib。

## 七、硬件前提

* 野火 STM32H750_V_Pro 开发板 + 板载 SDRAM（显存，地址 `0xD0000000`）
* 液晶屏接在板子的 RGB(LTDC) 接口上；背光为 PD13
* 显示屏会用到 PI/PJ/PK 大部分引脚和 PD13，本工程原功能用的是 PA9/PA10（USART1）、
  PC10/PC11（UART4），互不冲突
* 初始化顺序：`SystemClock_Config()` → `LCD_DisplayInit()`（内含 PLL3→27MHz、LTDC 引脚、
  SDRAM 初始化、开背光）→ 串口初始化，和原来一样接着跑

## 八、中文显示 / 调大字体 / 屏幕转向（后加的功能）

### 8.1 中文显示是怎么做的

野火例程里的中文是从**外部 QSPI Flash 或 SD 卡里的 GB2312 字库**取模的，本工程没有这些外部存储器，
所以改成：**把用到的汉字提前取模成 24x24 点阵，直接放进内部 Flash**，按 GBK 编码查表显示。
好处是不依赖任何外部存储、不占 QSPI/SD 驱动；代价是字表里只有你需要的那几十个字。

相关文件：

| 文件 | 作用 |
| --- | --- |
| `User/fonts/font_cn.c` | 中英混排显示、字号放大、屏幕旋转的实现 |
| `User/fonts/font_cn_data.c` | 144 个汉字的 24x24 点阵（脚本生成，别手改） |
| `User/fonts/make_cn_font.ps1` | 取模脚本（用 Windows 自带黑体渲染成点阵） |
| `User/fonts/cn_preview.png` | 点阵预览图，可直观检查字形 |

当前收录的字（144 个，覆盖界面标签、状态提示，以及工程里串口帮助文本用到的全部汉字）：`维特智能模块横滚俯仰航向加速度角温串口电脑液晶秒正在查找已到未请检连接静止校准数据显示中文字库无，：传感器采集样频率周期时缓存储读写上报输出入波特命令帮助正常异等待成功失败断开开始停止保存清除复位陀螺磁场四元数欧拉偏姿态原始值换算标定单位通讯完成错误设备地址寄存器号位组包帧头校验→全内减只回增大容宽小带束植版移结`

**要增加汉字**：改 `make_cn_font.ps1` 里的 `-Chars`，然后

```powershell
pwsh -File User/fonts/make_cn_font.ps1
```

再重新编译即可（脚本会同时更新 `font_cn_data.c` 和预览图；main.c 里用不到的字会被自动忽略，不会“乱码”）。

**想换字形或字更大**（比如宋体 32x32）：

```powershell
pwsh -File User/fonts/make_cn_font.ps1 -FontName SimSun -Cell 32
```

然后把 `User/fonts/font_cn.h` 里三个宏改成对应值（32x32、每行 4 字节）：

```c
#define CN_FONT_WIDTH          ((uint16_t)32)
#define CN_FONT_HEIGHT         ((uint16_t)32)
#define CN_FONT_BYTES_PER_ROW  ((uint16_t)4)
```

### 8.2 调大字体

`Src/main.c` 里 LCD 部分最上面有两行开关：

```c
#define LCD_UI_SCALE      2      /* 字号放大倍数：1 / 2 / 3 / 4 */
#define LCD_UI_ROTATION   0      /* 屏幕旋转：0 / 90 / 180 / 270 */
```

* 放大是**整倍数**，汉字和英文一起放大（行高 = 24 x 放大倍数）。
* 字号调大后每行更高、能放下的行数变少，程序会**自动按优先级少显示几行**：
  横滚/俯仰/航向 → 加速度 XYZ → 角速度 XYZ → 温度；实在放不下时连标题/状态行也会省掉。
* 一行宽度放不下时（竖屏或大字号），会自动改成**上面一行标签、下面一行数值**。
* 数值列宽度也会按可用宽度自动收窄，尽量不被切字。

实测各种组合能显示多少（16:9 的 800x480 屏）：

| 放大 | 旋转 | 逻辑画布 | 数据行 | 排版 | 单位列 |
| --- | --- | --- | --- | --- | --- |
| 1 | 0 / 180 | 800x480 | 10 行（全部） | 单行式 | 显示 |
| 2 | 0 / 180 | 800x480 | 8 行（角度+加速度+角速度XY） | 单行式 | 显示 |
| 1 | 90 / 270 | 480x800 | 10 行（全部） | 单行式 | 显示 |
| 2 | 90 / 270 | 480x800 | 7 行 | 两行式 | 显示 |
| 3 | 90 / 270 | 480x800 | 4 行 | 两行式 | 隐藏 |
| 3 | 0 / 180 | 800x480 | 2 行 | 两行式 | 显示 |

推荐：**横屏用 1~2，竖屏用 1~3**。想要“又大又清晰”，优先用 8.1 里 `-Cell 32` 重新取模，
而不是把 24x24 一直放大（放大超过 2 倍边缘会显得方）。

### 8.3 屏幕转向

```c
#define LCD_UI_ROTATION   0      /* 0 / 90 / 180 / 270 */
```

* 旋转**只改变画面内容的方向**：LTDC 时序始终是 800x480，屏幕接线、时序参数都不用动。
* `0` / `180` 是横屏（180 用于屏幕装倒了）；`90` / `270` 是竖屏，逻辑画布变成 480x800。
* 如果转过去方向反了（上下颠倒），把 `90` 和 `270` 对调即可。
* 实现方式是在写像素时做坐标变换（软件旋转），分辨率很高时也只是每帧多几十万次写像素，不影响 IMU 采集和串口。

### 8.4 数据行怎么增减（LCD_ROW_MAX）

`Src/main.c` 里：

```c
#define LCD_UI_TITLE_TEXT ""      /* 填 ""维特智能 IMU 模块"" 就显示标题；填 "" 不显示、也不占行 */
#define LCD_ROW_MAX       15      /* 行表最多能放几行（当前正好填了 15 行） */
```

* `LCD_ROW_MAX` 是**行表数组的长度**，必须 ≥ 你实际写的行数。
* **表里空着的行会自动跳过**（代码按“label 非空”筛一遍），所以把 LCD_ROW_MAX 改大而暂时不填新行，也不会死机；
  反过来如果 LCD_ROW_MAX 比实际行数小，编译会报 `too many initializers`。
* 一行数据的格式：

```c
  { "标签", "单位", 寄存器, 换算系数k, 显示倍数, 小数位 },
  { "磁场 X：", "",   HX,     1.0f,        1,   0 },      /* 物理量 = sReg[寄存器] * k，再乘显示倍数取小数 */
```

* 可用寄存器（见 `Inc/REG.h`）：`AX/AY/AZ`(0x34~36)、`GX/GY/GZ`(0x37~39)、`HX/HY/HZ`(0x3A~3C)、
  `Roll/Pitch/Yaw`(0x3D~3F)、`TEMP`(0x40)、`q0~q3`(0x51~54)、`BATVAL`(0x5C)、`VERSION`(0x2E)、
  `YYMM/DDHH/MMSS/MS`(0x30~33) 等。
* **能显示多少行还受屏幕高度限制**：`显示行数 = 画布高度 / 行高`（行高 = 24 × `LCD_UI_SCALE`）。
  例如 1 倍字号竖屏(480x800)能放 33 行，2 倍字号横屏(800x480)只能放 10 行 —— 这时把 LCD_ROW_MAX 调大也没用，
  会自动按表里的顺序截断（越靠前越优先显示）。
* 新行的标签如果用了字库里没有的汉字，那个字会显示为空白；用 `make_cn_font.ps1` 重新取模即可。

当前 15 行：横滚 / 俯仰 / 航向 / 加速度 XYZ / 角速度 XYZ / 温度 / 磁场 XYZ / 电压 / 版本。
最后两行（电压 `BATVAL`、版本 `VERSION`）是模块附加信息，模块不支持时会显示 0，直接改这两行即可换成别的量。

### 8.5 让采集的数据正好铺满整屏

排版逻辑已经改成**平均铺满**：把「标题 + 数据行 + 状态行」当作同样高的槽位，`槽高 = 画布高度 / 总行数`，
每行的文字在自己槽里垂直居中。所以屏幕上有多少行，就会从上到下均匀铺满，不会挤在顶部留一大片空白。

横向也做了对齐：**标签靠左、数值和单位靠右**（数值列右推到屏幕右边缘），画面横向同样占满。

实测（15 行数据 + 1 行状态）：

| 放大 | 旋转 | 画布 | 能显示行数 | 排版 | 纵向占用 |
| --- | --- | --- | --- | --- | --- |
| 1 | 90 / 270 | 480x800 | 15 行（全部） | 单行式 | 13~737 / 800 |
| 1 | 0 / 180 | 800x480 | 15 行（全部） | 单行式 | 3~447 / 480 |
| 2 | 90 / 270 | 480x800 | 7 行 | 两行式 | 2~698 / 800 |
| 2 | 0 / 180 | 800x480 | 9 行 | 单行式 | 0~432 / 480 |
| 3 | 90 / 270 | 480x800 | 4 行 | 两行式 | 8~632 / 800 |

结论：**1 倍字号（任何方向）能一次显示全部 15 行并铺满整屏**；字号放大后能显示的行数变少，
但剩下的行仍然会均匀铺满屏幕（不会只占上半屏）。想让 15 行、字又更大，就把字号保持在 1 倍，
用 `-Cell 32` 重新取模汉字（点阵本身变大，而不是放大 24x24），效果比直接放大倍数更清晰。
