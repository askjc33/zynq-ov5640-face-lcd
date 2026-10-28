# 08｜PL、裸机与 Linux：哪些代码在同一块板上接力

本章说明早期帧差代码与当前 Linux 人脸系统的**关系和区别**。不把“仓库里有此文件”当成“当前 SD 卡正在执行它”。真正的硬件配置由启动时加载的 `BOOT.BIN`/bitstream、设备树和内核版本决定；若要追溯精确硬件版本，须保存 XSA、启动文件哈希和构建记录。

## 1. 控制平面、像素平面、显示平面

```text
控制：PS 裸机 SCCB 或 Linux /dev/i2c-0 → OV5640 寄存器
像素：OV5640 DVP → ov5640_capture_data → AXI4-Stream → frame_difference_axis[_bram]
                                     → VDMA0 S2MM → DDR
显示：DDR → VDMA1 MM2S → HDMI VTC/DVI   （早期 HDMI 路线）
                   或 → axis_nt35510_display → 8080 LCD （LCD 路线）
Linux 人脸：DDR/V4L2 → NCNN → sysfs 带框帧 → VDMA1 MM2S → LCD
```

SCCB/I²C 与 DVP 是不同总线：读到 `0x5640` 仅证明配置通路当时可读。`TUSER` 通常标帧开始、`TLAST` 标行结束，`TVALID && TREADY` 才真正传输一个 beat；LCD 初始化期间可以拉低 `TREADY` 反压，使上游保持数据。S2MM 与 MM2S 方向相反，也由不同 DMA 通道负责。当前 Linux 驱动是 Xilinx DMAEngine 的**客户端**，并不替代 PL IP。

## 2. 原始/移植 HDMI 路线

`scripts/create_studyboard_project.tcl` 用 Vivado 2018.3 创建 Block Design：`ov5640_capture_data` 接 DVP，Video-In-to-AXIS 后接 `rtl/frame_difference_axis.v`，VDMA0 S2MM 写 DDR；VDMA1 MM2S 从 DDR 读，Video-Out/VTC 加 DVI/TMDS IP 输出 HDMI。`constraints/studyboard_top.xdc` 描述这一顶层的引脚。`bringup/hdmi_colorbar/` 用不依赖摄像头的彩条验证显示时钟/引脚/编码器，比直接调完整相机链容易定位。

早期 `rtl/frame_difference_axis.v` 不是 YOLO 人脸识别：它每 8×8 区块取一个亮度样本（640×480→80×60），比较相同位置的上一帧，超过阈值的采样点统计成运动范围，再在视频流上画**运动框**。它解决了直接拿一个正在被 VDMA 重写的 DDR“上一帧”做参照时，比较坐标/时间不稳定、运动框扩大到全屏的问题。但它使用异步读取的分布式 RAM 与差异图，资源/时序不一定适合后续版本；`rtl/frame_difference_axis_bram.v` 是面向 BRAM 的改进版。`ov5640_hdmi.srcs/` 里的 `AXI_VIP_Frame_Difference.v` 是原始工程导入模块，不能因名字相似就把它说成当前运行的检测算法。

`SDK/ov5640_hdmi_frame_difference/src/main.c` 是 ARM 裸机程序，不用 Linux：复位 PL 视频逻辑，EMIO bit-bang SCCB 读 ID，调用 `ov5640_init(640,480,2570,980)`，配置 `run_vdma_frame_buffer()` 写通道，随后初始化 HDMI DisplayCtrl/VTC/动态时钟，再启动读通道，最后打印两路 DMA 状态。`emio_sccb_cfg/` 实现低层 start/stop、字节发送与 ACK；`ov5640/ov5640_init.c` 保存验证过的寄存器写序列；`vdma_api/` 封装 XAxiVdma 读写几何、帧地址与启动；`dynclk/` 与 `display_ctrl_hdmi/` 设置 HDMI 显示模式。那些 C 文件使用 Xilinx standalone BSP 的 `xparameters.h`、`Xil_Out32()`、`XAxiVdma`，**不能直接作为 PetaLinux 用户程序编译**。

## 3. ATK-MD0430 LCD 由彩条到帧差

阶段 1 的 `bringup/atk_md0430_colorbar/atk_md0430_colorbar_top.v` 在 FPGA 里执行 LCD 复位、NT35510 寄存器初始化和彩条，`lcd8080_write_cycle.v` 保证 16 位 8080 总线片选、RS、WR 的准备/保持时序。`tb_atk_md0430_colorbar.v` 只验证协议/计数等仿真行为；板上肉眼彩条及电气时序仍要另验。参考引脚见 `docs/ATK_MD0430_V2_J7_接线表.md`，综合时用 `constraints/studyboard_lcd_top.xdc`。

阶段 2 的 `scripts/create_studyboard_lcd_project.tcl` 复用摄像头和 VDMA，但将检测模块换为 `frame_difference_axis_bram.v`、显示尾端换为 `axis_nt35510_display.v`。BRAM 版只存 80×60=4,800 个亮度样本，同步读上一帧样本并写入本帧，做差异阈值、空间去噪、运动点统计和框；`S1_AXIS` 保留历史 BD 接口兼容，但不再依赖“正在改写的 DDR 上一帧”作比较。参数 `DIFF_THRESHOLD`、最小/最大运动样本数等在 RTL 常量中；改它们是 PL 重综合/验证任务，不是修改 `run-face-lcd.sh` 的 `confidence=0.25`。`tb_frame_difference_axis_bram.v` 与 `scripts/simulate_frame_difference_axis_bram.tcl` 覆盖 RTL 行为，`synthesize_frame_difference_axis_bram.tcl` 观察资源。

`axis_nt35510_display.v` 先复用阶段 1 的初始化/彩条，`lcd_bl` 握手后交给 `axis_lcd_frame_writer.v`。后者先清 800×480 屏幕，再将 640×480 视频放在水平偏移 80、垂直偏移 0 的窗口；由 AXIS `TUSER/TLAST` 对齐帧/行，生成 LCD 8080 命令和 RGB565 写周期。仿真文件 `tb_axis_lcd_frame_writer.v` 和对应 Tcl 检查小尺寸计数/协议；真正颜色顺序、屏幕引脚和撕裂仍需实物确认。

`SDK/ov5640_lcd_frame_difference/main.c` 是这条裸机 LCD 路线的 PS 启动器：重置 PL→SCCB 配置 OV5640→清零三帧 DDR 并刷新 cache→启动 VDMA0 S2MM→启动 VDMA1 MM2S→打印 DMA 状态。代码注释清楚地把 NT35510 初始化和 RGB888→RGB565 交给 PL，而非该 C 文件。它使用 DDR 基址加 `0x01000000` 的固定裸机帧区；**Linux 运行时绝不能按此裸机地址手工写内存**，Linux 驱动有自己的 DMA 分配与内存管理。

## 4. 2020.2 迁移与当前 Linux 的联系

`scripts/create_studyboard_lcd_2020_2_project.tcl` 和 `build_studyboard_lcd_2020_2_project.tcl` 用独立 `build/v20/lcd20.xpr` 创建/构建 Vivado 2020.2 设计并导出含 bitstream 的 XSA；脚本刻意不覆盖 2018.3 的工程。PetaLinux 2020.2 可据相配的硬件描述生成 device tree/镜像，再叠加 `studyboard-frame-capture.dtsi`，使内核看到两条 VDMA 通道。当前仓库脚本说明了**可能的构建来源**，但没有仅凭源码就能证明板上 `BOOT.BIN` 正在使用哪个 `.bit`；需要保留固件哈希/XSA 来源才能作审计结论。

Linux 时代，`linux/ov5640_init/ov5640-init.c` 替代裸机 `emio_sccb_cfg/` 的启动职责（通过 `/dev/i2c-0`），`linux/studyboard_frame_capture/studyboard-frame-capture.c` 替代裸机对 XAxiVdma 的直接管理（通过 DMAEngine/vb2），而 `linux/studyboard_ncnn_face/*.cpp` 新增了 CPU 上的人脸检测与红框。**PL 运动框和用户态人脸框是两套不同算法**：前者只说明变化区域，后者识别 face 类。观察到白色运动框和红色人脸框同时存在时，先确认当前 bitstream 确实包含 PL 帧差，再分别检查两个来源；不要误以为 YOLO 产生两个颜色的框。

## 5. 改硬件时的依赖清单

| 想改的内容 | 牵动文件/接口 | 最低回归证据 |
| --- | --- | --- |
| OV5640 寄存器/帧率 | 裸机源表或 Linux 表生成器、`ov5640-init`、PL 接收时序、VDMA 帧边界 | ID/关键读回、5 帧 V4L2、长测 IRQ/缺帧、实物画面 |
| 分辨率、色深、字节序 | 相机表、`ov5640_capture_data`、RTL、VDMA hsize/stride、DTS、驱动 `sizeimage`、模型逆映射、LCD 画窗 | 构建时序、payload/颜色/行对齐、屏上框坐标 |
| PL 帧差阈值/框 | `frame_difference_axis_bram.v`、仿真 Tcl/testbench、bitstream/XSA | RTL 仿真、综合资源/时序、板上白框；不能以 NCNN mAP 代替 |
| LCD 8080 写时序 | `lcd8080_write_cycle.v`、`axis_lcd_frame_writer.v`、LCD XDC | 彩条、协议仿真、实物边沿/颜色/长测 |
| Linux 仅改人脸阈值/模型输入 | `run-face-lcd.sh`、匹配 NCNN 模型/用户态代码 | 无需重编 PL；PC/板端精度与性能比对 |

早期帧差教程已对时钟域、反压、地址、BRAM 算法和 Vivado/SDK 运行给出长篇推导，见 `docs/STUDYBOARD_OV5640_帧差项目新手教程.md` 和两份 `ATK_MD0430` 阶段文档。本章的目的，是让你在读那些资料时始终知道**当前 Linux 人脸程序用到什么、没有用到什么**。
