# firmware_witsdk —— 维特官方 STM32 例程 → STM32H750XB 移植版

> **注意（后续版本已改动）**：本文件写的是**旧版**的串口方案（USART1 上报电脑 + UART4 接模块）。
> 现在的版本已经改成：**IMU 接 USART1（PA9/PA10），UART4（PC10/PC11）删除，数据记录到 SD 卡**，
> 不再往电脑发 46 字节二进制帧，电脑命令也随之取消。
> 请看 **[README_SD卡记录说明.md](README_SD卡记录说明.md)**。

把维特智能官方的 STM32 例程（`STM32Core_SDK_Normal`，**STM32F103C8T6 + 标准库**）
移植到 **STM32H750XB（野火 STM32H7 Pro 开发板）+ HAL 库 + Keil MDK**。

* 原例程下载：`https://attach.wit-motion.cn/s/7ZHV?path=/WT-NORMAL协议/STM32`
  （压缩包 `STM32Core_SDK_Normal(旧版).zip`，已解压在 `H8\witsdk_STM32Core\`）
* 协议库 `wit_c_sdk.c / wit_c_sdk.h / REG.h` 原样使用，**一行没改**。

## 一、文件结构

```
firmware_witsdk/
├─ Inc/   main.h  stm32h7xx_hal_conf.h  stm32h7xx_it.h        （H750 工程自己的）
│         wit_c_sdk.h  REG.h                                  （维特 SDK，原样）
├─ Src/   main.c                                              （移植后的主程序）
│         wit_c_sdk.c                                         （维特 SDK，原样）
│         stm32h7xx_hal_msp.c  stm32h7xx_it.c  system_stm32h7xx.c
│         iwt603.c  imu_pack.c                                （可选，没用到，留着参考）
├─ Drivers/  CMSIS + STM32H7 HAL 库（和 STM32Cube_FW_H7 V1.13.0 一致）
└─ MDK-ARM/  Project.uvprojx（打开这个）  startup_stm32h750xx.s  H750XB_Flash.sct
```

## 二、原例程 ↔ 本工程 的对应关系

| 官方例程（F103 + 标准库） | 本工程（H750XB + HAL） |
| --- | --- |
| `Usart1Init(115200)` | `MX_USART1_UART_Init()` —— **PA9/PA10**，AF7，经板载 CH340C 到电脑 |
| `Usart2Init(9600)` | `MX_UART4_UART_Init()` —— **PC10/PC11**，AF8，接 IWT603 |
| `Uart2Send(p,n)` | `SensorUartSend()` 里 `HAL_UART_Transmit(&huart4,…)` |
| UART2 接收中断里喂字节 | `HAL_UART_RxCpltCallback()` 里 `WitSerialDataIn()` |
| `delay_ms()` | `Delayms()` 里 `HAL_Delay()` |
| `SysTick_Init()` + 标准库 | `HAL_Init()` + `SystemClock_Config()`（25MHz HSE → SYSCLK 400MHz） |
| `printf("acc:…")` 文字输出 | 默认改成 **46 字节二进制帧**给 MATLAB；`main.c` 里 `PC_OUT_BINARY` 改 0 就恢复成原例程的文字输出 |
| `stm32f10x.h` 等 F1 头文件 | `stm32h7xx_hal.h`，中断函数在 `stm32h7xx_it.c` |

**wit_c_sdk 的调用顺序和原例程完全一致**：

```c
WitInit(WIT_PROTOCOL_NORMAL, 0x50);   // 初始化标准协议
WitSerialWriteRegister(SensorUartSend);   // 发指令给模块的回调
WitRegisterCallBack(SensorDataUpdata);    // 数据更新回调（sReg[] 有新值）
WitDelayMsRegister(Delayms);
AutoScanSensor();     // 主动发读寄存器指令，4800→921600 逐档找模块
```

数据换算也和原例程一样：`sReg[AX..AZ]/32768*16`（g）、`sReg[GX..GZ]/32768*2000`（°/s）、
`sReg[Roll..Yaw]/32768*180`（°）、`sReg[TEMP]/100`（℃）。

## 三、电脑侧命令（和原例程一致，串口助手 115200-8-N-1，每条后面带回车换行）

| 发 | 作用 |
| --- | --- |
| `h` | 打印帮助 |
| `a` | 加速度校准（模块要水平静止） |
| `m` / `e` | 磁场校准开始 / 结束（中间围绕三轴转圈） |
| `R` / `r` | 回传率 → 10Hz / 1Hz |
| `B` / `b` | 模块波特率 → 115200 / 9600（改完板子会跟着改 UART4） |
| `C` / `c` | 回传内容 → 全开 / 只回加速度 |

## 四、上电时看什么（排查接线最有用）

板子上电后会依次用 4800/9600/19200/38400/57600/115200/230400/460800/921600
**主动发"读加速度"指令**找模块：

* 找到 → 打印 `xxxxx baud find sensor` + 帮助信息，说明模块这侧通了；
* 找不到 → 打印 `can not find sensor` / `please check your connection`。

## 五、移植时踩到的两个坑（都记下来）

1. **源码编码**：Keil ARMCC v5 在中文 Windows 下按 GBK 解析源码。本工程里带中文注释的
   `.c/.h` 都是 **GBK 编码**（不是 UTF-8）。如果哪天用别的编辑器改完存成 UTF-8，
   会出现 `error: #40: expected an identifier` 或 `#870-D: invalid multibyte character sequence`，
   把文件另存为 ANSI/GBK 即可。
2. **宏名撞车**：维特 SDK 的 `REG.h` 里 `#define q0 0x51`…（四元数寄存器地址），
   而旧的 `iwt603.h` 结构体里也有 `q0..q3` 成员 —— 两个头文件一起 include 会编译不过。
   本工程的做法：主程序只 include `wit_c_sdk.h`，46 字节帧在 `main.c` 里自己组
   （`PackFrame()`），不再依赖 `iwt603.h`。

## 六、编译结果

```
*** Using Compiler 'V5.06 update 6 (build 750)'
Build target 'H750_UART'
compiling main.c...
linking...
Program Size: Code=21564 RO-data=1288 RW-data=72 ZI-data=1960
FromELF: creating hex file...
"Objects\H750_UART.axf" - 0 Error(s), 0 Warning(s).
```

烧写：Keil 打开 `MDK-ARM\Project.uvprojx`，`Options for Target → Debug` 选你的仿真器
（工程里当前是 CMSIS-DAP，用 ST-Link 就换成 ST-Link Debugger），`F8` 下载。
可直接烧的固件：`MDK-ARM\Objects\H750_UART.hex`。
