# StudyBoard OV5640 → V4L2 → NCNN 人脸检测 → LCD 开发记录

初版记录：2026-10-12；补充实测：2026-10-13。本文是**实测状态快照**，供后续接续开发和整理教程；不表示长期可靠性、性能优化和开机自启已经全部验收。本文是 Zynq Linux 人脸检测/LCD 支线；仓库原有的 HDMI 帧差实验属于另一阶段，接口和验证结果不要混用。

## 1. 当前进度

| 项目 | 证据与结论 |
| --- | --- |
| OV5640 | `/dev/i2c-0`、地址 `0x3c`、ID `0x5640`；248 项初始化后关键寄存器、640×480 有效尺寸和 2570×980 帧总量读回通过。I²C 偶发 NAK 已用单寄存器重试恢复。 |
| Linux 采集 | `/dev/video0` V4L2 MMAP，640×480 BGR24；多次 300 帧测试约 28.5 fps，`sequence_gaps=0`、`bad_payloads=0`。 |
| 预处理 | 独立 224×224 预处理程序完成 300 帧测试；实际人脸模型另用 320×320 letterbox。 |
| 模型 | YOLOv8n 单类人脸模型训练、NCNN 导出完成；板端推理出现人脸框，保存图像目视核对正确。 |
| LCD `matched` | **实物通过**：LCD 显示本次推理原图及红色检测框。 |
| LCD `preview` | **长测与手动恢复通过**：9000/9000 帧、314.828 秒、28.59 fps，零缺口/坏帧，LCD 发布 4485 帧，退出码 0，屏幕全程更新；同次开机 Ctrl+C 后再启动通过。用户分别确认正常重启和断电再上电后手动运行均能检测显示；其中一次完整开机日志见第 3.8 节。 |
| 部署 | 已知可用运行包保存在 SD ext4，掉电后不应依赖 `/home/root` 上传目录；启动仍需手动执行脚本。 |

核心区别：**采集 28.5 fps ≠ 模型推理 28.5 fps ≠ 检测框每秒更新 28.5 次**。当前双核 NCNN 约每秒推理 1 次。`skipped_stale` 是只处理最新帧的设计，不等于 V4L2 丢帧；采集完整性看 `sequence_gaps` 和 `bad_payloads`。

## 2. 硬件、内核与应用的链路

```text
OV5640（DVP，传感器配置 RGB565）
    → PL 视频接收/像素转换 → VDMA0 S2MM → DDR
    → 内核 V4L2 /dev/video0（640×480 BGR24，vb2 MMAP）
    → 用户态 DQBUF → 复制到双槽“最新帧”邮箱 → 尽快 QBUF
         ├─ 采集线程：preview 时将最新原图与最近一次框送往 LCD
         └─ 推理线程：320×320 letterbox → NCNN YOLOv8n → NMS
                   → 框坐标映射回 640×480 原图 → 画红框
    → 驱动专用 lcd_frame（完整 BGR24）/lcd_commit
    → 独立显示缓冲 → VDMA1 MM2S → PL LCD 控制器 → LCD
```

- PL 逻辑像素可叫 RGB888，但 Zynq 小端内存中的连续三个字节是 **B、G、R**。V4L2 fourcc 为 `BGR3`，行跨度 `640×3=1920` 字节，一帧 `1920×480=921600` 字节。`lcd_frame` 接收原始 BGR24，不能写 PPM 文件头或 PNG 压缩数据。
- V4L2 是**内核提供的标准采集接口**；测试程序和 NCNN 应用在用户空间消费它。它可取代正常应用的自定义 sysfs 抓帧路径，但不取代 OV5640 初始化、PL、VDMA、LCD 接口或用户态推理。
- legacy sysfs `running=1` 与 `/dev/video0` 不能同时占有 VDMA0 S2MM。`lcd_frame`、`lcd_commit`、`lcd_status` 是本项目驱动专用扩展，既不是标准 V4L2 output，也不是 `/dev/fb0`。
- 采集 vb2 缓冲与显示 coherent 缓冲分开。应用拿到完成的采集帧后复制，并及时 `QBUF`；不在摄像头正在写的缓冲上画框。
- 模型输入固定 `in0 [1,3,320,320]`，输出 `out0 [1,5,2100]`，类别 0 为 face。640×480 缩放到 320×240，上下各补 40 像素，转 RGB float32 CHW 并乘 `1/255`。后处理包括置信度、单类 NMS、逆 letterbox 与坐标裁剪。

### LCD 两种模式

| 模式 | 屏幕内容 | 当前结论 |
| --- | --- | --- |
| 默认 `matched` | 参与**本次**推理的原图 + 本次检测框，图框严格对应。 | 物理 LCD 上红框已确认，约每次推理发布一张。 |
| `STUDYBOARD_LCD_MODE=preview` | 新一些的采集图 + 最近一次检测框；默认每 2 个采集帧尝试发布，框比当前图旧超过 60 个采集序号时隐藏。 | 300 帧短测有 135 次发布；不会加快推理，移动时框可能滞后。 |

第一张带框图由推理线程发布并启动连续 MM2S。用户态分块写显示后台缓冲；`lcd_commit=1` 后驱动把完整帧复制到前台，VDMA1 持续读取它。旧的有限单帧 MM2S 方案虽有 DMA 完成回调，但实物 LCD 没形成可见图像，所以改为 `frm_cnt_en=0` 连续运行。当前整帧复制可能产生瞬时撕裂；严格无撕裂的帧边界切换尚未开发。

## 3. 各阶段验证记录

### 3.1 传感器、VDMA 与 V4L2

1. `ov5640-id` 读到 `0x5640`。`ov5640-init` 248 项写入并关键寄存器读回 PASS。**ID/寄存器 PASS 只能证明 SCCB 配置，不等于 DVP 像素进入 VDMA。**
2. 先经自定义 sysfs/VDMA 确认 PL 链路，再建 `/dev/video0` MMAP 采集。一次 `studyboard-v4l2-test /dev/video0 1000`：1000/1000、54.162 秒、18.46 fps、零序号缺口、零坏 payload、999 次摘要变化。该程序逐帧算摘要，有额外 CPU 开销，不能据此说摄像头只有 18 fps。
3. 后续轻量应用多次约 28.5 fps；matched 闭环测试 300/300、10.505 秒、28.56 fps，preview 闭环 300/300、10.508 秒、28.55 fps，均无序号缺口/坏 payload。

### 3.2 独立的 224×224 预处理里程碑

`studyboard-v4l2-preprocess` 完成 BGR24→RGB24→224×224 双线性缩放→`[0,1]` CHW float32。300/300 帧、10.528 秒、28.50 fps、零缺口/坏 payload；平均处理 22.705 ms（颜色 3.742、缩放 13.482、归一化 5.481 ms），纯处理上限约 44.04 fps。**这不是当前 YOLOv8n 的 320×320 letterbox 输入。**

### 3.3 训练、导出、板端推理

- Windows 环境：Ultralytics 8.4.135、Python 3.11.16、PyTorch 2.12.1+cu126、GTX 1650 4 GB。数据目录 `E:\Yolo\ultralytics-main\newface`；使用 YOLOv8n 预训练权重。训练 89 个 epoch 后早停，最佳权重来自 epoch 59，`best.pt` 约 6.2 MB。
- 当时验证集仅 **19 张图、19 个人脸实例**：P 0.996、R 1.000、mAP50 0.995、mAP50-95 0.944。只能说明该小验证集表现好，**不能推断不同光照、姿态、距离和多人场景都有同等可靠性**。后续需独立测试集。
- `best.pt` 导出为 NCNN `model.ncnn.param`/`model.ncnn.bin`（导出目录约 11.6 MB）。PC 第一次推理失败原因是 `camera_samples` 输入路径不存在，并非模型导出损坏。
- 复现锁定信息：Ultralytics 8.4.135、pnnx 20260526、NCNN Python 包 1.0.20260526、板端 NCNN 源码 tag 20260526。部署清单中的 SHA-256：`model.ncnn.param` 为 `fdeca3c6a48921d30c613e20edcea602d00ba8c49c512bc9e5462ec1dccd244c`，`model.ncnn.bin` 为 `763469a0d1e821686d0f17c3bd1b37e2e2e39023684179e2f7bba77af899359d`。模型/运行时更新时先核对这份版本锁与文件校验值。
- PetaLinux 2020.2 SDK 的 ARM hard-float 工具链、NCNN SIMPLEOMP 已编译通过。板端单线程平均推理约 1390 ms，0.72 fps；双线程 SIMPLEOMP 独立测试平均约 868 ms，1.15 fps。摄像头对着人时，检测给出约 0.45～0.84 的置信度并在保存图像上看到正确红框；没有人时 `faces=0` 合理。

### 3.4 LCD `matched` 实测

一次 300 帧闭环：采集 10.505 秒、28.56 fps、零 gap/坏帧；推理 12 次、跳过过时帧 288 次、累计检出人脸 6 次；平均预处理 19.32 ms、推理 901.96 ms，按推理耗时约 1.11 fps；LCD 发布 12 帧，平均发布 19.17 ms。用户已在**物理 LCD** 看见摄像头图像与人脸红框。这证明采集→推理→显示闭环跑通，不代表实时 28.5 fps 检测。

### 3.5 LCD `preview` 短测

SD 上独立试验目录运行 `STUDYBOARD_LCD_MODE=preview sh ./start-face-lcd.sh 300`，前置的 5 帧 V4L2 检查通过。正式运行 300/300、10.508 秒、28.55 fps、零 gap/坏帧；推理 11 次、跳过过时帧 289 次、检出人脸累计 4 次；平均推理 1007.07 ms、约 0.99 fps；LCD 发布 135 帧。用完整采集时长折算约 12.85 次发布/秒；第一帧推理之后才启动预览，这**不是稳态屏幕刷新率的严格测量**。用户看到屏幕一段时间后暂停：`300` 是有限抓帧参数，约 10.5 秒后程序主动退出并停止显示；凭这份日志不能说程序卡死。后续更长时间的结果见第 3.7 节。

### 3.6 2026-10-13：`0` 模式停止与同次开机再启动

用户在物理 LCD 观察到画面持续更新。第一次运行 `STUDYBOARD_LCD_MODE=preview sh ./start-face-lcd.sh 0`：启动脚本对原有模块和 LCD 模块分别抓 5 帧成功；主程序运行 363 帧、12.736 秒、28.50 fps、`sequence_gaps=0`、`bad_payloads=0`，LCD 发布 167 帧，平均发布 11.13 ms，推理 13 次、平均 1017.62 ms、约 0.98 fps。Ctrl+C 后打印正常汇总，紧接着的 `continuous_return=0`。

随后**同一次开机**再次启动 preview 并抓 300 帧：300/300、10.532 秒、28.48 fps、零 gap/坏帧；LCD 发布 135 帧、平均发布 11.21 ms，推理 11 次、平均 1010.36 ms、约 0.99 fps。第二次日志包含完整汇总，但粘贴文本截止于 `echo "restart_return=$?"`，未显示其返回值；不能把未见到的返回值记成 0。两次测试证明超过 300 帧不会因先前短测而自动停、Ctrl+C 后资源能释放并立即再次工作；**12.736 秒不是几分钟长测，也不是重新上电复测**。

### 3.7 2026-10-13：preview 9000 帧长测

沿用 SD 试验包，执行 `STUDYBOARD_LCD_MODE=preview sh ./start-face-lcd.sh 9000` 并把完整输出保存到 `preview-long-20261013.log`。结果：9000/9000 帧、314.828 秒（约 5 分 15 秒）、28.59 fps、`sequence_gaps=0`、`bad_payloads=0`；NCNN 处理 293 帧、按策略跳过过时帧 8707 帧、总检测计数 124（**不是** 124 个不同的人），平均预处理 20.24 ms、推理 1053.51 ms、按推理耗时约 0.95 fps；LCD 发布 4485 帧，平均发布 10.16 ms，退出码 0。用户目视确认屏幕全程持续更新。4485 次/314.828 秒约为 14.25 次发布/秒，符合默认每 2 个采集帧尝试发布、首次推理完成后才启动预览的设计。**这一项证明一次约 5 分钟运行稳定；它本身不证明跨重启恢复，后者单独记录于第 3.8 节。**

### 3.8 2026-10-13：正常重启与重新上电后的手动恢复

用户分别确认：正常重启后，以及断电再上电后，SD 试验包均能**手动启动并在实物屏幕正常检测显示**。所附文本展示了其中一次从登录开始的完整启动测试，但没有分别给两种重启方式提供独立日志；因此不把一份日志描述成“两次都逐项记录”。该次日志中，OV5640 248 项初始化和关键寄存器读回通过；系统原有模块 5/5 帧、LCD 模块 5/5 帧，均零 gap/坏帧；应用 300/300 帧、10.505 秒、28.56 fps、零 gap/坏帧，NCNN 处理 11 帧、累计 3 次人脸检测，LCD 发布 135 帧、平均发布 11.03 ms，`return=0`。这支持 SD 运行包跨重启保留和**手动恢复流程**可用；并不意味着已经设置开机自动启动。

### 3.9 2026-10-13：约 1 fps 推理的第一轮优化准备（板端待验）

9000 帧长测显示瓶颈是 NCNN 推理约 1053.51 ms/次，而不是预处理约 20.24 ms 或 LCD 发布约 10.16 ms。优先尝试**同一训练权重、缩小模型输入**，暂不重训、更换网络、量化或修改内核。已将训练所得 `best.pt` 导出为 192×192、224×224、256×256 的 NCNN 模型；原 320×320 模型仍作基线。应用新增可选的输入尺寸参数，按 8/16/32 三个检测步长计算输出候选数：192→756、224→1029、256→1344、320→2100，并检查实际输出尺寸，不匹配即报错。

对复制出的 108 张/108 个人脸标注测试图，使用相同权重进行方形输入测试：320、256、224、192 的 PyTorch mAP50 都是 0.995；mAP50-95 分别约 0.9628、0.9636、0.9726、0.9577。192/224/256 的导出 NCNN 模型在 PC CPU 端验证，mAP50-95 与对应 PyTorch 结果一致。**测试集小且每张仅一张已标注人脸；PC 速度不能外推为 Zynq Cortex-A9 速度，224 的较高 mAP 也不能说明各种实际场景一定优于 320。**

独立试验包位于 `output/studyboard_face_speed_20261013/`，内含一次性应用构建脚本、三个小尺寸模型、独立 SD 安装脚本和 `320→224→256→192→320` 对比脚本。构建脚本复用现有 PetaLinux SDK 与 NCNN SIMPLEOMP，只编译应用，不重新编译 NCNN/内核、不更新 `image.ub` 或 SD BOOT 分区；安装脚本从已验证的 preview 包复制到新的 SD 目录，不修改原包。**目前完成的是 PC 模型验证、源码和部署准备；ARM 交叉编译、开发板实际推理耗时、物理 LCD 框位置与稳定性还没有本轮验证。**具体操作和回退见该试验包的 `README.md`。

### 3.10 2026-10-13：开发板多尺寸速度实测与短时目视复测

交叉编译、SHA256 校验和独立 SD 安装均通过。试验目录为 `/media/sd-mmcblk0p2/face-lcd-speed-20261013-01`，原 preview 目录未覆盖。第二次调用安装脚本得到 `trial already exists; preserving`，这是防覆盖保护，第一次安装已成功。五组各抓 600 帧、约 21 秒，均约 28.56 fps、`sequence_gaps=0`、`bad_payloads=0`；LCD 每组发布 285–294 帧，推理结果如下：

| 输入尺寸 | 平均推理 ms | 有效推理 fps | 处理帧数 | 备注 |
| --- | ---: | ---: | ---: | --- |
| 320 首轮 | 1024.28 | 0.98 | 21 | 基线 |
| 224 | 549.81 | 1.82 | 38 | 约为 320 的 1.86 倍 |
| 256 | 694.11 | 1.44 | 30 | 约为 320 的 1.48 倍 |
| 192 | 383.37 | 2.61 | 54 | 约为 320 的 2.67 倍 |
| 320 末轮 | 1025.71 | 0.97 | 21 | 与首轮接近，未见明显性能漂移 |

用户随后对 224 做 1238 帧/43.333 秒观察：采集 28.57 fps、零 gap/坏帧，推理处理 77 帧；对 192 做 530 帧/18.571 秒观察：采集 28.54 fps、零 gap/坏帧，推理处理 48 帧，并反馈两档人脸框稳定性差不多。这些处理帧数除以采集时长分别约 1.78 和 2.58 次/秒，与上表相符。**两段时长和画面不同，`total_faces=54` 与 `28` 不是漏检率或模型准确率；192 的 18.6 秒目视复测也尚未替代长时间稳定性测试。**在“优先提升速度”且当前目视效果相近的条件下，可暂用 192 作为试验目录的运行参数，保留 224 和原 320 作质量/故障回退；暂不据此覆盖已验证基线或宣称达到流畅的检测框帧率。

## 4. 构建、部署与重启使用

### Ubuntu/Windows 侧

- Ubuntu VM 的 PetaLinux 工程 `/home/ubuntu/petalinux/ALIENTEK-ZYNQ`。`petalinux-build --sdk` 最初受 `BB_NO_NETWORK` 和下载缓存缺失影响（chrpath、Linux headers、unfs3 等），补齐后 3287 个任务全部成功，生成 `images/linux/sdk.sh`。`petalinux-package --sysroot` 后使用 **`images/linux/sdk/environment-setup-cortexa9t2hf-neon-xilinx-linux-gnueabi`**；此前使用 `components/yocto/...` 环境曾遇编译器找不到。SDK 警告 `LD_LIBRARY_PATH` 时先检查并清理再 source。
- NCNN ARM 运行时采用 SIMPLEOMP；本 SDK 未提供可直接使用的 `libgomp`/`omp.h` 组合。可执行文件是 ARM EABI5 hard-float，解释器 `/lib/ld-linux-armhf.so.3`，RPATH `$ORIGIN/lib`，相邻 `lib/` 放匹配 SDK 的 `libstdc++.so.6`、`libgcc_s.so.1`。
- LCD 打包脚本只重建必要模块和应用，不重编 NCNN、不重新生成 `image.ub`、不刷 SD。BitBake 报成功但 `.ko` 找不到时，应查 recipe 工作目录与 `rm_work`；对整个 `build/tmp` 用 `find` 可能遇 Yocto 符号链接循环，不能以此断言构建失败。
- 网线直连：Windows 有线网卡 `192.168.10.1/24`、开发板 `eth0=192.168.10.2/24`，通常无 DHCP。重启后板端若无 IP，通过**串口**检查 `ip -4 addr show dev eth0`，再执行 `ip link set eth0 up` 和 `ip addr add 192.168.10.2/24 dev eth0`；在当前 SSH 会话里不要随意 `ip addr flush`。
- Dropbear 使用旧 `ssh-rsa` host-key 算法。Windows OpenSSH 需 `-o HostKeyAlgorithms=+ssh-rsa`，旧版服务器 `scp` 还需 `-O`。重启后密钥若变化，先通过可信串口核对指纹，再更新 `known_hosts`；不要直接关闭主机密钥校验。MobaXterm 的 `xauth: command not found` 是 X11 转发警告，不妨碍本任务的 shell/文件传输。

### SD 持久化和手动启动

`/home/root` 中先前出现重启后文件缺失。已知可用基线在 SD ext4：`/media/sd-mmcblk0p2/face-lcd-recovery-20261012-01`；preview 试验版在 `/media/sd-mmcblk0p2/face-lcd-preview-20261012-01`。两者独立，**不必每次掉电都重新上传完整模型/驱动**，但上电后要检查 SD 是否挂载、运行包是否齐全。当前没有开机自启服务。

板端重启后手动执行基线；`start-face-lcd.sh` 会核对文件和内核版本、同一 boot 的传感器状态，先用系统模块抓 5 帧，再用 LCD 模块抓 5 帧；失败会停止，避免盲目重试初始化。

```sh
cd /media/sd-mmcblk0p2/face-lcd-recovery-20261012-01
sh ./start-face-lcd.sh 0
```

`0` 持续运行，Ctrl+C 停止；`300` 是约 10.5 秒的短测。试验版在**另一目录**运行：

```sh
cd /media/sd-mmcblk0p2/face-lcd-preview-20261012-01
STUDYBOARD_LCD_MODE=preview sh ./start-face-lcd.sh 0
```

如 preview 不稳定，停止程序后回基线目录运行默认模式。应用运行时不要另用 sysfs 写 `running=1` 或 `display_running=1`。同一 boot 内反复运行不应反复 `ov5640-init`；`STUDYBOARD_FORCE_SENSOR_INIT=1` 不是常规恢复操作。

SD 的 BOOT 是 100 MB FAT 分区，数据在 ext4。曾出现 BOOT FAT dirty bit 和上电异常；已备份 BOOT 文件并用只读 `fsck.vfat -n` 诊断，但**不能据此认定 dirty bit 就是 DONE 灯不亮的根因**。不要无必要地覆盖 `BOOT.BIN`、`image.ub` 或重刷卡。

## 5. 遇到的问题与判断方法

| 症状 | 依据与处理原则 |
| --- | --- |
| `Timeout waiting for frame 0` | 旧系统模块和新 LCD 模块都曾无法完成 frame 0，S2MM IRQ 不增长；不应只怪 LCD 补丁。正常重启后传感器初始化一次，先旧模块 5 帧再新模块 5 帧，失败立即停止。 |
| VDMA 看似已启动仍无帧 | 测得 S2MM control `0x43000030=0x00017081`、status `0x43000034=0x00010000`、VSIZE=480、HSIZE=1920，但 IRQ47 未变。仅能说通道已配置而未完成帧；不能仅据此确定 DVP/PL/时钟具体故障。内核持有 DMA 时不要用 `devmem` 写寄存器。 |
| OV5640 写寄存器 NAK | 芯片 ID 可读，但如 `0x3108` 等写入曾失败。原版整体重跑 248 项不可靠；使用有限次数的**单寄存器**重试，最终还要检查 readback 与真实抓帧。 |
| ARM 程序报 `No such file or directory` | 既可能文件本身不在，也可能 ELF 动态解释器/库不在；此次 `/home/root` 文件重启后确有缺失。先 `ls -l`，再 `file` 和 `/lib/ld-linux-armhf.so.3 --list`。 |
| ping 通但 SSH/scp 失败 | ping 只说明 IP 可达；旧 Dropbear 要 `ssh-rsa`，host key 也可能随重启变化。通过串口核对指纹后再处理 `known_hosts`，`scp -O` 兼容旧协议。 |
| LCD 约 10 秒后停止 | 传入 `300`，到帧数后主动清理显示；要持续运行传 `0`。分别看退出码、`capture_summary` 和 `lcd_summary`。 |
| 控制台文件名似乎夹入驱动日志 | kernel printk 与用户态输出在同一行交错，例如 `face-lcd-last.ppm` 被打断；用 `ls` 查实际文件，勿据此判断路径损坏。 |
| 混淆计数器/性能 | `callback_count` 是设置描述符回调次数，不等于持续帧率；`time` 包括初始化和清理；`skipped_stale` 是跳过旧帧策略。分别看 `/proc/interrupts`、V4L2 sequence/payload、采集 FPS、推理耗时、LCD 发布数。 |

抓帧/黑屏时的排查顺序：物理供电和 DONE → 串口启动日志 → 网口/IP → 传感器 ID 与初始化 → 旧模块 5 帧 → LCD 模块 5 帧 → NCNN/LCD 应用。传感器读回通过而两种驱动均收不到 frame 0，应回到 DVP/PL/VDMA 输入链路，不要先调模型阈值。失败现场保留以下只读信息：

```sh
P=/sys/bus/platform/devices/studyboard-frame-capture
cat "$P/running" "$P/display_running"
test -r "$P/lcd_status" && cat "$P/lcd_status"
grep xilinx-dma-controller /proc/interrupts
dmesg | tail -n 60
```

只有采集正常且图像内容正确，`faces=0` 才有模型层面的意义；`lcd_summary` 大于零也不能单独证明屏幕颜色、线对齐、框位置正确，仍需实物目视。

## 6. 关键源码、运行包与教程提纲

| 用途 | 仓库路径 |
| --- | --- |
| V4L2 与 LCD 驱动 | `linux/studyboard_frame_capture/studyboard-frame-capture.c`、同目录 `README.md` |
| OV5640 初始化 | `linux/ov5640_init/ov5640-init.c` 与寄存器表 |
| 推理与 LCD 应用 | `linux/studyboard_ncnn_face/studyboard-ncnn-face.cpp`、`face_detector.cpp`、`lcd_output.cpp` |
| 启动、试验安装 | `linux/studyboard_ncnn_face/start-face-lcd.sh`、`run-face-lcd.sh`、`install-preview.sh` |
| 详细部署/版本锁 | `linux/studyboard_ncnn_face/LCD_部署说明.md`、`README.md`、`DEPLOYMENT-MANIFEST.txt` |

正式教程可按“硬件与启动介质 → OV5640/I²C/DVP → PL/VDMA 与字节序 → Linux 驱动及 V4L2 缓冲所有权 → 图像预处理 → 数据集/训练/导出 → ARM SDK/NCNN → LCD 接口 → SD 部署与网络 → 故障诊断 → 性能和可靠性验收”组织。每章保留命令、预期输出、失败判断点、实测边界，不只保存成功截图。

## 7. 下次工作的验收边界

1. `0` 模式 Ctrl+C 后同次开机再启动、9000 帧约 5 分钟长测以及正常重启/重新上电后的**手动恢复**均已验证。若下一阶段需要开机自启，应先做非阻塞、可回退的启动服务：等待 SD 挂载，保留现有 5 帧预检，失败记录日志且不无限重试，不改动 BOOT 分区或已知可用运行包；部署前单独确认需求。
2. 比较默认 matched 与 preview 的实际体验；预览约每秒十余次发布**不等于**十余次推理，框移动时可能滞后。
3. 补更大的独立测试集，覆盖不同光照、角度、远近和多人。当前 19 张验证图不足以证明泛化。
4. 再决定开机自启、IP 与 SSH host key 持久化。先保证 SD 挂载、驱动/内核版本、初始化时序、失败回退可靠，不做无条件启动重试。
5. 约 1 fps 推理是主要实时性瓶颈；若需求更快，再分别测试模型尺寸、量化、算子或硬件加速。严格无撕裂是独立的底层 DMA 改进题。

**截至 2026-10-13 的安全结论：**摄像头→V4L2→NCNN 人脸框→LCD 的 matched 闭环已实物跑通；preview 短测、Ctrl+C 后再启动、9000 帧约 5 分钟长测，以及正常重启/断电再上电后的手动启动均通过。开机自动启动尚未配置；模型泛化和约 1 fps 的推理性能仍是后续工作。
