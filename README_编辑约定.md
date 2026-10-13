# 编辑约定（VSCode / Keil 分工）

## 为什么需要这个约定

VSCode 和 Keil uVision 是两个互相独立的编辑器：各自把文件读进自己的内存里显示，
只在「打开文件」和「保存文件」这两个时刻才碰磁盘。

* 打开 = 从磁盘读一次
* 保存 = 把内存里的内容写回磁盘

中间任何人对磁盘的改动，另一边都不知道，也不会有提示。谁最后保存，谁赢，没有合并。

实际踩过的坑：

* `Src/main.c` 曾被还原回重构前的旧版本 —— 原因是 Keil 编辑器里还开着旧内容，
  一按保存就把旧内容写回磁盘，覆盖了已经改好的文件。
* `MDK-ARM/force.h` 也出现过类似情况（被写成一个只剩 8 行的残桩）。

Keil uVision 不会自动检测外部改动；VSCode 默认会（但没有未保存修改时才自动刷新）。

## 约定

1. 代码只在 VSCode 里改；Keil 只用来编译 / 下载。
2. 让 AI 或脚本改文件之前，先把对应文件在 Keil 里关掉；不要在 Keil 里保存这些源文件。
3. 不要在两边同时编辑同一个文件。VSCode 里若有未保存的修改，先保存（或撤销）再让 AI 改。
4. 编译 / 下载前，在 Keil 里按 File -> Reload，确保读到的是磁盘上的最新内容；
   或者直接用 VSCode 的 Keil Assistant 编译（它调 UV4，读的是磁盘文件，没有缓存问题）。
5. 发现两边不一致时：
   * Keil：File -> Reload
   * VSCode：关掉标签页重新打开，或 Ctrl+Shift+P -> Revert File

## 当前文件结构（传感器相关）

| 文件 | 内容 |
| --- | --- |
| `MDK-ARM/force.c` | 传感器（力传感器 ADC + IWT603 IMU）：初始化 + 轮询，IMU 只走 USB Host |
| `MDK-ARM/force.h` | 对外接口 `Force_Init()` / `Force_Task()`，以及显示/记录需要的全局量 |
| `Src/main.c` | 应用逻辑：LCD 显示、触摸 UI、SD 记录、编码器、继电器；调用 `Force_Init()` / `Force_Task()` |
| `Src/stm32h7xx_it.c` | 中断处理（USB OTG HS、EXTI8）；原 USART1 中断已移除 |

说明：`IMU_LINK_USB` 现在固定为 USB Host 链路；串口（USART1）那套代码已经从
`main.c` / `force.c` / `force.h` / `stm32h7xx_it.c` 中删除。

## 怎么核对改完后有没有被覆盖

看行数（2026-10-08 的记录）：

```
Src/main.c            1440 行
Src/stm32h7xx_it.c     131 行
MDK-ARM/force.c        378 行
MDK-ARM/force.h         30 行
```

关键点：`main.c` 里应该搜不到 `MX_ADC1_Init`、`SensorScan`、`WitInit`、
`LCD_UpdateLogStatus`；`Force_Init()` 和 `Force_Task()` 各出现 1 次。

如果 Keil 里看到的 `main.c` 还是 2000 多行、还能搜到 `MX_ADC1_Init`，
那就是 Keil 显示的旧缓存 —— File -> Reload 即可。

## 编译

命令行编译（Keil V5.06 / ARMCC）：

```
C:\Keil_v5\UV4\UV4.exe -b "...\\MDK-ARM\\Project.uvprojx" -j0 -o "<日志文件>"
```

输出：`MDK-ARM\Objects\H750_IMU_USB.axf`（当前 USB-only 版本 Code=77788）。
