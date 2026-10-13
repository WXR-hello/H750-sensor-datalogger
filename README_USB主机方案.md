# IMU 走 USB（USB Host + CDC 虚拟串口）—— 实施说明

> 状态：**已实现并编译通过**（2026-09-25）
> 固件：`MDK-ARM/H750_IMU_USB.hex`（Code=63528 / RO=29160 / RW=272 字节，Flash 约 93KB / 128KB）
> 切换开关：`Inc/main.h` 里的 `IMU_LINK_USB` / `IMU_LINK_UART`（两个宏必须有一个且只有一个为 1）

## 0. 修复记录（重要，2026-09-25 晚）

**症状**：烧进去以后屏幕一直停在 `USB 等待设备`，插拔模块没有任何变化。

**原因（固件 bug，不是硬件）**：ST 这套老主机库的状态机在 `HOST_IDLE` 里只有看到
`phost->device.is_connected == 1` 才往下走，而这个标志只在库的 `USBH_LL_Connect()` 里被置 1；
同理 `PortEnabled` 只在 `USBH_LL_PortEnabled()` 里被置 1。
这几个 `USBH_LL_xxx` 事件函数由库自己实现、但**库不会主动调用**，必须由应用在 HAL 的
HCD 回调里桥接（ST 老版 `usbh_conf.c` 模板里就是那 5 个函数）。第一版漏了这层桥接，
所以：**设备插入事件传不到主机库 → 状态机永远停在 HOST_IDLE → 屏幕一直"等待设备"**。

**修复**：在 `User/usbhost/usbh_conf.c` 末尾补上
`HAL_HCD_Connect_Callback / Disconnect / PortEnabled / PortDisabled / SOF` 五个回调，
分别桥接到 `USBH_LL_Connect / Disconnect / PortEnabled / PortDisabled / IncTimer`，
并在 `USBH_LL_Init()` 里用 `USBH_LL_SetTimer(phost, HAL_HCD_GetCurrentFrame(&hhcd))` 对齐计时器。
（编译后的 map 里可以看到 HAL 的弱定义被移除、替换成 usbh_conf.o 里的实现。）

**顺带加的诊断**：`USB_Host_Process()` 现在还会直接读 OTG 的 `HPRT.PCSTS` 位，
只要 PHY 在 D+/D- 上看到了设备上拉，屏幕就会显示 `USB 正在查找设备`。
于是两种故障能分开：

| 屏幕 | 含义 |
| --- | --- |
| 一直 `USB 等待设备` | PHY 完全没看到设备 → 物理层问题（线/供电/主机口走线或跳线）|
| 卡在 `USB 正在查找设备` | PHY 看到了设备，问题在枚举/类驱动（VID:PID 会显示在标题行）|

## 一、目标（已达成）

IMU（IWT603）不再走串口，改成**把模块的 USB 接到板子的 USB HOST 口**，
由 STM32H750 当 USB 主机收数据；数据照样**显示在 LCD**、**记录到 SD 卡 CSV**。
也就是只换传输层（UART → USB），显示和记录层不动。

```
IWT603 的 Type-C ──（A 转 C 数据线）── 底板 USB-A（丝印 USB HOST）
板子供电：DC（6~12V）          模块供电：主机口给 5V（也可自供，注意共地）
```

## 二、硬件/软件事实（本次实测与查证）

| 项目 | 结论 |
| --- | --- |
| 模块 USB 类型 | 插电脑显示 **"USB 串行设备 (COM)"**（在"端口 (COM 和 LPT)"下）→ **标准 CDC 虚拟串口**，不是 CH340 私有类 |
| MCU 侧接口 | **USB_OTG_HS + 片内全速 PHY**：`PB14 = D-`、`PB15 = D+`（AF12），对应底板 USB HOST 口 |
| H7 必须做的事 | 片内 PHY 一定要开 **VDD33USB 电压检测器**（`HAL_PWREx_EnableUSBVoltageDetector()`），否则插上设备也不枚举 |
| USB 内核时钟 | 必须 **48MHz**。原来给 LTDC 的 PLL3 是 270MHz（除不出 48），已改成 **VCO=432MHz**：`PLL3R=432/16=27MHz` 给 LTDC（**和原来完全一样**）、`PLL3Q=432/9=48MHz` 给 USB |
| 库 | ST USB Host 库（2015 版，`USBH_Init` 三参数）+ CDC 类，已放进 `User/usbhost/` |
| 内存 | 工程用 MicroLIB、堆只有 512B → 主机库的 `USBH_malloc/free` 改成 `usbh_conf.c` 里的**静态内存池**，不碰堆 |
| 编译结果 | **0 Error / 0 Warning**；两条链路（USB 版 / 串口版）都能编译 |

## 三、代码结构

### 3.1 新增

| 文件 | 作用 |
| --- | --- |
| `User/usbhost/usbh_conf.h` | 主机库配置：端点/接口/类数量、描述符缓冲、日志宏、内存接口 |
| `User/usbhost/usbh_conf.c` | **HCD 粘合层**（`USBH_LL_Init/Start/Stop/ResetPort/OpenPipe/SubmitURB/GetURBState/...` 转成 `HAL_HCD_*`）+ 静态内存池 + `HCD_HandleTypeDef hhcd` |
| `User/usbhost/usb_host.c/.h` | 应用层：初始化、枚举、状态上报、CDC 收发；收到的字节通过回调交给 `main.c` 喂给 `wit_c_sdk` |
| `User/usbhost/usbh_core.c / usbh_ctlreq.c / usbh_ioreq.c / usbh_pipes.c / usbh_cdc.c` + 头文件 | ST USB Host 库（Core + CDC 类）|

### 3.2 修改

| 文件 | 改动 |
| --- | --- |
| `Inc/main.h` | 新增链路开关 `IMU_LINK_USB` / `IMU_LINK_UART`（含冲突检查 `#error`）|
| `Inc/stm32h7xx_hal_conf.h` | 把 `HAL_HCD_MODULE_ENABLED` 从注释块挪到有效区 |
| `Src/stm32h7xx_hal_msp.c` | 新增 `HAL_HCD_MspInit/DeInit`：OTG_HS 时钟 + 复位、USB 3.3V 检测器、**PB14/PB15 = AF12**、`OTG_HS_IRQn` |
| `Src/stm32h7xx_it.c` | 新增 `OTG_HS_IRQHandler()` → `HAL_HCD_IRQHandler(&hhcd)` |
| `User/lcd/bsp_lcd.c` | `LCD_ClockConfig()` 里 PLL3 由 270MHz 改成 432MHz（27MHz LTDC 不变，多出 48MHz 给 USB）|
| `Src/main.c` | 两条链路用 `#if IMU_LINK_USB` 分开：USB 版初始化主机、主循环跑 `USB_Host_Process()`、没数据时发"读加速度"探测；LCD 多一行 USB 状态；收到字节两条链路共用 `SensorRxByte()` |
| `MDK-ARM/Project.uvprojx` | 新增分组 `User/USB`（7 个 .c）、HAL 组加 `stm32h7xx_hal_hcd.c` / `stm32h7xx_ll_usb.c`、头文件路径加 `..\User\usbhost`、输出名改 `H750_IMU_USB` |
| `User/usbhost/usbh_cdc.c` | 本地改动：CDC 类请求里 `GET_LINE_CODING` 失败（有些虚拟串口设备不实现该请求、会 STALL）时不再把主机打进错误态，而是当作类可用继续跑 |

### 3.3 数据流

```
模块 USB → CDC 批量 IN 端点 → USBH_CDC_ReceiveCallback()
        → main.c 的 SensorUsbInput → SensorRxByte()
        → WitSerialDataIn() → sReg[] → LCD 数值 + SD 卡 CSV（IMU0001.CSV …）

给模块发指令：wit_c_sdk 的写回调 SensorUsbSend() → USB_Host_SendBytes()（发送队列）
        → USB_Host_Process() → CDC 批量 OUT 端点
```

## 四、屏幕上的显示

| 位置 | 显示内容 |
| --- | --- |
| 标题行 | 记录中显示 CSV 文件名（`IMU0001.CSV`）；否则显示模块 VID:PID（如 `1A86:7523`）|
| 信息行（新增） | `USB 正在连接` / `USB 等待设备` / `USB 正在查找设备` / `USB CDC 正常 RX n` / `USB 设备已断开` / `USB 连接错误` |
| 中间 | 横滚/俯仰/航向、加速度、角速度、温度…（和串口版一样）|
| 底部状态行 | `已存 N 帧` / `无SD请检查连接` / `SD读写错误` |

> 字库：本工程的汉字是内部 Flash 点阵（`User/fonts/font_cn_data.c`，146 个字）。
> 上面所有汉字都已在字库里（以后改显示文字时要注意这一点，不在字库里的字会显示空白）。

## 五、编译与烧录

1. Keil 打开 `MDK-ARM/Project.uvprojx`，目标 `H750_UART`，直接 Build → 生成 `MDK-ARM/Objects/H750_IMU_USB.hex`
   （命令行：`UV4.exe -b Project.uvprojx -t H750_UART -o build.log`）
2. 烧录：`MDK-ARM/H750_IMU_USB.hex`（Keil 里直接 Download 也可以）
3. 上电顺序：先给板子 DC 供电 → 屏幕出现 `USBLOG v1 USB-HOST` → 插上模块的 USB →
   信息行变 `USB 正在查找设备` → `USB CDC 正常 RX n`（n 开始涨）→ 底部 `已存 N 帧`

## 六、排查表

| 屏幕现象 | 含义 / 怎么办 |
| --- | --- |
| 一直 `USB 等待设备` | 主机没看到设备：换一根能传数据的线、把模块插到底、量一下 USB HOST 口 VBUS 有没有 5V |
| `USB 正在查找设备` 一直不变 | 枚举/类驱动没过：模块可能不是标准 CDC，或枚举中途出错；先用电脑确认模块能认成 COM 口 |
| `USB CDC 正常 RX 0` 不涨 | 枚举好了但模块不发数据：界面上的"读加速度"探测也没回 → 检查模块的输出率设置和 USB 数据通路 |
| `USB 设备已断开` | 接触不良/线松（或模块掉电）|
| 数值在动但显示 `无SD请检查连接` | 卡没插好，或不是 FAT32 |

## 七、回退（改回串口版）

1. `Inc/main.h`：`IMU_LINK_USB` 改 0、`IMU_LINK_UART` 改 1（两条路都保留，都能编译）；
2. 重新编译烧写；或者直接烧上一次的串口版固件 `MDK-ARM/H750_UART_SDLOG.hex`；
3. 串口版接线：模块 `TX→PA10`、`RX→PA9`、`GND`、`3V3`，**并且把 J80/J81 两个跳线帽拔掉**（装着就是板载 CH340C 占着这两个脚）；
4. 回退到串口版不影响 USB 那边：串口版不初始化 USB，PB14/PB15 不会被占用。

## 八、已知限制

* USB 版没有"波特率"这个概念，所以屏幕不再显示波特率；模块回传率由固件上电后发的 `WitSetOutputRate(RRATE_10HZ)` + `WitSetContent(ACC|GYRO|ANGLE)` 固定；
* USB 用"CPU + 中断"的方式收发（没开 DMA），占一点 CPU，但 10Hz 数据量极小，不影响 LCD 和 SD 卡；
* 万一模块的 USB 描述符里"通信接口子类/协议"不是 `0x02 / 0x01`（标准 CDC-ACM 的常见取值），CDC 类会匹配不上——那种情况需要在 `usbh_cdc.c` 的接口匹配处放宽条件（按屏幕上的接口类码再定）。
