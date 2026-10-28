# 摄像头、LCD 与 PC 网口预览联合测试

测试记录日期：2026-10-16。文档归档日期：2026-10-28。

## 证据来源与范围

下面保存开发者提供的板端输出摘要和同次 PC 显示观察。开发者先确认 PC
可正常显示，随后明确确认“摄像头 → LCD 检测显示 + 网口 → PC 已跑通”。
这是既有实测归档，本次文档修改没有重新执行板端测试。

测试路径是 V4L2 单采集进程、NCNN 推理、LCD preview 与 SBV1 原始 BGR24
TCP 视频。板端保存图像的位置为
`/media/sd-mmcblk0p2/face-lcd-network-trial-20261016/face-lcd-last.ppm`。

摘要未包含完整启动命令、模型尺寸、线程参数和系统资源占用。
脚本默认值不能替代当次启动记录，因此本记录不作为不同模型尺寸或开网/关网
之间严格横向对照的依据。实物照片、视频及原始日志附件尚待补入仓库。

## 板端输出摘要

```text
network_summary: port=5000 stride=6 sampled=403 sent=399 overwritten=1 connections=2 disconnects=2
inference=181 source_sequence=6097 bytes=921600 faces=0 preprocess=22.76ms inference=867.31ms postprocess=0.07ms
inference=182 source_sequence=6108 bytes=921600 faces=0 preprocess=18.47ms inference=774.58ms postprocess=0.07ms
capture_summary: frames=6109 seconds=213.710 fps=28.59 sequence_gaps=0 bad_payloads=0
inference_summary: processed=182 skipped_stale=5927 total_faces=34 last_sequence=6108
average_ms: preprocess=21.81 inference=1153.67 postprocess=0.07 maximum_inference=1407.27 effective_inference_fps=0.87
lcd_summary: frames=3040 average_publish_ms=11.66
```

停止时内核 `display stopped` 消息与用户态保存路径交错打印，上面仅摘录
独立统计行，没有据此认定路径或驱动异常。

## PC 侧观察

开发者提供的窗口截图显示摄像头画面和持续增长的源序号，稳定区间打印的
`PC display` 约为 4.76–4.77 fps。这是接收脚本的显示速率，不能当成
摄像头或模型帧率。网络分支发送原图，PC 没有检测框；LCD 使用带框预览。

## 统计口径

| 指标 | 计算 / 解释 |
| --- | --- |
| 采集吞吐 | 6109 / 213.710 ≈ 28.59 fps |
| LCD 新画面发布 | 3040 / 213.710 ≈ 14.22 次/秒；不代表面板扫描频率 |
| 纯推理速度 | 1000 / 1153.67 ≈ 0.87 次/秒；未含全部预处理和调度时间 |
| 总体处理吞吐 | 182 / 213.710 ≈ 0.85 次/秒；采用采集计时，末尾还有推理收尾 |
| 网络目标节拍 | 28.59 / 6 ≈ 4.77 帧/秒，与 PC 稳定区间吻合 |
| `skipped_stale=5927` | 推理主动跳过过时输入，不能当成摄像头丢帧 |
| `total_faces=34` | 各次检测框的累加，不是 34 个不同的人，也不是准确率 |
| `sampled=403` | PC 已连接且命中抽帧条件时进入网络缓冲的次数 |
| `sent=399` | 完成整帧发送的次数，不单独证明 PC 实际显示了每一帧 |
| `overwritten=1` | 新图替换旧待发图；发送中的槽位不会被覆盖 |
| `connections/disconnects` | 连接建立和发送失败/断开统计，次数本身不能说明故障原因 |

这次序号与负载检查通过，开发者确认 LCD、PC 同时显示。
记录支持“功能链路跑通、此次约 3 分 34 秒运行通过”；长期稳定性、
断网恢复、资源开销和现场精度需要单独测试。

复测时应保存完整启动命令、模型/线程/网络参数和所有 summary，
记录程序、模型哈希、内核版本及实际观察。
详见[素材与测试清单](../project-media-and-tests.md)。
