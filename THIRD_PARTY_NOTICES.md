# 第三方源码和模型说明

本工程从既有的 OV5640/HDMI 帧差实验逐步移植到 STUDYBOARD Zynq-7020，
并增加 ATK-MD0430 LCD、PetaLinux V4L2、NCNN 人脸检测与网口预览。
导入的源码、IP 和文档保留各文件中原有的作者、版权和许可声明。

- `SDK/`、`ip_repo/` 中包含 Xilinx/AMD、Digilent、正点原子等来源的代码；
  相应声明见文件头，其中部分 Digilent 文件标注 BSD 许可。
- `ov5640_hdmi.srcs/`、历史 README 的 HDMI/帧差内容来自原始工程资料。
  当前工作目录没有原 Git 历史，本文不猜测原仓库 URL 或把这些内容重新许可。
- 新增 Linux 驱动和用户态文件使用各自文件头的 SPDX 声明；
  某个文件的许可不自动覆盖整个混合来源项目。
- NCNN、Ultralytics、pnnx 和 PetaLinux/Vivado 是构建或训练所需的外部依赖，
  不在此仓库中重新打包完整工具链。
- `models/`（若存在）保存本项目的训练权重和导出模型，来源与校验信息见该目录说明。
  权重及数据的使用条件需要结合上游工具和原始数据集的条款确认。

此仓库没有为所有历史文件统一添加新的开源许可证。
