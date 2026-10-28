# 网口摄像头实时预览：独立试验版

这个版本只改用户态人脸程序；不改 FPGA、内核模块、模型、`image.ub` 或 SD 启动分区。不开启环境变量时，原程序的采集、推理和 LCD 行为不变。务必先在一个新的 SD 卡试验目录中运行，不覆盖已验证的启动包。

## 数据路径与上限

`OV5640 → PL/VDMA → /dev/video0 → 原有采集线程`，从采集线程每隔 6 帧复制一张原始 BGR24 图到网络专用双缓冲，再由独立线程通过 TCP 发送。PC 连接开发板 `192.168.10.2:5000`，运行 `pc_video_viewer.py` 显示。LCD 仍由原程序输出带检测框的连续预览；**第一版电脑画面不带红框**。

640×480×3 = 921600 字节/帧；5 帧/秒约 36.9 Mb/s。28.5 帧/秒原始画面约 210 Mb/s，超过先前观测到的 100 Mb/s 网口。`STUDYBOARD_STREAM_STRIDE=6` 在相机约 28.5 fps 时得到约 4.75 fps。网络慢时只丢旧的网络待发帧，不等待 PC；最终仍需用开发板日志核对采集帧率及 LCD 更新率。TCP 服务未加认证或加密，只用于可信的直连局域网。

## 一次性构建（Ubuntu 虚拟机）

把本目录以下文件同步到 Ubuntu 的同一个干净目录，例如 `/home/ubuntu/studyboard_ncnn_face_net`：

```text
Makefile
studyboard-ncnn-face.cpp
face_detector.h / face_detector.cpp
lcd_output.h / lcd_output.cpp
video_stream_server.h / video_stream_server.cpp
```

如果 Windows 的 `D:\share` 正是 Ubuntu 的 `/mnt/hgfs/share`，可在 PowerShell 中一次复制（换路径前先核对）：

```powershell
$src = 'E:\zynq\github\ZYNQ_ov5640_hdmi_frame_difference-master\linux\studyboard_ncnn_face'
$dst = 'D:\share\studyboard_face_net_app'
New-Item -ItemType Directory -Path $dst -ErrorAction Stop
'Makefile','studyboard-ncnn-face.cpp','face_detector.h','face_detector.cpp','lcd_output.h','lcd_output.cpp','video_stream_server.h','video_stream_server.cpp','pc_video_viewer.py' | ForEach-Object { Copy-Item -LiteralPath (Join-Path $src $_) -Destination $dst }
```

Ubuntu 中先确认目标目录不存在，再复制到本地文件系统进行编译：

```sh
test ! -e /home/ubuntu/studyboard_ncnn_face_net && \
  cp -a /mnt/hgfs/share/studyboard_face_net_app /home/ubuntu/studyboard_ncnn_face_net
```

继续使用与已验证程序相同的 PetaLinux SDK 和 NCNN simpleomp 安装目录。在新终端中执行：

```sh
unset LD_LIBRARY_PATH
source /home/ubuntu/petalinux/ALIENTEK-ZYNQ/images/linux/sdk/environment-setup-cortexa9t2hf-neon-xilinx-linux-gnueabi
cd /home/ubuntu/studyboard_ncnn_face_net
make -j2 APP=studyboard-ncnn-face-lcd NCNN_INSTALL=/home/ubuntu/ncnn-zynq-20260526-simpleomp OPENMP=simple
file studyboard-ncnn-face-lcd
arm-xilinx-linux-gnueabi-readelf -d studyboard-ncnn-face-lcd | grep -E 'NEEDED|RPATH|RUNPATH'
```

`file` 应显示 ARM 32-bit ELF。若你的 NCNN 安装目录不同，只替换 `NCNN_INSTALL`；不要为本次网口功能重新编译 NCNN 或内核。构建产物是新的 `studyboard-ncnn-face-lcd`，PC 脚本无需交叉编译。

构建成功后可将新程序拷回共享目录，供 Windows `scp` 使用：

```sh
cp -v studyboard-ncnn-face-lcd /mnt/hgfs/share/studyboard_face_net_app/
```

## 保留稳定版并建立板端试验目录

在开发板上，把 `BASE` 设为你**当前实际确认可用**的持久化目录。下面仅用以前的目录名作示例；先 `ls` 核对，勿盲目照抄。

```sh
BASE=/media/sd-mmcblk0p2/face-lcd-preview-20261012-01
TRIAL=/media/sd-mmcblk0p2/face-lcd-network-trial
test -f "$BASE/start-face-lcd.sh" && \
  test -f "$BASE/studyboard-ncnn-face-lcd" && \
  test ! -e "$TRIAL" && cp -a "$BASE" "$TRIAL"
ls -lh "$TRIAL/studyboard-ncnn-face-lcd"
```

若 `test` 返回非零，先停下来查路径。把 Ubuntu 新编译的 ARM 可执行文件通过共享目录/SCP 传到 **`$TRIAL/studyboard-ncnn-face-lcd`**，不要传到 `$BASE`。传输后核对 `file`/`sha256sum`；试验目录原有的 `SHA256SUMS` 若仍存在，二进制一项会过期，应对新文件重新生成校验清单，不要沿用旧校验值。原版可随时从 `$BASE` 启动。

在 Windows PowerShell 上传新二进制（须先创建上述试验目录）：

```powershell
scp -O -o HostKeyAlgorithms=+ssh-rsa 'D:\share\studyboard_face_net_app\studyboard-ncnn-face-lcd' 'root@192.168.10.2:/media/sd-mmcblk0p2/face-lcd-network-trial/studyboard-ncnn-face-lcd'
```

如果 SSH 再次报告主机密钥变化，应先从开发板串口核对新指纹；不要为了完成上传而跳过主机密钥校验。

## 运行顺序

先确认电脑能 `ping 192.168.10.2`。开发板执行：

```sh
cd /media/sd-mmcblk0p2/face-lcd-network-trial
STUDYBOARD_LCD_MODE=preview STUDYBOARD_MODEL_SIZE=192 \
STUDYBOARD_STREAM_PORT=5000 STUDYBOARD_STREAM_STRIDE=6 \
sh ./start-face-lcd.sh 0
```

`0` 表示持续运行，用 `Ctrl-C` 停止。192 仅为例子：若你的当前稳定模型是 224，就用 `STUDYBOARD_MODEL_SIZE=224`，不要因此切换或重导模型。程序应打印 `network: raw 640x480 BGR24 TCP port=5000`，电脑连接后打印 `network: PC connected`。

电脑 Windows PowerShell 中，使用已有含 NumPy/OpenCV 的训练环境，进入 `pc_video_viewer.py` 所在目录后：

```powershell
python .\pc_video_viewer.py --host 192.168.10.2 --port 5000
```

按 `Q`、`Esc` 或 `Ctrl-C` 结束电脑预览。电脑程序会在短暂断线时自动重连；板端程序退出后会显示 `network_summary`。板端仍需检查 `capture_summary` 的 `sequence_gaps=0`、`bad_payloads=0`，以及 LCD 是否全程更新。如果网络打开后采集或 LCD 明显变差，可将 `STUDYBOARD_STREAM_STRIDE` 增至 10 或 15，再对比测量。仅启动 PC 脚本不会自动启动板端服务。

## 重要边界

- 当前只有一个 PC 连接；后来的连接需等前一个断开。
- PC 显示的是原始画面，不带人脸框；板上 LCD 仍按原逻辑带框。需要电脑端红框时应另加同步元数据或发送已标注帧，不能再开第二个 V4L2 采集进程。
- 软件是未经板端实测的新试验版。先做短时间联调，再做长时间测试与断线重连测试；通过前不要设为开机自动运行。
