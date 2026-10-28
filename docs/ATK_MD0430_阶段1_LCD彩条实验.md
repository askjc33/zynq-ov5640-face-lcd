# ATK-MD0430 V2.0 阶段 1：独立 LCD 彩条实验

本文对应 Vivado 2018.3、STUDYBOARD Zynq-7020 和 ATK-MD0430 V2.0。目标不是立刻替换 HDMI，而是先用一个完全独立、可观察、可仿真的工程验证：

1. J7 接线及 3.3 V IO 电平正确；
2. FPGA 能产生符合 Intel 8080 并口语义的写周期；
3. NT35510 能完成复位、初始化、横屏设置和 RGB565 显存写入；
4. 800×480 面板能显示 8 条竖向彩条。

现有摄像头、DDR、VDMA、帧差和 HDMI 工程均未修改。阶段 1 成功后，再把彩条像素源替换为摄像头/帧差 AXI4-Stream。

## 1. 为什么先做独立实验

若直接修改完整工程，白屏可能来自接线、IO 电压、LCD 初始化、像素格式、VDMA、AXI 握手或摄像头中的任意一层。独立实验只保留下面的数据流：

```text
50 MHz PL 时钟
      │
      ▼
NT35510 初始化/彩条状态机
      │ tx_valid/tx_ready/tx_done
      ▼
8080 单次写周期发生器
      │ CS、RS、WR、D[15:0]
      ▼
ATK-MD0430 内部 NT35510 GRAM
      │ 屏控制器自行刷新
      ▼
800×480 LCD 面板
```

这里不使用 PS、DDR、VDMA，也不需要 SDK 程序。只需将 bitstream 下载到 PL。

## 2. 文件分层

| 文件 | 层次 | 作用 |
| --- | --- | --- |
| `bringup/atk_md0430_colorbar/lcd8080_write_cycle.v` | 总线物理周期 | 把一次 `tx_rs + tx_data` 请求转换成 CS/WR/数据时序 |
| `bringup/atk_md0430_colorbar/atk_md0430_colorbar_top.v` | 设备协议与像素源 | NT35510 初始化、窗口设置、RGB565 彩条和总状态机 |
| `bringup/atk_md0430_colorbar/atk_md0430_colorbar.xdc` | 板级连接 | 50 MHz 时钟、复位键和 J7 FPGA 管脚约束 |
| `bringup/atk_md0430_colorbar/tb_atk_md0430_colorbar.v` | 行为验证 | 模拟 LCD 在 WR 上升沿锁存并检查命令和像素 |
| `scripts/simulate_atk_md0430_colorbar.tcl` | 仿真入口 | 用 Vivado XSim 重建并运行行为仿真 |
| `scripts/build_atk_md0430_colorbar.tcl` | 实现入口 | 重建工程、综合、实现、生成 bit 和报告 |

把“总线周期”和“设备初始化”分开，是为了后续复用：接入摄像头时只更换顶层数据来源，底层 8080 周期模块仍然可以保留或优化。

## 3. Intel 8080-16 位接口

### 3.1 信号语义

| 信号 | 有效电平 | 含义 |
| --- | --- | --- |
| `CS` | 低 | 选择 LCD 控制器 |
| `RS` | 0/1 | 0 表示命令或寄存器地址，1 表示数据/像素 |
| `WR` | 低脉冲 | 写选通；NT35510 在 WR 返回高电平的边沿锁存总线 |
| `RD` | 低脉冲 | 读选通；阶段 1 不读，始终保持高 |
| `D[15:0]` | 双向 | 阶段 1 只作为 FPGA 输出 |
| `RST` | 低 | LCD 硬件复位 |
| `BL` | 高 | 打开模块背光控制 |

一次 `LCD_WriteReg(0x3A00, 0x0055)` 在总线上不是一个周期，而是两个周期：

```text
周期 1：RS=0，D=0x3A00，WR 产生低脉冲    // 选择像素格式寄存器
周期 2：RS=1，D=0x0055，WR 产生低脉冲    // 写入 RGB565 设置
```

写像素时，先发送一次 `RS=0, D=0x2C00` 进入显存写入模式，之后保持发送 `RS=1` 的 16 位 RGB565 像素。

### 3.2 当前保守时序

`lcd8080_write_cycle.v` 在 50 MHz 下使用 20 ns 时钟，步骤如下：

```text
IDLE → SETUP → WR_LOW1 → WR_LOW2 → RELEASE → IDLE
```

- 数据、RS 和 CS 先建立；
- WR 低电平维持约 40 ns；
- WR 上升后才结束周期；
- 上层握手目前还留有空闲拍，目的首先是提高杜邦线测试的容错率。

这不是最终性能版本。阶段 2 会减少握手空拍，或使用异步 FIFO 解耦 AXI4-Stream 与 LCD 写时钟。优化前必须根据真实波形重新检查建立/保持时间。

## 4. NT35510 初始化层

顶层状态机按以下顺序运行：

```text
RESET_LOW
  → RESET_WAIT
  → PAGE1_POWER
  → GAMMA_D1...D6
  → PAGE0_DISPLAY
  → SLEEP_OUT(0x1100)
  → 等待 120 ms
  → DISPLAY_ON(0x2900)
  → 设置 800×480 窗口
  → RAM_WRITE(0x2C00)
  → 写 384000 个像素
  → 打开背光并停止写入
```

### 4.1 重要命令

| 命令/寄存器 | 作用 |
| --- | --- |
| `F000..F004` | 选择 NT35510 扩展命令页 |
| `B0..BF` | 内部电源、升压、VCOM 等模拟参数 |
| `D1..D6` | 红/绿/蓝正负 Gamma 曲线 |
| `3A00 = 0x55` | 16 bit/pixel，即 RGB565 |
| `3600 = 0xA0` | 交换横纵扫描并翻转一轴，形成 800×480 横屏 |
| `1100` | 退出睡眠 |
| `2900` | 开启显示 |
| `2A00..2A03` | 列范围 0～799 |
| `2B00..2B03` | 行范围 0～479 |
| `2C00` | 开始连续写显示 RAM |

Gamma 曲线的 52 个字节只存储一份，然后用 `gamma_bank=0..5` 写到 D1～D6。这样能直接看到“六组相同曲线”的算法关系，也避免复制 312 行常数。

初始化值参考历史 ALIENTEK NT35510 驱动序列；模块型号、8080-16 位接口和 RGB565 参数同时由正点原子公开 ATK-MD0430 资料确认。初始化值最终仍以本块 V2.0 实物测试为准。本阶段尚未实现 ID 回读，原因是先将 D[15:0] 保持为单向输出，降低首次接线测试复杂度。

## 5. RGB565 彩条

一个像素的 16 位布局为：

```text
D15                           D0
RRRRR GGGGGG BBBBB
 5位    6位    5位
```

顶层按横坐标把 800 列分成 8 组，每组 100 列：

| 区域 | RGB565 | 颜色 |
| ---: | ---: | --- |
| 0 | `FFFF` | 白 |
| 1 | `FFE0` | 黄 |
| 2 | `07FF` | 青 |
| 3 | `07E0` | 绿 |
| 4 | `F81F` | 品红 |
| 5 | `F800` | 红 |
| 6 | `001F` | 蓝 |
| 7 | `0000` | 黑 |

如果颜色位置正确但红蓝互换，优先检查 D[15:0] 接线，然后再检查扫描/颜色顺序配置，不能直接靠交换颜色常量掩盖硬件错线。

## 6. 已完成的自动验证

### 6.1 行为仿真

仿真使用 8×4 小画面，但完整执行初始化状态机。测试平台在 `lcd_wr_n` 上升沿采样，这正是 LCD 锁存数据的时刻。已验证：

- 看到了 `0x1100`、`0x2900` 和 `0x2C00`；
- `RD` 全程为高；
- 32 个像素数量正确；
- 每行的 8 种 RGB565 颜色顺序正确。

Vivado 2018.3 仿真结果：

```text
PASS: initialization commands and 32 RGB565 pixels verified
```

复现命令：

```powershell
& 'E:\Xilinx\Vivado\2018.3\bin\vivado.bat' `
  -mode batch -source 'scripts\simulate_atk_md0430_colorbar.tcl'
```

### 6.2 综合、实现和时序

完整 800×480 参数已通过 Vivado 2018.3：

- 综合：0 error，0 critical warning；
- 布局布线：0 error；
- WNS：13.737 ns；
- WHS：0.153 ns；
- LUT：277，约 0.52%；
- 寄存器：130，约 0.12%；
- BRAM：0；
- DSP：0。

`lcd_rd_n` 被综合器报告为常量 1，是阶段 1 的预期设计。纯 PL 工程还会出现没有 PS7 的 Zynq DRC warning；它不阻止 JTAG 下载，正式主工程中会保留 PS7。

## 7. 上板前检查

不要带电插拔屏幕或杜邦线。

1. 开发板断电，确认使用右侧 J7，而不是中央 42Pin RGB FPC 座。
2. 根据 `docs/ATK_MD0430_V2_J7_接线表.md` 逐线连接。
3. 屏幕排针 1 脚、J7 的奇偶排必须以丝印/万用表确认；从背面观察会镜像。
4. 先只接 GND、3.3 V 和 5 V，通电测量屏端：逻辑电源约 3.3 V、背光电源约 5 V。
5. 断电后再接 D0～D15、CS、RS、WR、RD、RST、BL。
6. 信号线首次尽量控制在约 10 cm，并至少连接两根可靠地线。
7. 检查 5 V 只到屏幕 V5/背光供电，绝不能进入 FPGA IO。

## 8. 用 Vivado 2018.3 打开和下载

### 8.1 直接打开已生成工程

在 Vivado 2018.3 中选择 `File → Open Project`，打开：

```text
build/atk_md0430_colorbar/studyboard_atk_md0430_colorbar.xpr
```

然后：

1. `Open Hardware Manager`；
2. `Open Target → Auto Connect`；
3. 选择 `xc7z020_1`；
4. `Program Device`；
5. 选择 `output/bit/studyboard_atk_md0430_colorbar.bit`；
6. 点击 `Program`。

### 8.2 从脚本完全重建

```powershell
& 'E:\Xilinx\Vivado\2018.3\bin\vivado.bat' `
  -mode batch -source 'scripts\build_atk_md0430_colorbar.tcl'
```

输出文件：

```text
output/bit/studyboard_atk_md0430_colorbar.bit
output/bit/studyboard_atk_md0430_colorbar_timing.rpt
output/bit/studyboard_atk_md0430_colorbar_utilization.rpt
```

## 9. 预期上板现象

下载 bitstream 后约 0.3～0.4 秒内背光保持关闭，随后一次性出现完整的 8 条竖向彩条。彩条不会动画，但应持续稳定显示，因为 LCD 控制器在刷新自己的 GRAM。

按下开发板 `PL_KEY` 会使顶层重新复位，屏幕短暂熄灭，然后重新初始化并写入彩条。

## 10. 故障定位顺序

### 背光完全不亮

1. 测量屏幕 V5 是否为 5 V；
2. 检查 BL 是否接 J7-36；
3. 检查 FPGA 是否成功下载；
4. 观察下载后 BL 是否从低变高。

### 背光亮但白屏

1. 检查 RST、CS、WR、RS；
2. 确认 D0～D15 没有整体错一位或镜像；
3. 用示波器/逻辑分析仪检查 WR 是否有初始化脉冲和约 384000 个像素脉冲；
4. 检查屏幕确为 ATK-MD0430 V2.0/NT35510。

### 有图但颜色错误

1. 对照白、黄、青、绿、品红、红、蓝、黑的固定顺序；
2. 单根核对 D0～D15；
3. 红蓝互换时再检查 `3600` 的颜色/扫描配置；
4. 噪点或偶发色块通常优先缩短信号线、加强接地或降低写速度。

### 图像旋转、镜像或尺寸不对

先只调整 `page0_data()` 中 `3600` 对应的值，并记录 MX/MY/MV 三个位的作用；不要同时修改窗口尺寸和像素计数，否则难以判断是哪一项生效。

## 11. 阶段 2 数据流预告

彩条成功后，下一阶段不会直接删除 HDMI，而是并行增加 LCD 输出分支：

```text
OV5640 DVP
   → Video In to AXI4-Stream
   → 帧差模块
   → VDMA0 写 DDR
   → LCD 专用 MM2S/格式转换
   → AXI4-Stream FIFO
   → RGB888 转 RGB565
   → 8080 连续写控制器
   → ATK-MD0430
```

需要重点解决三个问题：

1. **格式转换**：现工程 24 位 RGB888，LCD 为 16 位 RGB565；
2. **流控**：LCD 无 AXI `tready`，必须用 FIFO 吸收短时速率差；
3. **带宽**：800×480×16 bit×30 fps 约为 184.32 Mbit/s，即 23.04 MB/s，不含协议空拍；当前保守控制器需要提速后才能稳定达到 30 fps。

只有阶段 1 实物通过后才进入这一步，这样后续问题会集中在数据流和带宽，不再混入接线与初始化的不确定性。

## 12. 资料来源与边界

- 正点原子 ATK-MD0430 公开仓库：模块为 NT35510、800×480、RGB565、Intel 8080-16 位并口；
- 用户提供的 STUDYBOARD 系统板原理图：J7、Bank 35、电源和 FPGA 管脚；
- 历史 ALIENTEK 兼容 STM32 LCD 驱动镜像：NT35510 电源、Gamma 和显示初始化序列；
- 当前阶段的仿真和实现报告：只证明 RTL/约束/FPGA 时序一致，不能替代屏幕实物电气验证。

