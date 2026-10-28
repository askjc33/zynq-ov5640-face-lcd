# 07｜Linux 人脸系统：按一次运行逐层读源码

本章讲**代码如何执行**，不是重复操作命令。建议一边打开对应源码，一边沿“启动脚本→驱动→采集循环→推理→LCD→退出”顺序读。所有行号会随编辑变化，因此以函数/结构名定位；本章的事实来自 `linux/` 当前源码及已有板端日志，主机测试不能替代上板验收。

## 1. 启动脚本实际做了什么

入口是 `linux/studyboard_ncnn_face/start-face-lcd.sh`。它先解析 `[frame-count]`：`0` 表示无限，未给参数时也默认 0；检查 root、bundle 文件、模型 320 基线、`studyboard-v4l2-test`、`KERNEL-RELEASE == uname -r`，再检查旧 `running`/`display_running` 都是 0。若当前还没初始化传感器，它把 Linux `/proc/sys/kernel/random/boot_id` 记在 `/tmp/studyboard-ov5640-boot-id`，并用独立的“本次启动已尝试”标记防止失败后同一次开机无限重试。

未装 LCD 扩展模块时，脚本先用系统已有 capture 模块抓 5 帧；通过后卸载旧模块、`insmod` 同版本 LCD 模块，再抓 5 帧。任一阶段失败会尝试回退原模块或直接退出，不会继续运行 NCNN。最后 `exec sh run-face-lcd.sh "$FRAMES"`：`exec` 让后续退出状态就是子脚本/程序状态，不再留一个多余的 shell 层。**因此日常请调用 `start`，不要跳过它直接运行 ELF**。

`run-face-lcd.sh` 把环境变量映射成程序参数：`STUDYBOARD_LCD_MODE` 默认 `matched`；`STUDYBOARD_MODEL_SIZE` 默认 320（192/224/256 分别对应 `face-model-尺寸`）；`STUDYBOARD_PREVIEW_STRIDE` 默认 2。它检查 `lcd_frame` 可写、模型文件存在，停止 legacy sysfs 通道；若本次 boot ID 已初始化就跳过 `ov5640-init`。调用 C++ 程序时固定设备 `/dev/video0`、2 线程、confidence 0.25、NMS IoU 0.45、输出 `face-lcd-last.ppm`。这些“默认值”是**启动脚本**的默认值；直接运行 C++ 二进制还有自己的 CLI 默认值（例如 `threads=1`、`frames=300`、旧 `/home/root` 模型路径），两者不能混用。

## 2. 驱动为何能出现 `/dev/video0`

`studyboard-frame-capture.dtsi` 将设备树 `compatible`、两条 DMA phandle 和图像几何传给 `studyboard-frame-capture.c`。`studyboard_capture_probe()` 通过 `dma_request_chan(...,"capture")` 取得 VDMA0 S2MM，`..."display"` 取得 VDMA1 MM2S；用 `dma_alloc_coherent` 分配三帧诊断/LCD 区（2,764,800 字节），再创建 sysfs 属性，调用 `studyboard_v4l2_register()` 配置 `vb2_queue`、注册 video device。**probe 只申请资源，不启动视频**。

该驱动管理至少四种不同对象：

| 对象 | 地址/所有权 | 用途 |
| --- | --- | --- |
| `buffer_cpu`/`buffer_dma` 三帧 coherent 区 | 同一内存的 CPU 虚拟地址/DMA 总线地址 | 旧 sysfs 抓图、LCD front/back；**不是**V4L2 的四个 MMAP buffer |
| `snapshot_cpu` | `vzalloc` 的 CPU 冻结副本 | 诊断 `snapshot_raw`，免得 DMA 写时用户读到半帧 |
| `vb2_dma_contig` 队列缓冲 | vb2 分配、QBUF/DQBUF 转移使用权 | 标准 V4L2 用户程序采集 |
| 用户态 `LatestFrameMailbox.slots[2]` | 用户空间 `malloc` 的两帧 | 慢推理与快采集之间只保留最新完整图像 |

不要把 `DMA 地址`当 CPU 指针，也不要把三个 coherent 帧误称作“用户申请的四个 V4L2 缓冲”。`studyboard_v4l2_open()` 在 `open()` 时就设唯一 owner，防止另一个进程/旧 `running=1` 抢 S2MM；`release()` 结束 vb2 会话并回收该进程 LCD 会话。`studyboard_v4l2_queue_setup()` 要求每帧 `width×height×3`，至少三缓冲；`studyboard_v4l2_buffer_queue()` 拿 vb2 的 DMA 地址做 interleaved 描述符；帧完成回调填序号/时间戳/payload 并 `vb2_buffer_done()`。`start_streaming()` 配置 VDMA 持续运行，`stop_streaming()` 终止并归还缓冲。特殊的 park/连续模式是针对本项目已验证的 2020.2 Xilinx DMAEngine 行为，不应随手改成“每帧停机重启”。

驱动同时暴露旧 `running`、`display_running`、`snapshot_raw` 和新 `lcd_frame`、`lcd_commit`、`lcd_status`。前者是带来可观察性的 bring-up 诊断入口，正常人脸程序不直接用它采集。LCD 不是 `/dev/fb0`，也不是 V4L2 output 节点；它是本驱动定制的 sysfs 二进制接口。

## 3. C++ `main()`：从参数到第一帧

程序 `linux/studyboard_ncnn_face/studyboard-ncnn-face.cpp` 最多接受 12 个位置参数，脚本实际传满：

| 位置 | 含义 | 脚本当前值/来源 |
| ---: | --- | --- |
| 1 | V4L2 节点 | `/dev/video0` |
| 2/3 | NCNN param/bin | 对应 `face-model[-尺寸]/model.ncnn.*` |
| 4 | 采集帧目标；0 无限 | `start-face-lcd.sh` 命令行参数 |
| 5 | NCNN 线程数 | 2 |
| 6/7 | 置信度/NMS 阈值 | 0.25/0.45 |
| 8 | 最后一张带框 PPM；`-` 不保存 | 当前 bundle `face-lcd-last.ppm` |
| 9 | LCD sysfs 目录；`-` 不显示 | `/sys/bus/platform/devices/studyboard-frame-capture` |
| 10 | `matched` 或 `preview` | `STUDYBOARD_LCD_MODE` |
| 11 | preview 每几帧发布一次，1～8 | 默认 2 |
| 12 | 模型方形输入尺寸 | 192/224/256/320 |

`main()` 先让 `LcdOutput::configure()` 检查 sysfs 文件大小/权限，再让 `FaceDetector::load()` 加载模型；接着初始化 `pthread_mutex`/条件变量和两槽邮箱，必要时另分配一帧 preview 图。视频 `open(...O_NONBLOCK)` 后调用 `VIDIOC_QUERYCAP` 确认 capture+streaming，`VIDIOC_S_FMT` 要求 640×480 `BGR24` 和 `sizeimage=921600`。再 `VIDIOC_REQBUFS(4,MMAP)`，逐个 `QUERYBUF/mmap`，全部 `QBUF`，启动推理线程，最后 `STREAMON`。这套顺序解释了“模块加载了 `/dev/video0`，但在 STREAMON 前不会收到帧”。

## 4. 采集线程与两槽邮箱：为何跳过旧帧

主线程每次 `poll()` 最多等 3000 ms；超时即打印 `Timeout waiting for frame N`。有帧后 `DQBUF`，检查 index、`bytesused`、相邻 `sequence`；坏 payload 会计入 `bad_payloads`，序号不连续计入 `sequence_gaps`。随后锁邮箱，选择**没有被推理线程占用**的槽，把最多 921600 字节复制过去，不足部分清零，写 sequence/bytesused/generation，唤醒 worker；解锁后立即把 V4L2 buffer `QBUF` 还给驱动。这个复制是有意的：NCNN 可能算 0.4～1 秒，而相机约 35 ms 一帧，不能一直占着 vb2 缓冲。

`inference_worker()` 等条件变量，只读最新一代的完整槽；取得槽时设置 `processing_slot` 防止采集覆盖，推理完成并制作独立带框副本后释放槽。因此 `captured - processed = skipped_stale` 是用户空间低延迟策略，**不是**V4L2 收帧失败。真正抓帧健康要结合 sequence gap、payload、IRQ、PPM 观察。

`preview` 中还有首次发布门槛：先让推理线程发布一张完整带框帧使连续 MM2S 启动，之后主采集循环每隔 `preview_stride` 帧在较新的图上叠加最近一次结果。若检测框来自超过 60 个采集序号之前，就不再画它；画面仍继续更新。`matched` 则由推理线程只发布**此次被推理的原图和此次框**。这也解释为什么 preview 的画面速率高于检测速率、红框可能略滞后。

## 5. `FaceDetector`：把字节变成红框

`FaceDetector::load()` 明确只接收 192/224/256/320 输入，关闭 Vulkan，设置 `num_threads`，加载同一次导出的 `.param` 和 `.bin`。`prediction_count_for_size(S)` 计算 `(S/8)^2+(S/16)^2+(S/32)^2`，例如 192→756、320→2100。`detect()` 的步骤：

```text
640×480 BGR24
 → ncnn::Mat::from_pixels_resize(PIXEL_BGR2RGB)
 → 等比缩放；例如 192×144
 → 上下填 114（192 时各 24 像素）
 → 每通道乘 1/255，输入 RGB float32 CHW
 → extractor.input("in0") / extract("out0")
 → 核对输出为 5×N；取 cx,cy,w,h,score
 → score≥confidence 的框逆 letterbox、裁剪到 640×480
 → 按 score 排序，同类 IoU>nms_threshold 的重复框抑制，最多 100 个
 → 在 BGR 副本上画 3 像素红框；最后一张可写 PPM
```

归一化**发生在填边以后**，所以填充值是 114 而不是 `114/255`。输出坐标逆变换须减实际左右/上下 padding，再除本次缩放系数。改输入尺寸时如果还用 320 的上下 40 像素会使框偏移。代码拒绝输出 `out0` 候选数/行数不符的模型，而不是静默读错。`face_detector.h` 目前注释只写“224、256、320”而 C++ 实现明确允许 192，属于注释漂移，应以实际 CLI/实现和运行日志为准。

## 6. `LcdOutput` 与驱动：一帧怎样上屏

`LcdOutput::publish()` 检查必须恰好 921600 字节，重新 `open(lcd_frame)` 让文件偏移从 0 开始，最多 4096 字节/次写完整帧；写失败不发 `lcd_commit=1`。驱动 `lcd_frame_write()` 校验 owner、偏移连续、长度不越界，只把数据存入非显示的 back。`lcd_commit_store()` 必须确认已写满一帧；第一次把 back 作为 front 并启动连续 VDMA1 MM2S，之后 `studyboard_publish_lcd()` 把完整 back `memcpy` 到持续被 MM2S 读取的 front。所有 frame-store 指同一 front，使 DMA 在一次发布后不会停止；退出 `stop()` 发 `lcd_commit=0`。`v4l2_release()` 和驱动 `remove()` 也有兜底清理。

这套机制避免把**sysfs 分块写到一半**的帧直接显示，但前台整帧复制时仍可能与 MM2S 读重叠，所以**不保证严格无撕裂**。若未来要保证无撕裂，应设计真实的帧边界交换/同步，而非仅提高 `preview_stride`。`lcd_status.frames` 是驱动接受发布的次数，不是 LCD 面板刷新率。

## 7. 退出、失败与日志含义

有限帧数到达或 `Ctrl+C` 后，主线程 `STREAMOFF`，设置 `capture_done` 并唤醒/等待推理线程。worker 保存最后一张 PPM，打印检测/统计；主线程再停 LCD、`munmap`、释放 malloc/互斥量和文件。`exit_status=0` 需至少一次推理、无 sequence gap、无坏 payload、worker 未失败；如果 `0` 次推理，即使曾短暂采到帧也不能算成功。

| 统计 | 代码如何算 | 不代表什么 |
| --- | --- | --- |
| `capture_summary.fps` | `captured_frames / (STREAMON 到 STREAMOFF 的秒数)` | 不是模型速度 |
| `inference_summary.processed` | worker 成功检测次数 | 不等于采集帧数 |
| `skipped_stale` | `captured_frames - processed` | 不是 DMA 丢帧 |
| `total_faces` | 各次推理输出框总数 | 不是不同人的数量 |
| `effective_inference_fps` | `1000 / 平均 inference_ms` | 不是端到端框延迟 |
| `lcd_summary.frames` | 首次及 preview/matched 的发布调用数 | 不是物理 LCD 刷新帧率 |

读代码的安全练习：不改硬件，只在同一真实日志里手算一遍 921600、192 的 756 候选、`capture-processed`、`1000/平均 inference_ms`，再核对程序输出。能解释每一个分母和缓冲所有权，才算读懂这条链。
