# 下一阶段：把人脸检测图像显示到 LCD

## 已验证后的重启启动方式（SD 卡运行包）

2026-10-12 实物验证：300/300 帧抓取成功，LCD 能看到人脸红框；
摄像头约 28.55 fps，而当前 NCNN 推理及带框画面更新约 1.10 fps。
运行包已保存在 `/media/sd-mmcblk0p2/face-lcd-recovery-20261012-01`，
掉电后不用再上传模型、应用或驱动。

新增的 `start-face-lcd.sh` 是**重启后手动执行的一键启动脚本**，不是开机自动服务：

```sh
cd /media/sd-mmcblk0p2/face-lcd-recovery-20261012-01
sh ./start-face-lcd.sh 0
```

`0` 为持续运行，`300` 为抓取 300 帧后退出。脚本校验运行包和内核版本，
只在本次开机尚未初始化时调用包内带 SCCB 单寄存器重试的 `ov5640-init`；
先用系统原有驱动抓取 5 帧，再热加载 LCD 驱动抓取 5 帧，最后启动检测显示。
任一抓帧检查失败即停止，不会无条件反复初始化摄像头；LCD 驱动加载或
抓帧失败时尝试恢复系统原有驱动。已经加载 LCD 驱动、但本次开机没有
传感器初始化记录时脚本会拒绝再次配置摄像头，要求先正常重启。

这一步只需把脚本放入 SD 运行包，**不需要重编 NCNN、内核模块或模型**。
以后的完整打包构建会同时生成带重试的 `ov5640-init` 和该脚本。
暂不自动写入启动项；先验证重启后一键启动，再决定是否固化开机自启。

## 可选：较流畅的预览＋低频更新红框

新用户态应用增加 `preview` 模式，**不改内核模块或模型**。保持 V4L2
采集与 NCNN 推理双线程；推理线程先发布第一张带框图，之后采集循环每
2 帧尝试发布一张最新预览图，并把最近一次推理得到的框画在这张图上。
框若超过 60 个采集序号（约 2 秒）仍无新结果，则暂时隐藏。采集仍先
归还 V4L2 缓冲，再进行 LCD 写入；预览没有改写 DMA 捕获缓冲。

预览不是更快的模型推理：2026-10-12 的 300 帧短测中，采集 28.55 fps、
无 sequence gap/坏 payload，LCD 发布 135 帧、推理平均 1007.07 ms；
这不等于每秒检测 13 次。红框仍约每秒更新一次，移动时会滞后，
一次 9000 帧长测已通过，正常重启和重新上电后的手动恢复也已由用户确认。
默认模式继续使用
已经验证成功的
`matched`（推理对应的原图＋本次检测框）；部署新版应用后可以试：

```sh
cd /media/sd-mmcblk0p2/face-lcd-preview-20261012-01
STUDYBOARD_LCD_MODE=preview sh ./start-face-lcd.sh 300
```

若 `capture_summary` 出现 sequence gap、帧率明显下降或显示异常，
可先用 `STUDYBOARD_LCD_MODE=preview STUDYBOARD_PREVIEW_STRIDE=3 sh ./start-face-lcd.sh 300`
降低 LCD 更新频率（允许 1～8），无需重编译；仍不理想就直接回到
默认的 `sh ./start-face-lcd.sh 300`，不必回退内核或模型。
**旧版 `studyboard-ncnn-face-lcd` 不支持 preview 参数；先编译并部署
新版应用和新版 `run-face-lcd.sh`，再启用该模式。**

只构建、传输一次用户态试验包，不覆盖当前成功运行的 SD 基线：

Windows PowerShell（确认目标目录此前不存在）：

```powershell
Copy-Item -LiteralPath 'E:\zynq\github\ZYNQ_ov5640_hdmi_frame_difference-master\output\studyboard_face_lcd_step1' -Destination 'D:\share\studyboard_face_preview_20261012' -Recurse
```

Ubuntu VM 新终端（无需 source PetaLinux，脚本自己 source SDK）：

```bash
bash /mnt/hgfs/share/studyboard_face_preview_20261012/build-preview-app.sh
```

Windows PowerShell（只上传一个包含新应用与脚本的目录，不重传模型/驱动）：

```powershell
scp -O -o HostKeyAlgorithms=+ssh-rsa -r 'D:\share\studyboard_face_preview_20261012\preview-update-20261012-01' 'root@192.168.10.2:/media/sd-mmcblk0p2/'
```

开发板：

```sh
cd /media/sd-mmcblk0p2/preview-update-20261012-01
sh ./install-preview.sh
cd /media/sd-mmcblk0p2/face-lcd-preview-20261012-01
STUDYBOARD_LCD_MODE=preview sh ./start-face-lcd.sh 300
```

安装脚本先校验上传文件，再在同一 SD 分区复制现有成功运行包为新目录，
仅替换用户态应用与两个脚本，最后重新生成并验证 SHA256SUMS。
它不会覆盖 `face-lcd-recovery-20261012-01`。试验结束后用原目录运行
默认模式即可回到当前已验证状态。上述流程已完成一次 300 帧上板短测：
300/300、10.508 秒、采集 28.55 fps、LCD 发布 135 帧。传参 `300`
时约 10.5 秒后停屏是正常退出；需传 `0` 才能做连续运行试验。
2026-10-13 再运行 9000 帧、314.828 秒、28.59 fps，零缺口/坏帧，LCD
发布 4485 帧，退出码 0；用户确认屏幕全程更新。之后正常重启和重新
上电均能从 SD 运行包手动恢复检测显示；尚未安装开机自启服务。实测边界和待办
集中记录在仓库根目录的
`studyboard_face_lcd_devlog_2026-10-12.md`。

## 本次目标与当前状态

你已在开发板验证：V4L2 采集约 28.55 fps，300 帧无 sequence gap/坏 payload；
NCNN SIMPLEOMP 双核平均推理约 868 ms，能检测人脸。单核约 1393 ms，双核约快 1.60 倍。

本次新增驱动和应用代码，把“实际参与本次推理的原图＋本次人脸框”送到 LCD。
不是把旧框叠到另一张实时图，也不是 28.5 fps 的人脸检测。预计显示更新约每秒一次，
实际速度以包含显示耗时的上板结果为准。

本地已验证：完整应用使用真实 NCNN 头文件的 Linux C++ 语法检查；LCD 用户态传输的
完整帧、多次更新、错误尺寸、缺失接口、提交失败和停止操作测试（含地址/未定义行为检测）。
测试后端是普通文件，不代表 sysfs/VDMA/LCD 已验证。内核模块必须在下面的 PetaLinux
工程中编译，物理屏幕必须上板检查。

## 数据为什么这样走

```text
OV5640 → PL → VDMA0 S2MM → V4L2 的 vb2 DDR 缓冲
                                ↓ DQBUF、复制、立即 QBUF
                       用户态 latest-frame mailbox
                                ↓
                  letterbox → NCNN → NMS → 原图画红框
                                ↓ 完整 640×480 BGR24 图
                     lcd_frame 分块写入独立显示缓冲
                                ↓ lcd_commit=1
                     VDMA1 MM2S → PL LCD 控制器 → LCD GRAM
```

一帧仍然是 640×480×3=921600 字节。不能往 lcd_frame 写 PPM 文件头或 PNG 压缩数据。
应用直接写内存中的 BGR24 像素；PL 按原来的 RGB888→RGB565 通路驱动屏幕，不必修改 PL。

显示复用驱动原先用于 sysfs 诊断的三帧 coherent 内存；采集用另外的 vb2 内存。
不是新分配三帧，也不是让 CPU 在摄像头正在写的缓冲上画框。

实物验证表明本项目的 LCD AXI 接收端需要 VDMA1 持续运行；有限单帧模式虽然产生了
DMA 完成回调，却没有形成可见画面。因此修正版使用 `frm_cnt_en=0` 连续 MM2S：第一次
提交后，三个硬件 frame-store 都重复读取同一个前台缓冲；用户态分块写独立后台缓冲，
`lcd_commit=1` 再把完整后台帧一次性复制到前台。后续更新不再停止或复位 VDMA1。

这个 module-only 方案优先保证已上板验证过的持续显示时序。提交时的整帧内核复制可能
让一次 LCD 刷新看到短暂过渡，但下一次刷新必定是完整新帧；后续若需要严格无撕裂，
再给底层 Xilinx VDMA 驱动增加帧边界 park-frame 切换接口。

## 1. Ubuntu 虚拟机：一次编译驱动和应用

共享目录中本次源码包为 `/mnt/hgfs/share/studyboard_face_lcd_step1`。
旧模型和旧检测程序目录保持不变。

打开一个**新的 Ubuntu 终端**，不要先 source 交叉编译 SDK。脚本会在子 shell 中
为应用设置 SDK，避免污染 PetaLinux/BitBake 的环境。

```bash
source /opt/pkg/petalinux/2020.2/settings.sh
bash /mnt/hgfs/share/studyboard_face_lcd_step1/build-lcd-bundle.sh
```

脚本默认使用以下已验证路径：

- PetaLinux：`/home/ubuntu/petalinux/ALIENTEK-ZYNQ`
- 双核 NCNN：`/home/ubuntu/ncnn-zynq-20260526-simpleomp`
- 模型：`/mnt/hgfs/share/studyboard_ncnn_face/model`

若你的实际安装路径不同，可按顺序传入三个目录参数：

```bash
bash /mnt/hgfs/share/studyboard_face_lcd_step1/build-lcd-bundle.sh \
  /home/ubuntu/petalinux/ALIENTEK-ZYNQ \
  /home/ubuntu/ncnn-zynq-20260526-simpleomp \
  /mnt/hgfs/share/studyboard_ncnn_face/model
```

它只执行模块的 `petalinux-build -c studyboard-frame-capture` 和应用的 `make`，
**不重编 NCNN、不重建 image.ub、不刷 SD 卡**。修改 PetaLinux 模块源码前自动备份到
工程下 `lcd-source-backup.XXXXXX`，成功后生成共享目录里的 `board/`：

```text
board/
  studyboard-frame-capture.ko
  studyboard-ncnn-face-lcd
  KERNEL-RELEASE
  SHA256SUMS
  load-lcd-driver.sh
  run-face-lcd.sh
  face-model/model.ncnn.param
  face-model/model.ncnn.bin
  lib/libstdc++.so.6
  lib/libgcc_s.so.1
```

文件齐全再继续上传。若只有源码包、没有 board 目录，说明还没成功完成这一步。

## 2. Windows PowerShell：一次上传整个运行包

开发板保持已启动且能 SSH 登录，Windows 192.168.10.1，开发板 192.168.10.2。
在 Windows PowerShell 执行，**不是开发板 shell**：

```powershell
scp -O -o HostKeyAlgorithms=+ssh-rsa -r "D:\share\studyboard_face_lcd_step1\board" root@192.168.10.2:/home/root/face-lcd-step1
```

首次上传前 `/home/root/face-lcd-step1` 应不存在。若要再次上传更新，使用
`scp ... -r "D:\share\studyboard_face_lcd_step1\board\." root@192.168.10.2:/home/root/face-lcd-step1/`
或在 MobaXterm 文件栏把 board 内全部文件覆盖到该目录，避免出现多一层 board 的目录。
程序和 `lib` 必须保持相邻，因为可执行文件的库搜索路径为 `$ORIGIN/lib`。

## 3. 开发板 SSH：热加载并运行

先关闭其他正在使用摄像头的程序，再在开发板执行：

```sh
cd /home/root/face-lcd-step1
sh ./load-lcd-driver.sh
sh ./run-face-lcd.sh 300
echo "return=$?"
```

加载脚本先校验完整性和内核版本，再正常卸载旧模块并加载新模块，绝不强制卸载。
不会覆盖根文件系统中原先安装的模块。摄像头初始化已经包含在运行脚本里；脚本通过
`/proc/sys/kernel/random/boot_id` 保证同一次开机只执行一次 `ov5640-init`，后续运行直接
复用传感器配置，避免重复配置后 PL 接收端等不到新帧边界。

只有确认传感器配置丢失时才强制重新初始化：

```sh
STUDYBOARD_FORCE_SENSOR_INIT=1 sh ./run-face-lcd.sh 300
```

看着摄像头，缓慢移动人脸，检查 LCD 图像及红框随检测更新。一次 300 帧约采集
10.5 秒，最后推理和显示可能使总用时更长。运行结束后同目录有 `face-lcd-last.ppm`。

第一次通过后无需再次卸载驱动，直接连续运行：

```sh
sh ./run-face-lcd.sh 0
```

`0` 表示持续运行；Ctrl-C 正常退出。再执行同一命令应能恢复显示，不应需要重启。
程序运行期间不要另执行 `echo 1 > running` 或 `echo 1 > display_running`。

## 验收与故障信息

成功条件：屏幕显示几何完整、颜色正确的摄像头图像，人脸框与该图像匹配并持续更新；
`sequence_gaps=0`、`bad_payloads=0`、`lcd_summary: frames=...` 为正数，退出码为 0。
还必须验证退出后再次启动，不能只以“第一次亮屏”为通过。

`effective_inference_fps` 仍然是按模型推理耗时计算的指标，不等于实际 LCD 更新率；
新输出 `lcd: ... publish=...ms` 和 `lcd_summary` 单独报告显示提交开销。第一帧包含
连续 VDMA 的启动时间，之后主要是 sysfs 写入和前后台完整帧复制时间；`lcd_status`
应包含 `mode=continuous-copy`。

失败时保留从程序启动到错误的完整输出，并提供：

```sh
P=/sys/bus/platform/devices/studyboard-frame-capture
cat "$P/lcd_status"
grep xilinx-dma-controller /proc/interrupts
dmesg | tail -n 60
```

若提示 `lcd_frame` 不存在，说明仍是旧驱动；若加载报内核版本不匹配，不要强行 insmod，
应使用与当前上板系统对应的 PetaLinux 工程编译。若 DMA 超时，停止程序后再诊断；
强制停止后的 LCD 帧同步不保证已恢复，先不要反复运行来掩盖错误。

## 回退和掉电说明

停止应用后可恢复系统原来安装的模块：

```sh
rmmod studyboard_frame_capture
modprobe studyboard_frame_capture
```

旧的 `studyboard-ncnn-face-simpleomp` 和模型目录没有被覆盖。
模块热加载只在当前开机周期有效；板端以前出现过 `/home/root` 上传文件掉电后
消失的情况。因此当前已知可用包保存在 SD ext4 的
`/media/sd-mmcblk0p2/face-lcd-recovery-20261012-01`，preview 试验包保存在
`/media/sd-mmcblk0p2/face-lcd-preview-20261012-01`。重启后先检查 SD 已挂载和文件齐全，
再手动运行 `start-face-lcd.sh`；正常情况下不用每次重传模型、应用或驱动。
当前尚未安装开机自启服务，也未完成 preview 连续长测和推理优化。
