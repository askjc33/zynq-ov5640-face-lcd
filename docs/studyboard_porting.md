# 正点原子 Zynq-7020 最小系统板移植说明（Vivado/SDK 2018.3）

## 1. 当前结论

原仓库不是一个可直接打开的完整 Vivado 工程：缺少 `.xpr`、Block Design、顶层 wrapper、
Xilinx IP 输出文件、`video_fifo`、HDF 和 BSP。本目录已经根据仓库 README、用户提供的
`STUDYBOARD.08 A` 原理图和本机官方板卡工程重建了这些内容。

已确认并验证：

- FPGA：`xc7z020clg400-2`。
- 工具：Vivado/SDK 2018.3。
- PS 时钟：33.333333 MHz；PL `FCLK_CLK0`：100 MHz。
- DDR3：两片 H5TQ4G63AFR-PBI，32 位、1 GiB；PS DDR 频率约 533.333 MHz。
- OV5640 `XCLK`：由 PL 输出 24 MHz。
- 图像模式：RGB888，640×480。
- 顶层综合：0 error、0 critical warning。
- 实现 DRC：0 error。
- 最终时序：WNS `+3.168 ns`、TNS `0`、WHS `+0.040 ns`、THS `0`。
- SDK standalone 应用已实际编译，ELF 大小为 497,308 字节。

生成物位于 `output/full/`：

- `studyboard_frame_difference.bit`：完整 PL bitstream。
- `ov5640_frame_difference.elf`：Cortex-A9 standalone 程序。
- `system.hdf`：SDK 2018.3 硬件平台，内部已确认包含 `system_wrapper.bit`。
- `system.hwdef`：不含 bitstream 的硬件描述中间文件。

另有 `output/bit/studyboard_hdmi_colorbar.bit`，用于先单独验证 HDMI、PL 50 MHz 时钟和
TMDS 引脚，不需要运行 ARM 软件。

## 2. 重建后的数据通路

```text
OV5640 DVP -> Capture -> Video In to AXI4-Stream -> Frame Difference
                                                   ^           |
                                                   |           v
                                           VDMA0 MM2S     VDMA0 S2MM
                                                   \           /
                                                     PS DDR3
                                                        |
                                                   VDMA1 MM2S
                                                        |
                                           AXI4-Stream to Video Out
                                                        |
                                                  DVI/TMDS -> HDMI
```

PS 通过 AXI-Lite 配置两路 VDMA、VTC 和动态像素时钟，通过两位 GPIO EMIO 模拟 SCCB，
配置 OV5640。帧缓冲从 PS DDR 基地址再偏移 `0x01000000` 开始。

## 3. 板级引脚

### OV5640（Bank 34，3.3 V）

| 信号 | 管脚 | 信号 | 管脚 |
| --- | --- | --- | --- |
| `cam_data[0]` | R14 | `cam_data[4]` | U14 |
| `cam_data[1]` | U13 | `cam_data[5]` | W13 |
| `cam_data[2]` | V13 | `cam_data[6]` | V12 |
| `cam_data[3]` | U15 | `cam_data[7]` | Y14 |
| `cam_href` | T12 | `cam_vsync` | U12 |
| `cam_pclk` | W14 | `cam_xclk` | W15 |
| `cam_rst_n` | P14 | `cam_pwdn` | V15 |
| SCCB SCL / EMIO[0] | T10 | SCCB SDA / EMIO[1] | T11 |

板上已有 5.1 kΩ SCCB 上拉。只能使用与该板 CAMERA/FPC 接口定义兼容的 OV5640 模块；
插拔排线前必须断电，并确认排线金手指方向。

### HDMI（Bank 35）

| 信号 | P | N |
| --- | --- | --- |
| TMDS clock | J18 | H18 |
| TMDS data 0 | G19 | G20 |
| TMDS data 1 | K19 | J19 |
| TMDS data 2 | J20 | H20 |

该板没有原目标板的 `tmds_oen` 信号，重建工程没有把它导出。固定 640×480 输出不依赖
DDC/EDID 或 HPD。

## 4. 建议的首次上板顺序

1. 断电连接 HDMI；先不要接摄像头。
2. 按板上丝印把启动拨码切到 JTAG 模式，连接 `UART JTAG` USB 口并给板卡供电。
3. 用 Vivado Hardware Manager 下载 `output/bit/studyboard_hdmi_colorbar.bit`。显示器应出现
   640×480 彩条；若无图，先排查显示器兼容性、HDMI 线和板级时钟，不要立即调摄像头。
4. 断电，连接兼容的 OV5640 模组和 HDMI，再上电。
5. 在 SDK 2018.3 打开 `build/sdk_cli_workspace`，选择
   `ov5640_frame_difference`，执行 **Run As > Launch on Hardware (System Debugger)**。
   HDF 已包含 bitstream，也可以先在 Vivado 下载 `output/full/studyboard_frame_difference.bit`，
   再从 SDK 运行 `output/full/ov5640_frame_difference.elf`。
6. 打开 UART0 串口，参数 `115200 8N1`。正常应看到：

   ```text
   OV5640 detected successful!
   ```

   第一帧缓冲尚未稳定时可能短暂出现黑屏或杂点，等待数帧后再判断。

注意：完整 bitstream 单独下载后不会自动产生有效 HDMI 图像；VDMA、VTC、动态像素时钟和
OV5640 都由 ELF 初始化，因此必须同时运行 ARM 程序。

## 5. 从脚本重新生成

在仓库根目录的 PowerShell 中执行：

```powershell
& 'E:\Xilinx\Vivado\2018.3\bin\vivado.bat' -mode batch -source '.\scripts\check_vivado_environment.tcl'
& 'E:\Xilinx\Vivado\2018.3\bin\vivado.bat' -mode batch -source '.\scripts\create_studyboard_project.tcl'
& 'E:\Xilinx\Vivado\2018.3\bin\vivado.bat' -mode batch -source '.\scripts\build_studyboard_project.tcl'
& 'E:\Xilinx\SDK\2018.3\bin\xsct.bat' '.\scripts\build_studyboard_sdk.tcl'
```

如果实现已完成、只需重新导出 HDF/bit，可运行：

```powershell
& 'E:\Xilinx\Vivado\2018.3\bin\vivado.bat' -mode batch -source '.\scripts\export_studyboard_hardware.tcl'
```

Vivado 工程为 `build/studyboard_frame_difference/studyboard_frame_difference.xpr`；脚本会重新
生成工程，因此应修改 `scripts/create_studyboard_project.tcl` 和源文件，而不要只修改
`build/` 下的自动生成文件。

## 6. 已修复的问题

- 原仓库帧差 RTL 的纵坐标计数写成了 `s0_y_cnt <= y_cnt + 1'b1`，已改为
  `s0_y_cnt <= s0_y_cnt + 1'b1`。
- 补建 24 位、2048 深度、FWFT 模式的 `video_fifo`。
- 为自定义帧差模块补建 AXI4-Stream 接口 wrapper。
- 根据本板原理图重写相机和 HDMI XDC。
- 恢复官方板卡 PS7 DDR、UART0、QSPI、GPIO EMIO 配置。
- 为摄像头 PCLK 域增加独立复位同步器。修复前有 141 个跨域 setup 违例、WNS
  `-3.093 ns`；修复后所有约束时序通过。
- 使用 `write_sysdef` 生成真正包含 bitstream 的 SDK 2018.3 HDF。

## 7. 已知警告与调试重点

- 原理图把 `cam_pclk` 接到 W14，而 W14 不是时钟专用输入。XDC 必须使用
  `CLOCK_DEDICATED_ROUTE FALSE`。最终 72 MHz 约束下摄像头域 WNS 为 `+9.285 ns`，但
  非专用时钟走线仍会增加板级抖动和温压敏感性；相机异常时优先用 ILA 检查 PCLK、VSYNC、
  HREF 和帧计数。
- Vivado 会对原始相机采集 RTL和帧差 FIFO 报 BRAM 控制信号由异步复位寄存器驱动的
  `REQP-1839/1840` 警告。它们没有阻止实现，且当前时序通过；若出现只在冷启动发生的偶发
  花屏，应继续把这些控制寄存器改为同步复位，而不是屏蔽 DRC。
- 原始帧差加速器对 AXI4-Stream backpressure 的处理不完整；正常 VDMA 连续接收时通常不会
  触发。如果 ILA 观察到 `m_axis_tready` 长时间拉低或丢行，需要为加速器增加输出 skid
  buffer，并让所有像素计数只在 `TVALID && TREADY` 时更新。
- Windows 会提示工程路径较长。当前构建成功；若后续新增 IP 后遇到 260 字符路径问题，可用
  `subst` 把仓库映射到较短盘符再重新生成工程。

## 8. 故障定位顺序

- 彩条 bitstream 无图：先查 HDMI 线、显示器、JTAG 下载是否成功和 PL 50 MHz 时钟。
- 彩条正常、完整工程无图：确认 ELF 已运行，检查 UART 是否有启动输出。
- UART 报 `OV5640 detected failed!`：断电检查排线方向、模块兼容性、24 MHz XCLK、SCL/SDA
  电平和 5.1 kΩ 上拉。
- OV5640 识别成功但仍黑屏：ILA 依次检查 `cam_vsync/cam_href`、Video-In AXIS、VDMA0
  S2MM/MM2S、VDMA1 MM2S 和 Video-Out lock/underflow。
- 有画面但运动框异常：先确认 640×480 模式和行尾 `TLAST`，然后重点检查帧差 FIFO 对齐和
  VDMA 帧索引同步。

当前结论来自原理图、工程生成、综合、实现和 SDK 编译验证；尚未在用户这块实物板上完成
JTAG 下载、摄像头识别和 HDMI 实拍闭环，上板结果仍需按上述顺序确认。

### SCCB 诊断输出

当前 ELF 会在初始化前打印 SCCB 空闲电平，并连续读取五次芯片 ID。正常输出应包含
`SCL=1 SDA=1` 和 `0x5640`。若读到 `0xffff`，通常是模块未响应、XCLK 缺失、模块未供电或
SDA 没有接通；若读到 `0x0000`，通常是 SDA 被拉低、短路或模块方向错误；若 ID 正确但后续
无画面，再进入 VSYNC/HREF/PCLK 和 VDMA 数据通路排查。
