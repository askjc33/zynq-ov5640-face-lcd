# 第三方源码和模型说明

本工程从既有的 OV5640/HDMI 帧差实验逐步移植到 STUDYBOARD Zynq-7020，
并增加 ATK-MD0430 LCD、PetaLinux V4L2、NCNN 人脸检测与网口预览。
导入的源码、IP 和文档保留各文件中原有的作者、版权和许可声明。

开发者确认的原始参考项目为
[luyixdu/ZYNQ_ov5640_hdmi_frame_difference](https://github.com/luyixdu/ZYNQ_ov5640_hdmi_frame_difference)。
其 README 描述 OV5640、VDMA 与 HDMI 帧差运动检测，相关 SDK、IP 和
HDL 构成本工程的历史基础。当前首页围绕 Linux 人脸检测、LCD 和网络扩展
重新撰写，不再将上游整篇说明和演示图片作为本项目当前成果展示。

- `SDK/`、`ip_repo/` 中包含 Xilinx/AMD、Digilent、正点原子等来源的代码；
  相应声明见文件头，其中部分 Digilent 文件标注 BSD 许可。
- `ov5640_hdmi.srcs/` 和部分 SDK/IP 来自原始工程资料。
  本仓库以源码快照导入，没有导入上游完整 Git 历史；来源链接不构成重新许可。
- 新增 Linux 驱动和用户态文件使用各自文件头的 SPDX 声明；
  某个文件的许可不自动覆盖整个混合来源项目。
- NCNN、Ultralytics、pnnx 和 PetaLinux/Vivado 是构建或训练所需的外部依赖，
  不在此仓库中重新打包完整工具链。
- `models/` 保存本项目的训练权重和导出模型，来源与校验信息见该目录说明。
  权重及数据的使用条件需要结合上游工具和原始数据集的条款确认。

此仓库没有为所有历史文件统一添加新的开源许可证。
