// SPDX-License-Identifier: GPL-2.0
/*
 * StudyBoard 视频采集/显示 DMAEngine 客户端驱动（教学版注释）。
 *
 * 一、这个文件处在整个系统的哪一层？
 *
 *   用户空间（echo/cat sysfs，v4l2-ctl/OpenCV 访问 /dev/videoX）
 *                    |
 *                    v
 *   本驱动 studyboard-frame-capture.c
 *       - 从设备树读取图像尺寸及 DMA 通道引用；
 *       - 分配三帧连续且 DMA 可访问的 DDR 缓冲区；
 *       - 通过 Linux DMAEngine API 配置、启动和停止两个 VDMA 通道；
 *       - 提供 sysfs 调试接口和固定 BGR24 的 V4L2 MMAP 接口。
 *                    |
 *                    v
 *   内核已有的 Xilinx AXI VDMA 驱动（xilinx_dma）
 *       - 真正访问 VDMA 寄存器并处理中断。
 *                    |
 *                    v
 *   PL 中的 AXI VDMA0 / AXI VDMA1 硬件
 *
 * 因此，本文件不是直接读写 VDMA MMIO 寄存器的底层驱动，而是使用
 * DMAEngine 通用框架的“客户端”。在已验证 sysfs 冻结帧后，本版
 * 又用 videobuf2 把 VDMA0 封装为 /dev/videoX，先支持固定 640x480
 * BGR24 MMAP 采集。它仍是教学型单节点驱动，还没有 OV5640 subdev
 * 和完整 media graph。
 *
 * 二、像素数据通路
 *
 *   OV5640
 *     -> PL 帧差模块
 *     -> AXI4-Stream
 *     -> VDMA0 S2MM（Stream to Memory-Mapped，向 DDR 写）
 *     -> 三个 dma_alloc_coherent() 分配的 24 位帧缓冲区
 *     -> VDMA1 MM2S（Memory-Mapped to Stream，从 DDR 读）
 *     -> PL RGB888 转 RGB565/LCD 8080 总线模块
 *     -> NT35510 LCD
 *
 * 三、为什么采集和显示分开控制？
 *
 * 可以先只启动 VDMA0，证明“摄像头/PL -> DDR”正确；再启动 VDMA1，
 * 证明“DDR -> LCD”正确。这样显示失败时不会把问题笼统归咎于整条链路。
 */

/* 中断回调和进程上下文可能同时访问的计数/状态。 */
#include <linux/atomic.h>
/* completion 用于等待某个 DMA 描述符触发回调。 */
#include <linux/completion.h>
/* 一致性 DMA 缓冲区的分配/释放接口。 */
#include <linux/dma-mapping.h>
/* Xilinx VDMA 特有配置结构和 set_config() 接口。 */
#include <linux/dma/xilinx_dma.h>
/* Linux 通用 DMAEngine：申请通道、准备/提交描述符、启动/停止。 */
#include <linux/dmaengine.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/jiffies.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/vmalloc.h>

/* V4L2 设备注册、ioctl 和 videobuf2 连续 DMA 缓冲队列。 */
#include <media/v4l2-device.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-dma-contig.h>
#include <media/videobuf2-v4l2.h>

/*
 * 驱动最多管理三个帧缓冲。设备树 frame-count 不能超过此值，因为下面的
 * cookies[]、callbacks[] 等数组均按这个上限静态分配。
 */
#define STUDYBOARD_MAX_FRAMES 3U
/*
 * 设备树省略对应属性时使用的默认格式。PL 逻辑上是 RGB888，
 * 但 Zynq ARM 是小端序，24 位 TDATA={R,G,B} 写入 DDR 后，CPU 按地址
 * 由低到高读到的字节是 B,G,R。因此后续 V4L2 用户态格式应声明为
 * V4L2_PIX_FMT_BGR24，而不是 RGB24。
 */
#define STUDYBOARD_DEFAULT_WIDTH 640U
#define STUDYBOARD_DEFAULT_HEIGHT 480U
#define STUDYBOARD_DEFAULT_BYTES_PER_PIXEL 3U
#define STUDYBOARD_DEFAULT_FRAME_COUNT 3U
#define STUDYBOARD_V4L2_MIN_BUFFERS 3U
/* 等待一次描述符完成回调的最长时间，防止 probe/echo 永久睡眠。 */
#define STUDYBOARD_SETUP_TIMEOUT_MS 3000U

/* 前置声明，使回调上下文结构能够保存主设备状态指针。 */
struct studyboard_capture;

/*
 * videobuf2 为每个 QBUF 分配的驱动私有包装。
 *
 * vb 是 V4L2/vb2 公共头；queued 用于本驱动跟踪已提交给
 * DMAEngine 但还未完成的缓冲；capture 供 DMA 回调找回设备。
 */
struct studyboard_v4l2_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head queued;
	struct studyboard_capture *capture;
};

/*
 * 每个帧描述符的回调参数。
 * DMAEngine 的 callback_param 是 void *，所以用这个小结构同时携带：
 *   1. capture：整个驱动实例；
 *   2. index：该描述符对应 frame0/frame1/frame2 中的哪一帧。
 */
struct studyboard_frame_callback {
	struct studyboard_capture *capture;
	u32 index;
};

/*
 * 一个设备实例的全部运行状态。
 *
 * 设备树中每出现一个 compatible = "studyboard,frame-capture-1.0" 节点，
 * platform 总线匹配成功后就会在 probe() 中分配一个本结构。
 */
struct studyboard_capture {
	/* platform device 对应的 struct device，用于日志、DMA 和 sysfs。 */
	struct device *dev;
	/* VDMA0 S2MM：PL AXI4-Stream -> DDR，设备树名为 "capture"。 */
	struct dma_chan *channel;
	/* VDMA1 MM2S：DDR -> LCD AXI4-Stream，设备树名为 "display"。 */
	struct dma_chan *display_channel;
	/* 用于 dma_alloc_coherent() 的 DMA 设备。 */
	struct device *dma_dev;

	/*
	 * 同一块帧缓冲区的两个地址：
	 *   buffer_cpu：CPU 虚拟地址，内核用它 memset/计算摘要；
	 *   buffer_dma：DMA 总线地址，写入 VDMA 帧地址寄存器。
	 * 两者数值和含义不同，不能相互替代。
	 */
	void *buffer_cpu;
	dma_addr_t buffer_dma;
	/*
	 * snapshot_cpu 是只供 CPU 使用的冻结帧副本，不交给 VDMA。
	 * 它把“DMA 正在循环覆盖的缓冲”与“用户正在读取的快照”隔离开。
	 */
	void *snapshot_cpu;

	/* 一帧大小和三帧总大小，单位均为字节。 */
	size_t frame_bytes;
	size_t total_bytes;
	/* 图像几何参数来自设备树，缺省为 640x480x3、三帧。 */
	u32 width;
	u32 height;
	u32 bytes_per_pixel;
	/* 每行在 DDR 中占用的字节数；当前无行尾填充，等于 width*bpp。 */
	u32 stride;
	u32 frame_count;

	/*
	 * cookie 是 DMAEngine 为每次提交返回的事务编号，主要用于错误检查/
	 * 状态追踪；callbacks[] 为每个采集帧保存回调参数。
	 */
	dma_cookie_t cookies[STUDYBOARD_MAX_FRAMES];
	struct studyboard_frame_callback callbacks[STUDYBOARD_MAX_FRAMES];
	/* 显示通道对应的 cookie 和回调参数。 */
	dma_cookie_t display_cookies[STUDYBOARD_MAX_FRAMES];
	struct studyboard_frame_callback display_callbacks[STUDYBOARD_MAX_FRAMES];

	/* start 函数提交描述符后，通过这两个 completion 等待相应回调。 */
	struct completion descriptor_done;
	struct completion display_descriptor_done;
	/*
	 * 回调统计只记录本驱动所提交描述符的回调次数，不是持续的实时帧率计数。
	 * VDMA 进入硬件循环模式后，即使 /proc/interrupts 持续增长，这里的数值
	 * 也不保证逐帧增长。
	 */
	atomic64_t callback_count;
	atomic64_t display_callback_count;
	/* 最后一次触发本驱动回调的描述符所对应的帧下标。 */
	atomic_t last_frame;
	atomic_t display_last_frame;

	/* 串行化 sysfs 启停请求，避免两个进程同时改变 DMA 状态。 */
	struct mutex control_lock;

	/*
	 * V4L2 层：v4l2_lock 保护 ioctl/vb2 队列操作；queued_lock
	 * 保护进程上下文 QBUF/STREAMOFF 与 DMA 回调共享的链表。
	 */
	struct mutex v4l2_lock;
	spinlock_t v4l2_queued_lock;
	struct list_head v4l2_queued_buffers;
	struct v4l2_device v4l2_device;
	struct video_device video_device;
	struct vb2_queue vb2_queue;
	u32 v4l2_sequence;
	bool v4l2_streaming;
	bool v4l2_owner;
	bool v4l2_registered;
	/* 大于一页的二进制 sysfs 文件，用来导出 snapshot_cpu。 */
	struct bin_attribute snapshot_bin_attribute;
	/* snapshot_index 表示冻结的是 frame0/1/2；valid=false 表示尚无快照。 */
	u32 snapshot_index;
	bool snapshot_valid;
	/* 软件记录的两个通道状态，避免重复启动/停止。 */
	bool running;
	bool display_running;
	/*
	 * 用户态 LCD 路径复用旧诊断路径的 coherent 缓冲，但绝不与旧采集并用。
	 * V4L2 使用另外分配的 vb2 缓冲，因此两条实际 DMA 数据通路相互独立。
	 * lcd_front 是 VDMA1 持续重复读取的图像；lcd_back 是用户正在分段
	 * 写入的图像。第一次提交用写好的缓冲启动 MM2S；后续提交把完整
	 * back 帧一次性复制到 front，绝不让 sysfs 的分块写直接碰正在显示的帧。
	 * lcd_running 表示 LCD 会话和连续 MM2S 都已启动。
	 * 所有字段由 control_lock 保护，pid 引用防止进程号复用造成所有权混淆。
	 */
	struct bin_attribute lcd_bin_attribute;
	struct pid *lcd_owner;
	size_t lcd_written;
	u32 lcd_front;
	u32 lcd_back;
	u64 lcd_frames;
	bool lcd_running;
};

/*
 * 采集描述符完成回调。
 *
 * 此函数通常由 DMAEngine/Xilinx VDMA 中断处理路径调用，因此不能睡眠，
 * 只做原子变量更新和 complete() 唤醒等待者。parameter 就是提交描述符时
 * 填入 descriptor->callback_param 的 callbacks[index] 地址。
 */
static void studyboard_frame_done(void *parameter)
{
	struct studyboard_frame_callback *callback = parameter;
	struct studyboard_capture *capture = callback->capture;

	/* 记录刚完成的是哪个帧描述符，并累计一次软件回调。 */
	atomic_set(&capture->last_frame, callback->index);
	atomic64_inc(&capture->callback_count);
	/* 唤醒 studyboard_submit_frame() 中的超时等待。 */
	complete(&capture->descriptor_done);
}

/* 显示（MM2S）描述符完成回调，职责与采集回调相同。 */
static void studyboard_display_frame_done(void *parameter)
{
	struct studyboard_frame_callback *callback = parameter;
	struct studyboard_capture *capture = callback->capture;

	atomic_set(&capture->display_last_frame, callback->index);
	atomic64_inc(&capture->display_callback_count);
	complete(&capture->display_descriptor_done);
}

/*
 * 为 VDMA0 S2MM 提交“一整帧”的 DMAEngine 描述符。
 *
 * capture->buffer_dma 指向三帧连续 DDR 空间的开头，第 index 帧地址为：
 *
 *   frame_dma = buffer_dma + index * frame_bytes
 *
 * interleaved template 在这里描述一个二维图像：
 *   numf                = height：一帧包含 height 行；
 *   frame_size          = 1：每一行只有一个 data_chunk；
 *   sgl[0].size         = stride：每行搬运 stride 字节；
 *   sgl[0].icg          = 0：相邻行之间没有空洞。
 *
 * 方向是 DMA_DEV_TO_MEM：源端是 VDMA 的 AXI4-Stream S2MM 接口，源地址
 * 不是普通内存地址；dst_start 才是 DDR 目的地址。
 */
static int studyboard_submit_frame(
		struct studyboard_capture *capture,
		struct dma_interleaved_template *transfer,
		size_t transfer_bytes, u32 index)
{
	struct dma_async_tx_descriptor *descriptor;
	dma_addr_t frame_dma = capture->buffer_dma +
			(index * capture->frame_bytes);
	unsigned long completed;
	int ret;

	/* 同一模板对象会为三帧重复使用，每次必须清除上一次的内容。 */
	memset(transfer, 0, transfer_bytes);
	transfer->dst_start = frame_dma;
	transfer->dir = DMA_DEV_TO_MEM;
	transfer->numf = capture->height;
	transfer->frame_size = 1;
	transfer->sgl[0].size = capture->stride;
	transfer->sgl[0].icg = 0;

	/*
	 * 让底层 Xilinx VDMA 驱动把上述通用二维模板转换为 VDMA 参数。
	 * DMA_PREP_INTERRUPT 表示完成时需要中断/回调；DMA_CTRL_ACK 表示
	 * 客户端已经认可描述符，DMAEngine 可在完成后回收它。
	 */
	descriptor = dmaengine_prep_interleaved_dma(
			capture->channel, transfer,
			DMA_CTRL_ACK | DMA_PREP_INTERRUPT);
	if (!descriptor) {
		dev_err(capture->dev,
			"failed to prepare frame %u DMA descriptor\n", index);
		return -EIO;
	}

	/* 把帧号传给完成回调。descriptor 的所有权仍属于 DMAEngine。 */
	capture->callbacks[index].capture = capture;
	capture->callbacks[index].index = index;
	descriptor->callback = studyboard_frame_done;
	descriptor->callback_param = &capture->callbacks[index];

	/* 清掉 completion 的旧完成状态，确保这次等待对应本次提交。 */
	reinit_completion(&capture->descriptor_done);
	/*
	 * submit() 只是把描述符放入软件队列并返回 cookie，还没有保证硬件
	 * 已经运行；dma_submit_error() 必须检查 cookie 是否编码了错误。
	 */
	capture->cookies[index] = dmaengine_submit(descriptor);
	ret = dma_submit_error(capture->cookies[index]);
	if (ret) {
		dev_err(capture->dev,
			"failed to submit frame %u descriptor: %d\n", index, ret);
		return ret;
	}

	/* 通知 DMAEngine：开始处理已经提交的事务。 */
	dma_async_issue_pending(capture->channel);
	/*
	 * 最多等待 3 秒。返回 0 表示超时，非 0 是剩余 jiffies，并不是帧号。
	 * 这里的等待发生在进程上下文（如 sysfs echo），允许睡眠。
	 */
	completed = wait_for_completion_timeout(
			&capture->descriptor_done,
			msecs_to_jiffies(STUDYBOARD_SETUP_TIMEOUT_MS));
	if (!completed) {
		dev_err(capture->dev,
			"timeout waiting for frame %u setup completion\n", index);
		return -ETIMEDOUT;
	}

	return 0;
}

/*
 * 为 VDMA1 MM2S 提交一帧显示描述符。
 * 它和上面的 S2MM 几乎镜像：同一个 frame_dma 此时放入 src_start，方向
 * 为 DMA_MEM_TO_DEV，表示从 DDR 读出 RGB888 并送往 LCD AXI4-Stream。
 */
static int studyboard_submit_display_frame(
		struct studyboard_capture *capture,
		struct dma_interleaved_template *transfer,
		size_t transfer_bytes, u32 index)
{
	struct dma_async_tx_descriptor *descriptor;
	dma_addr_t frame_dma = capture->buffer_dma +
			(index * capture->frame_bytes);
	unsigned long completed;
	int ret;

	memset(transfer, 0, transfer_bytes);
	transfer->src_start = frame_dma;
	transfer->dir = DMA_MEM_TO_DEV;
	transfer->numf = capture->height;
	transfer->frame_size = 1;
	transfer->sgl[0].size = capture->stride;
	transfer->sgl[0].icg = 0;

	/* 使用 display_channel，底层对应设备树中名为 display 的 MM2S 通道。 */
	descriptor = dmaengine_prep_interleaved_dma(
			capture->display_channel, transfer,
			DMA_CTRL_ACK | DMA_PREP_INTERRUPT);
	if (!descriptor) {
		dev_err(capture->dev,
			"failed to prepare display frame %u DMA descriptor\n",
			index);
		return -EIO;
	}

	capture->display_callbacks[index].capture = capture;
	capture->display_callbacks[index].index = index;
	descriptor->callback = studyboard_display_frame_done;
	descriptor->callback_param = &capture->display_callbacks[index];

	/* 与采集通道分开使用 completion/cookie，避免两个通道相互唤醒。 */
	reinit_completion(&capture->display_descriptor_done);
	capture->display_cookies[index] = dmaengine_submit(descriptor);
	ret = dma_submit_error(capture->display_cookies[index]);
	if (ret) {
		dev_err(capture->dev,
			"failed to submit display frame %u descriptor: %d\n",
			index, ret);
		return ret;
	}

	dma_async_issue_pending(capture->display_channel);
	completed = wait_for_completion_timeout(
			&capture->display_descriptor_done,
			msecs_to_jiffies(STUDYBOARD_SETUP_TIMEOUT_MS));
	if (!completed) {
		dev_err(capture->dev,
			"timeout waiting for display frame %u setup completion\n",
			index);
		return -ETIMEDOUT;
	}

	return 0;
}

/*
 * 启动采集通道：配置 VDMA0 S2MM 的三个帧地址并进入循环写帧模式。
 * 调用者必须已经持有 control_lock。
 */
static int studyboard_start_capture(struct studyboard_capture *capture)
{
	struct xilinx_vdma_config config = { 0 };
	struct dma_interleaved_template *transfer;
	size_t transfer_bytes;
	u32 index;
	int ret;

	/* sysfs 被重复写 1 时保持幂等，不重复提交一组描述符。 */
	if (capture->running)
		return 0;

	/* 每次重新开始测试时清零软件统计；-1 表示尚无任何回调。 */
	atomic64_set(&capture->callback_count, 0);
	atomic_set(&capture->last_frame, -1);

	/*
	 * 第一步先使用 park（驻留）模式安全地写入每个 frame-store 地址。
	 *
	 * PetaLinux 2020.2 所用的 Xilinx DMAEngine 驱动，每提交一个描述符
	 * 才写一个 VDMA Start Address 寄存器。如果一开始就打开循环模式，
	 * VDMA 可能在 frame1/frame2 地址尚未写入时自动前进，进而向地址 0
	 * 写数据。因此先 park，依次提交 0/1/2，待三个地址都有效再循环。
	 *
	 * 下列字段含义：
	 *   frm_dly=0   ：帧地址切换不额外延迟；
	 *   gen_lock=0  ：不使用 VDMA genlock 同步；
	 *   frm_cnt_en=0：不是只运行固定帧数后停止；
	 *   park=1      ：暂时驻留在指定 frame store；
	 *   park_frm=0  ：驻留 frame0；
	 *   coalesc=1   ：每个完成事件即可产生中断；
	 *   ext_fsync=0 ：不使用外部帧同步输入；
	 *   vflip_en=0  ：不做垂直翻转。
	 */
	config.frm_dly = 0;
	config.gen_lock = 0;
	config.master = 0;
	config.frm_cnt_en = 0;
	config.park = 1;
	config.park_frm = 0;
	config.coalesc = 1;
	config.delay = 0;
	config.reset = 0;
	config.ext_fsync = 0;
	config.vflip_en = false;

	/* 这是 Xilinx 扩展API，不属于纯通用 DMAEngine 接口。 */
	ret = xilinx_vdma_channel_set_config(capture->channel, &config);
	if (ret) {
		dev_err(capture->dev, "VDMA channel configuration failed: %d\n",
			ret);
		return ret;
	}

	/*
	 * dma_interleaved_template 的最后是柔性数组 sgl[]，sizeof(*transfer)
	 * 不包含任何 sgl 元素。本驱动需要 sgl[0]，所以动态分配：
	 *
	 *   结构体固定部分 + 一个 struct data_chunk
	 *
	 * 不能只在栈上声明普通 dma_interleaved_template 再写 sgl[0]，那样会
	 * 越界覆盖内核栈；早期出现的 stack-protector panic 就与此类错误有关。
	 */
	transfer_bytes = sizeof(*transfer) + sizeof(transfer->sgl[0]);
	transfer = kzalloc(transfer_bytes, GFP_KERNEL);
	if (!transfer)
		return -ENOMEM;

	/* park 模式下逐个编程 frame0、frame1、frame2 的 DDR 地址。 */
	for (index = 0; index < capture->frame_count; ++index) {
		ret = studyboard_submit_frame(capture, transfer,
					      transfer_bytes, index);
		if (ret)
			goto terminate;
	}

	/*
	 * 三个 frame-store 地址现在都有效。把 park 改为 0 只是更新通道配置；
	 * Xilinx 驱动会在下一次 start_transfer() 时把它应用到硬件，所以还要
	 * 再提交一次 frame0。之后 VDMA 硬件会在 0/1/2 三个地址间循环写入。
	 */
	config.park = 0;
	ret = xilinx_vdma_channel_set_config(capture->channel, &config);
	if (ret) {
		dev_err(capture->dev,
			"failed to enable VDMA circular mode: %d\n", ret);
		goto terminate;
	}

	ret = studyboard_submit_frame(capture, transfer, transfer_bytes, 0);
	if (ret)
		goto terminate;

	/* 描述符准备完成后，临时模板本身已无用途；帧缓冲仍然保留。 */
	kfree(transfer);
	capture->running = true;
	dev_info(capture->dev,
		 "capture started: %ux%u logical RGB888 / memory BGR24, stride=%u, frames=%u\n",
		 capture->width, capture->height, capture->stride,
		 capture->frame_count);
	return 0;

terminate:
	/* 任一步失败都停止通道，避免留下半配置、仍在写内存的 VDMA。 */
	kfree(transfer);
	dmaengine_terminate_all(capture->channel);
	return ret;
}

/* 停止 S2MM；只停止硬件事务，不释放三帧 DDR 缓冲区。 */
static void studyboard_stop_capture(struct studyboard_capture *capture)
{
	if (!capture->running)
		return;

	dmaengine_terminate_all(capture->channel);
	capture->running = false;
	dev_info(capture->dev, "capture stopped\n");
}

/*
 * 启动显示通道：配置 VDMA1 MM2S 循环读取同一组三帧缓冲区。
 * 调用者必须已经持有 control_lock。
 */
static int studyboard_start_display(struct studyboard_capture *capture)
{
	struct xilinx_vdma_config config = { 0 };
	struct dma_interleaved_template *transfer;
	size_t transfer_bytes;
	u32 index;
	int ret;

	if (capture->lcd_owner)
		return -EBUSY;
	if (capture->display_running)
		return 0;
	/*
	 * 本教学驱动要求先启动采集，确保 DDR 中有摄像头画面；否则显示通道
	 * 只会读到 probe() 时 memset() 产生的黑帧或旧数据。
	 */
	if (!capture->running) {
		dev_err(capture->dev,
			"start capture before starting the LCD display channel\n");
		return -EPIPE;
	}

	atomic64_set(&capture->display_callback_count, 0);
	atomic_set(&capture->display_last_frame, -1);

	/* 配置含义与采集通道相同：先 park，编程完三个读地址后再循环。 */
	config.frm_dly = 0;
	config.gen_lock = 0;
	config.master = 0;
	config.frm_cnt_en = 0;
	config.park = 1;
	config.park_frm = 0;
	config.coalesc = 1;
	config.delay = 0;
	config.reset = 0;
	config.ext_fsync = 0;
	config.vflip_en = false;

	ret = xilinx_vdma_channel_set_config(capture->display_channel, &config);
	if (ret) {
		dev_err(capture->dev,
			"display VDMA channel configuration failed: %d\n", ret);
		return ret;
	}

	transfer_bytes = sizeof(*transfer) + sizeof(transfer->sgl[0]);
	transfer = kzalloc(transfer_bytes, GFP_KERNEL);
	if (!transfer)
		return -ENOMEM;

	/* park 模式下逐个编程所有 MM2S frame-store 读地址。 */
	for (index = 0; index < capture->frame_count; ++index) {
		ret = studyboard_submit_display_frame(capture, transfer,
						      transfer_bytes, index);
		if (ret)
			goto terminate;
	}

	/* 最后再次提交 frame0，使 park=0 生效并开始 0/1/2 循环读取。 */
	config.park = 0;
	ret = xilinx_vdma_channel_set_config(capture->display_channel, &config);
	if (ret) {
		dev_err(capture->dev,
			"failed to enable display VDMA circular mode: %d\n", ret);
		goto terminate;
	}

	ret = studyboard_submit_display_frame(capture, transfer,
					      transfer_bytes, 0);
	if (ret)
		goto terminate;

	kfree(transfer);
	capture->display_running = true;
	dev_info(capture->dev,
		 "display started: DDR BGR24 bytes -> logical RGB888 -> LCD, frames=%u\n",
		 capture->frame_count);
	return 0;

terminate:
	/* 启动失败时只终止显示通道，采集通道仍可继续用于独立诊断。 */
	kfree(transfer);
	dmaengine_terminate_all(capture->display_channel);
	return ret;
}

/* 停止 MM2S；LCD 会停在最后已写入 GRAM 的画面，不在这里清屏。 */
static void studyboard_stop_display(struct studyboard_capture *capture)
{
	if (capture->display_running || capture->lcd_running) {
		dmaengine_terminate_all(capture->display_channel);
		dev_info(capture->dev, "display stopped\n");
	}
	capture->display_running = false;
	capture->lcd_running = false;
	capture->lcd_written = 0;
	if (capture->lcd_owner) {
		put_pid(capture->lcd_owner);
		capture->lcd_owner = NULL;
	}
}

/*
 * 启动用户态 LCD 会话的连续 MM2S。
 *
 * 上板已经证明本设计的 LCD AXI 接收端要求 VDMA1 持续运行。linux-xlnx
 * 2020.2 的 direct-register VDMA 驱动也不能可靠地反复执行“有限一帧、
 * terminate/reset、再启动”。因此这里沿用已验证的 legacy 启动序列：
 * 先在 park 模式下编程全部 frame-store，再切到循环模式。
 *
 * 与 legacy 不同的是，所有 frame-store 都指向同一个 lcd_front 地址。
 * 这样 VDMA 虽在 0/1/2 槽位间循环，实际始终重复读取稳定的前台帧；
 * lcd_back 不属于 MM2S，用户可以安全地通过 sysfs 分块写完整一帧。
 * 调用者必须持有 control_lock。
 */
static int studyboard_start_lcd_display(struct studyboard_capture *capture)
{
	struct xilinx_vdma_config config = { 0 };
	struct dma_interleaved_template *transfer;
	size_t transfer_bytes;
	u32 slot;
	int ret;

	if (!capture->v4l2_owner || capture->running || capture->display_running)
		return -EBUSY;
	if (capture->lcd_running)
		return 0;

	atomic64_set(&capture->display_callback_count, 0);
	atomic_set(&capture->display_last_frame, -1);

	config.frm_dly = 0;
	config.gen_lock = 0;
	config.master = 0;
	config.frm_cnt_en = 0;
	config.park = 1;
	config.park_frm = 0;
	config.coalesc = 1;
	config.delay = 0;
	config.reset = 0;
	config.ext_fsync = 0;
	config.vflip_en = false;
	ret = xilinx_vdma_channel_set_config(capture->display_channel, &config);
	if (ret)
		return ret;

	transfer_bytes = sizeof(*transfer) + sizeof(transfer->sgl[0]);
	transfer = kzalloc(transfer_bytes, GFP_KERNEL);
	if (!transfer)
		return -ENOMEM;

	/*
	 * Xilinx 驱动按内部 desc_submitcount 选择要编程的硬件槽位。重复提交
	 * lcd_front，便会把 frame-store 0/1/2 都设置为同一个前台地址。
	 */
	for (slot = 0; slot < capture->frame_count; ++slot) {
		ret = studyboard_submit_display_frame(capture, transfer,
						      transfer_bytes,
						      capture->lcd_front);
		if (ret)
			goto terminate;
	}

	/* 再提交一次让 park=0 生效，之后 MM2S 不停机地重复发送 front。 */
	config.park = 0;
	ret = xilinx_vdma_channel_set_config(capture->display_channel, &config);
	if (ret)
		goto terminate;
	ret = studyboard_submit_display_frame(capture, transfer, transfer_bytes,
					      capture->lcd_front);
	if (ret)
		goto terminate;

	kfree(transfer);
	capture->lcd_running = true;
	dev_info(capture->dev,
		 "LCD continuous display started: front=%u, repeated frame stores=%u\n",
		 capture->lcd_front, capture->frame_count);
	return 0;

terminate:
	kfree(transfer);
	dmaengine_terminate_all(capture->display_channel);
	return ret;
}

/*
 * 发布一张完整 BGR24 图像。调用者持有 control_lock。
 *
 * 第一次提交直接把已经写完的 back 设为 front 并启动连续 MM2S。
 * 后续提交只做一次完整帧 memcpy：用户态的多次 PAGE_SIZE 写永远发生在
 * back，因而不会把一张半写帧暴露给 LCD。DMA 可能在这次短 memcpy 中
 * 读到一个过渡帧，但下一次重复读取必定是完整新帧；整个通道从不停止，
 * 与已经上板验证的实时显示模式一致。
 */
static int studyboard_publish_lcd(struct studyboard_capture *capture)
{
	void *front_cpu;
	void *back_cpu;
	int ret;

	if (!capture->lcd_running) {
		capture->lcd_front = capture->lcd_back;
		dma_wmb();
		ret = studyboard_start_lcd_display(capture);
		if (ret) {
			studyboard_stop_display(capture);
			return ret;
		}
	} else {
		front_cpu = (u8 *)capture->buffer_cpu +
			capture->lcd_front * capture->frame_bytes;
		back_cpu = (u8 *)capture->buffer_cpu +
			capture->lcd_back * capture->frame_bytes;
		memcpy(front_cpu, back_cpu, capture->frame_bytes);
	}

	/* coherent 不需要 cache flush，屏障保证整帧先于下一次 AXI 读取可见。 */
	dma_wmb();
	capture->lcd_written = 0;
	capture->lcd_frames++;
	return 0;
}

/* -------------------------------------------------------------------------
 * V4L2/videobuf2 采集路径
 * -------------------------------------------------------------------------
 *
 * 这条路径不使用前面三块固定 coherent 帧缓冲，而是让
 * videobuf2-dma-contig 为每个 VIDIOC_REQBUFS 缓冲分配连续 DMA
 * 内存。QBUF 把该缓冲的 DMA 地址作为一帧描述符提交给
 * Xilinx DMAEngine；帧完成回调再用 vb2_buffer_done() 把所有权
 * 交给用户空间。
 *
 * PetaLinux 2020.2 的 Xilinx VDMA DMAEngine 驱动要求通道保持运行：
 * 每次帧完成中断把当前描述符移到 done_list，再从 pending_list
 * 启动下一描述符并改变 PARK_PTR。不能打开 FRAMECNT_EN 让硬件每帧
 * 停机；该版本驱动在有限帧计数结束后不能可靠地连续重启，表现为
 * v4l2-ctl 只得到一两帧后永久等待。
 *
 * 因此这里使用“park + 连续运行”：每个描述符仍只对应一个 vb2
 * 缓冲，一帧完成后回调立即把它归还用户；下一描述符已经由底层
 * 中断处理程序切换到另一块已排队缓冲，不会继续写刚完成的缓冲。
 */

static void studyboard_v4l2_return_all_buffers(
		struct studyboard_capture *capture,
		enum vb2_buffer_state state)
{
	struct studyboard_v4l2_buffer *buffer;
	struct studyboard_v4l2_buffer *next;
	unsigned long flags;

	spin_lock_irqsave(&capture->v4l2_queued_lock, flags);
	list_for_each_entry_safe(buffer, next,
				 &capture->v4l2_queued_buffers, queued) {
		list_del_init(&buffer->queued);
		vb2_buffer_done(&buffer->vb.vb2_buf, state);
	}
	spin_unlock_irqrestore(&capture->v4l2_queued_lock, flags);
}

/* Xilinx DMAEngine 在一帧 S2MM 完成后从 tasklet 调用。 */
static void studyboard_v4l2_frame_done(void *parameter)
{
	struct studyboard_v4l2_buffer *buffer = parameter;
	struct studyboard_capture *capture = buffer->capture;
	enum vb2_buffer_state state;
	unsigned long flags;

	spin_lock_irqsave(&capture->v4l2_queued_lock, flags);
	if (list_empty(&buffer->queued)) {
		/* STREAMOFF 可能已经取走该缓冲，不能完成两次。 */
		spin_unlock_irqrestore(&capture->v4l2_queued_lock, flags);
		return;
	}

	list_del_init(&buffer->queued);
	state = capture->v4l2_streaming ?
		VB2_BUF_STATE_DONE : VB2_BUF_STATE_ERROR;
	if (state == VB2_BUF_STATE_DONE)
		buffer->vb.sequence = capture->v4l2_sequence++;
	spin_unlock_irqrestore(&capture->v4l2_queued_lock, flags);

	buffer->vb.field = V4L2_FIELD_NONE;
	buffer->vb.vb2_buf.timestamp = ktime_get_ns();
	vb2_set_plane_payload(&buffer->vb.vb2_buf, 0,
			      capture->frame_bytes);
	vb2_buffer_done(&buffer->vb.vb2_buf, state);
}

static int studyboard_v4l2_queue_setup(struct vb2_queue *queue,
		unsigned int *buffer_count, unsigned int *plane_count,
		unsigned int sizes[], struct device *allocation_devices[])
{
	struct studyboard_capture *capture = vb2_get_drv_priv(queue);

	if (*plane_count) {
		if (*plane_count != 1 || sizes[0] < capture->frame_bytes)
			return -EINVAL;
		return 0;
	}

	/* 三缓冲能让一块 DMA 写、一块用户读、一块排队等待。 */
	if (*buffer_count < STUDYBOARD_V4L2_MIN_BUFFERS)
		*buffer_count = STUDYBOARD_V4L2_MIN_BUFFERS;
	*plane_count = 1;
	sizes[0] = capture->frame_bytes;
	return 0;
}

static int studyboard_v4l2_buffer_init(struct vb2_buffer *vb)
{
	struct studyboard_capture *capture = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *v4l2_buffer = to_vb2_v4l2_buffer(vb);
	struct studyboard_v4l2_buffer *buffer = container_of(
			v4l2_buffer, struct studyboard_v4l2_buffer, vb);

	INIT_LIST_HEAD(&buffer->queued);
	buffer->capture = capture;
	return 0;
}

static int studyboard_v4l2_buffer_prepare(struct vb2_buffer *vb)
{
	struct studyboard_capture *capture = vb2_get_drv_priv(vb->vb2_queue);

	if (vb2_plane_size(vb, 0) < capture->frame_bytes)
		return -EINVAL;

	vb2_set_plane_payload(vb, 0, capture->frame_bytes);
	return 0;
}

static void studyboard_v4l2_buffer_queue(struct vb2_buffer *vb)
{
	struct studyboard_capture *capture = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *v4l2_buffer = to_vb2_v4l2_buffer(vb);
	struct studyboard_v4l2_buffer *buffer = container_of(
			v4l2_buffer, struct studyboard_v4l2_buffer, vb);
	struct dma_interleaved_template *transfer;
	struct dma_async_tx_descriptor *descriptor;
	dma_addr_t frame_dma;
	dma_cookie_t cookie;
	size_t transfer_bytes;
	unsigned long flags;
	int ret;

	frame_dma = vb2_dma_contig_plane_dma_addr(vb, 0);
	transfer_bytes = sizeof(*transfer) + sizeof(transfer->sgl[0]);
	transfer = kzalloc(transfer_bytes, GFP_KERNEL);
	if (!transfer)
		goto buffer_error;

	transfer->dst_start = frame_dma;
	transfer->dir = DMA_DEV_TO_MEM;
	transfer->numf = capture->height;
	transfer->frame_size = 1;
	transfer->sgl[0].size = capture->stride;
	transfer->sgl[0].icg = 0;

	descriptor = dmaengine_prep_interleaved_dma(
			capture->channel, transfer,
			DMA_CTRL_ACK | DMA_PREP_INTERRUPT);
	/* prep 已经把几何参数复制到底层描述符，模板可释放。 */
	kfree(transfer);
	if (!descriptor)
		goto buffer_error;

	descriptor->callback = studyboard_v4l2_frame_done;
	descriptor->callback_param = buffer;

	spin_lock_irqsave(&capture->v4l2_queued_lock, flags);
	list_add_tail(&buffer->queued, &capture->v4l2_queued_buffers);
	spin_unlock_irqrestore(&capture->v4l2_queued_lock, flags);

	cookie = dmaengine_submit(descriptor);
	ret = dma_submit_error(cookie);
	if (ret) {
		dev_err(capture->dev,
			"failed to submit V4L2 frame buffer: %d\n", ret);
		spin_lock_irqsave(&capture->v4l2_queued_lock, flags);
		if (!list_empty(&buffer->queued))
			list_del_init(&buffer->queued);
		spin_unlock_irqrestore(&capture->v4l2_queued_lock, flags);
		goto buffer_error;
	}

	/* STREAMON 之后的重新 QBUF 需立即推进底层队列。 */
	if (READ_ONCE(capture->v4l2_streaming))
		dma_async_issue_pending(capture->channel);
	return;

buffer_error:
	vb2_buffer_done(vb, VB2_BUF_STATE_ERROR);
}

static int studyboard_v4l2_start_streaming(struct vb2_queue *queue,
					   unsigned int count)
{
	struct studyboard_capture *capture = vb2_get_drv_priv(queue);
	struct xilinx_vdma_config config = { 0 };
	unsigned long flags;
	int ret;

	mutex_lock(&capture->control_lock);
	if (capture->running || capture->display_running) {
		ret = -EBUSY;
		goto unlock_error;
	}

	config.frm_dly = 0;
	config.gen_lock = 0;
	config.master = 0;
	/* 必须持续运行；见本节顶部关于 linux-xlnx 2020.2 的说明。 */
	config.frm_cnt_en = 0;
	config.park = 1;
	config.park_frm = 0;
	config.coalesc = 1;
	config.delay = 0;
	config.reset = 0;
	config.ext_fsync = 0;
	config.vflip_en = false;

	ret = xilinx_vdma_channel_set_config(capture->channel, &config);
	if (ret) {
		dev_err(capture->dev,
			"failed to configure V4L2 capture VDMA: %d\n", ret);
		goto unlock_error;
	}

	capture->v4l2_sequence = 0;
	spin_lock_irqsave(&capture->v4l2_queued_lock, flags);
	capture->v4l2_streaming = true;
	spin_unlock_irqrestore(&capture->v4l2_queued_lock, flags);
	mutex_unlock(&capture->control_lock);

	dma_async_issue_pending(capture->channel);
	dev_info(capture->dev,
		 "V4L2 stream started: %ux%u BGR24, buffers=%u\n",
		 capture->width, capture->height, count);
	return 0;

unlock_error:
	mutex_unlock(&capture->control_lock);
	/* QBUF 已经提交的描述符必须回收，再归还 vb2 缓冲。 */
	dmaengine_terminate_all(capture->channel);
	studyboard_v4l2_return_all_buffers(capture, VB2_BUF_STATE_QUEUED);
	return ret;
}

static void studyboard_v4l2_stop_streaming(struct vb2_queue *queue)
{
	struct studyboard_capture *capture = vb2_get_drv_priv(queue);
	unsigned long flags;

	mutex_lock(&capture->control_lock);
	/* 先阻止回调宣布 DONE，再停止并释放底层描述符。 */
	spin_lock_irqsave(&capture->v4l2_queued_lock, flags);
	capture->v4l2_streaming = false;
	spin_unlock_irqrestore(&capture->v4l2_queued_lock, flags);
	dmaengine_terminate_all(capture->channel);
	mutex_unlock(&capture->control_lock);

	studyboard_v4l2_return_all_buffers(capture, VB2_BUF_STATE_ERROR);
	dev_info(capture->dev, "V4L2 stream stopped\n");
}

static const struct vb2_ops studyboard_v4l2_queue_operations = {
	.queue_setup = studyboard_v4l2_queue_setup,
	.buf_init = studyboard_v4l2_buffer_init,
	.buf_prepare = studyboard_v4l2_buffer_prepare,
	.buf_queue = studyboard_v4l2_buffer_queue,
	.start_streaming = studyboard_v4l2_start_streaming,
	.stop_streaming = studyboard_v4l2_stop_streaming,
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,
};

/* 本阶段只有一种实测通过的固定格式，所有 TRY/G/S_FMT 共用。 */
static void studyboard_v4l2_fill_format(struct studyboard_capture *capture,
					struct v4l2_pix_format *format)
{
	memset(format, 0, sizeof(*format));
	format->width = capture->width;
	format->height = capture->height;
	format->pixelformat = V4L2_PIX_FMT_BGR24;
	format->field = V4L2_FIELD_NONE;
	format->bytesperline = capture->stride;
	format->sizeimage = capture->frame_bytes;
	format->colorspace = V4L2_COLORSPACE_SRGB;
	format->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	format->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	format->xfer_func = V4L2_XFER_FUNC_DEFAULT;
}

static int studyboard_v4l2_query_capability(struct file *file, void *private,
					    struct v4l2_capability *capability)
{
	struct studyboard_capture *capture = video_drvdata(file);

	strscpy((char *)capability->driver, "studyboard-vdma",
		sizeof(capability->driver));
	strscpy((char *)capability->card, "StudyBoard OV5640 PL capture",
		sizeof(capability->card));
	snprintf((char *)capability->bus_info, sizeof(capability->bus_info),
		 "platform:%s", dev_name(capture->dev));
	capability->device_caps = V4L2_CAP_VIDEO_CAPTURE |
				      V4L2_CAP_STREAMING;
	capability->capabilities = capability->device_caps |
				   V4L2_CAP_DEVICE_CAPS;
	return 0;
}

static int studyboard_v4l2_enum_format(struct file *file, void *private,
				       struct v4l2_fmtdesc *format)
{
	if (format->index != 0)
		return -EINVAL;

	format->pixelformat = V4L2_PIX_FMT_BGR24;
	strscpy((char *)format->description, "BGR24 (PL logical RGB888)",
		sizeof(format->description));
	return 0;
}

static int studyboard_v4l2_get_format(struct file *file, void *private,
				      struct v4l2_format *format)
{
	struct studyboard_capture *capture = video_drvdata(file);

	studyboard_v4l2_fill_format(capture, &format->fmt.pix);
	return 0;
}

static int studyboard_v4l2_try_format(struct file *file, void *private,
				      struct v4l2_format *format)
{
	struct studyboard_capture *capture = video_drvdata(file);

	/* 不做软件缩放/格式转换，请求会被收敛到唯一硬件格式。 */
	studyboard_v4l2_fill_format(capture, &format->fmt.pix);
	return 0;
}

static int studyboard_v4l2_set_format(struct file *file, void *private,
				      struct v4l2_format *format)
{
	struct studyboard_capture *capture = video_drvdata(file);

	if (vb2_is_busy(&capture->vb2_queue))
		return -EBUSY;

	studyboard_v4l2_fill_format(capture, &format->fmt.pix);
	return 0;
}

static int studyboard_v4l2_enum_frame_size(struct file *file, void *private,
					   struct v4l2_frmsizeenum *size)
{
	struct studyboard_capture *capture = video_drvdata(file);

	if (size->index != 0 || size->pixel_format != V4L2_PIX_FMT_BGR24)
		return -EINVAL;

	size->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	size->discrete.width = capture->width;
	size->discrete.height = capture->height;
	return 0;
}

static int studyboard_v4l2_enum_input(struct file *file, void *private,
				      struct v4l2_input *input)
{
	if (input->index != 0)
		return -EINVAL;

	strscpy((char *)input->name, "OV5640 via PL frame difference",
		sizeof(input->name));
	input->type = V4L2_INPUT_TYPE_CAMERA;
	return 0;
}

static int studyboard_v4l2_get_input(struct file *file, void *private,
				     unsigned int *index)
{
	*index = 0;
	return 0;
}

static int studyboard_v4l2_set_input(struct file *file, void *private,
				     unsigned int index)
{
	return index == 0 ? 0 : -EINVAL;
}

static const struct v4l2_ioctl_ops studyboard_v4l2_ioctl_operations = {
	.vidioc_querycap = studyboard_v4l2_query_capability,
	.vidioc_enum_fmt_vid_cap = studyboard_v4l2_enum_format,
	.vidioc_g_fmt_vid_cap = studyboard_v4l2_get_format,
	.vidioc_try_fmt_vid_cap = studyboard_v4l2_try_format,
	.vidioc_s_fmt_vid_cap = studyboard_v4l2_set_format,
	.vidioc_enum_framesizes = studyboard_v4l2_enum_frame_size,
	.vidioc_enum_input = studyboard_v4l2_enum_input,
	.vidioc_g_input = studyboard_v4l2_get_input,
	.vidioc_s_input = studyboard_v4l2_set_input,
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
};

/*
 * 仅允许一个 V4L2 文件持有采集通道。在 open 期间就取得
 * owner，不等到 STREAMON；这样可防止用户已 QBUF 提交描述符
 * 后，另一进程通过 sysfs 启动旧循环采集。
 */
static int studyboard_v4l2_open(struct file *file)
{
	struct studyboard_capture *capture = video_drvdata(file);
	int ret;

	mutex_lock(&capture->control_lock);
	if (capture->v4l2_owner || capture->running ||
	    capture->display_running) {
		mutex_unlock(&capture->control_lock);
		return -EBUSY;
	}
	capture->v4l2_owner = true;
	mutex_unlock(&capture->control_lock);

	ret = v4l2_fh_open(file);
	if (ret) {
		mutex_lock(&capture->control_lock);
		capture->v4l2_owner = false;
		mutex_unlock(&capture->control_lock);
	}
	return ret;
}

static int studyboard_v4l2_release(struct file *file)
{
	struct studyboard_capture *capture = video_drvdata(file);
	int ret;

	/* vb2_fop_release() 会先 STREAMOFF 并释放 MMAP 缓冲。 */
	ret = vb2_fop_release(file);
	mutex_lock(&capture->control_lock);
	/* 关闭唯一采集 fd 时也回收用户态 LCD 会话，异常退出不会永久占住显示。 */
	if (capture->lcd_owner)
		studyboard_stop_display(capture);
	capture->v4l2_owner = false;
	mutex_unlock(&capture->control_lock);
	return ret;
}

static const struct v4l2_file_operations studyboard_v4l2_file_operations = {
	.owner = THIS_MODULE,
	.open = studyboard_v4l2_open,
	.release = studyboard_v4l2_release,
	.poll = vb2_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap = vb2_fop_mmap,
};

static int studyboard_v4l2_register(struct studyboard_capture *capture)
{
	struct vb2_queue *queue = &capture->vb2_queue;
	struct video_device *video = &capture->video_device;
	int ret;

	ret = v4l2_device_register(capture->dev, &capture->v4l2_device);
	if (ret) {
		dev_err(capture->dev, "failed to register V4L2 device: %d\n",
			ret);
		return ret;
	}

	memset(queue, 0, sizeof(*queue));
	queue->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	queue->io_modes = VB2_MMAP;
	queue->drv_priv = capture;
	queue->buf_struct_size = sizeof(struct studyboard_v4l2_buffer);
	queue->ops = &studyboard_v4l2_queue_operations;
	queue->mem_ops = &vb2_dma_contig_memops;
	queue->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC |
				 V4L2_BUF_FLAG_TSTAMP_SRC_EOF;
	queue->min_buffers_needed = STUDYBOARD_V4L2_MIN_BUFFERS;
	queue->lock = &capture->v4l2_lock;
	/* 分配的缓冲必须使用 VDMA 设备的 DMA mask/地址域。 */
	queue->dev = capture->dma_dev;

	ret = vb2_queue_init(queue);
	if (ret) {
		dev_err(capture->dev, "failed to initialize vb2 queue: %d\n",
			ret);
		goto unregister_v4l2;
	}

	memset(video, 0, sizeof(*video));
	strscpy(video->name, "studyboard-ov5640",
		sizeof(video->name));
	video->v4l2_dev = &capture->v4l2_device;
	video->fops = &studyboard_v4l2_file_operations;
	video->ioctl_ops = &studyboard_v4l2_ioctl_operations;
	video->release = video_device_release_empty;
	video->lock = &capture->v4l2_lock;
	video->queue = queue;
	video->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
	video_set_drvdata(video, capture);

	/* Linux 5.4 names a normal video-capture node VFL_TYPE_GRABBER. */
	ret = video_register_device(video, VFL_TYPE_GRABBER, -1);
	if (ret) {
		dev_err(capture->dev, "failed to register video node: %d\n", ret);
		goto release_queue;
	}

	capture->v4l2_registered = true;
	dev_info(capture->dev,
		 "registered /dev/video%d as %ux%u BGR24 MMAP capture\n",
		 video->num, capture->width, capture->height);
	return 0;

release_queue:
	vb2_queue_release(queue);
unregister_v4l2:
	v4l2_device_unregister(&capture->v4l2_device);
	return ret;
}

static void studyboard_v4l2_unregister(struct studyboard_capture *capture)
{
	if (!capture->v4l2_registered)
		return;

	video_unregister_device(&capture->video_device);
	vb2_queue_release(&capture->vb2_queue);
	v4l2_device_unregister(&capture->v4l2_device);
	capture->v4l2_registered = false;
}

/*
 * 对一帧做“稀疏 FNV-1a 摘要”，用于快速判断 DDR 内容是否变化。
 *
 * 每隔 257 字节取一个样本，因此读取成本远低于扫描约 900 KiB 的完整帧。
 * 该摘要不是加密哈希，也不能证明帧完整性；只适合作为 bring-up 活动指示。
 */
static u32 studyboard_sample_digest(const u8 *frame, size_t frame_bytes)
{
	/*
	 * sysfs show 在进程上下文执行，但 DMA 可能同时改写该帧；因此这次摘要
	 * 甚至可能混合一帧更新前后的数据。READ_ONCE() 只保证每个字节执行一次
	 * 实际读取，不能冻结 DMA，也不是内存快照。
	 */
	u32 digest = 2166136261U;
	size_t offset;

	for (offset = 0; offset < frame_bytes; offset += 257U) {
		digest ^= READ_ONCE(frame[offset]);
		digest *= 16777619U;
	}

	return digest;
}

/*
 * 以下 *_show()/ *_store() 是 sysfs 属性的读写函数。
 *
 * DEVICE_ATTR_RO(name) 要求提供 name_show()，生成只读文件；
 * DEVICE_ATTR_RW(name) 要求提供 name_show()/name_store()，生成可读写文件。
 * dev_get_drvdata() 取回 probe() 中 platform_set_drvdata() 保存的 capture。
 */

/* 输出分辨率、像素字节数、行跨度及内存占用，不改变硬件状态。 */
static ssize_t geometry_show(struct device *dev,
			     struct device_attribute *attribute, char *buffer)
{
	struct studyboard_capture *capture = dev_get_drvdata(dev);

	return scnprintf(buffer, PAGE_SIZE,
			 "%ux%u bytes_per_pixel=%u stride=%u "
			 "frame_bytes=%zu frames=%u total_bytes=%zu\n",
			 capture->width, capture->height,
			 capture->bytes_per_pixel, capture->stride,
			 capture->frame_bytes, capture->frame_count,
			 capture->total_bytes);
}
static DEVICE_ATTR_RO(geometry);

/* running 对应采集 S2MM 通道：读取得到 0 或 1。 */
static ssize_t running_show(struct device *dev,
			    struct device_attribute *attribute, char *buffer)
{
	struct studyboard_capture *capture = dev_get_drvdata(dev);

	return scnprintf(buffer, PAGE_SIZE, "%u\n", capture->running ? 1 : 0);
}

static ssize_t running_store(struct device *dev,
			     struct device_attribute *attribute,
			     const char *buffer, size_t count)
{
	struct studyboard_capture *capture = dev_get_drvdata(dev);
	bool enable;
	int ret = 0;

	/* kstrtobool 接受 1/0、y/n、yes/no、true/false 等常见写法。 */
	ret = kstrtobool(buffer, &enable);
	if (ret)
		return ret;

	/* start/stop 可能睡眠，所以用 mutex，不能使用 spinlock。 */
	mutex_lock(&capture->control_lock);
	if (enable) {
		/* /dev/videoX 从 open 到 close 期间独占 VDMA0。 */
		if (capture->v4l2_owner || capture->lcd_owner)
			ret = -EBUSY;
		else
			ret = studyboard_start_capture(capture);
	}
	else {
		/*
		 * 停止采集前先停止显示，保持本驱动定义的生命周期顺序：
		 * start capture -> start display -> stop display -> stop capture。
		 */
		studyboard_stop_display(capture);
		studyboard_stop_capture(capture);
	}
	mutex_unlock(&capture->control_lock);

	/* sysfs 约定：成功返回用户写入的字节数，失败返回负 errno。 */
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(running);

/* display_running 对应显示 MM2S 通道：读取得到 0 或 1。 */
static ssize_t display_running_show(struct device *dev,
				    struct device_attribute *attribute,
				    char *buffer)
{
	struct studyboard_capture *capture = dev_get_drvdata(dev);

	return scnprintf(buffer, PAGE_SIZE, "%u\n",
			 (capture->display_running || capture->lcd_running) ? 1 : 0);
}

static ssize_t display_running_store(struct device *dev,
				     struct device_attribute *attribute,
				     const char *buffer, size_t count)
{
	struct studyboard_capture *capture = dev_get_drvdata(dev);
	bool enable;
	int ret;

	ret = kstrtobool(buffer, &enable);
	if (ret)
		return ret;

	mutex_lock(&capture->control_lock);
	if (enable)
		ret = studyboard_start_display(capture);
	else {
		studyboard_stop_display(capture);
		ret = 0;
	}
	mutex_unlock(&capture->control_lock);

	return ret ? ret : count;
}
static DEVICE_ATTR_RW(display_running);

/*
 * lcd_frame 是只写、恰好一帧长的二进制 sysfs 文件。
 * 用户态 write/pwrite 可能被 sysfs 拆成 PAGE_SIZE 小块，所以检查 offset
 * 连续递增，而不是要求一次内核回调收齐 921600 字节。
 * offset=0 表示开始/重做一帧；没有 lcd_commit=1，半帧绝不会交给 VDMA。
 */
static ssize_t lcd_frame_write(struct file *file, struct kobject *kobject,
			      struct bin_attribute *attribute, char *buffer,
			      loff_t offset, size_t count)
{
	struct studyboard_capture *capture = attribute->private;
	ssize_t ret = count;

	if (!count)
		return 0;
	mutex_lock(&capture->control_lock);
	if (!capture->v4l2_owner || capture->running || capture->display_running) {
		ret = -EBUSY;
		goto unlock;
	}
	/* 连续显示至少需要一块 front 和一块不被 MM2S 读取的 staging back。 */
	if (capture->frame_count < 2) {
		ret = -ENOSPC;
		goto unlock;
	}
	if (capture->lcd_owner && capture->lcd_owner != task_tgid(current)) {
		ret = -EBUSY;
		goto unlock;
	}
	if (offset < 0 || offset >= capture->frame_bytes ||
	    count > capture->frame_bytes - offset) {
		ret = -EINVAL;
		goto unlock;
	}
	if (!capture->lcd_owner) {
		if (offset) {
			ret = -EINVAL;
			goto unlock;
		}
		capture->lcd_owner = get_task_pid(current, PIDTYPE_TGID);
		capture->lcd_frames = 0;
	}
	if (!offset) {
		capture->lcd_written = 0;
		capture->lcd_back = capture->lcd_running ?
			(capture->lcd_front + 1) % capture->frame_count : 0;
	}
	if (offset != capture->lcd_written) {
		ret = -EINVAL;
		goto unlock;
	}
	memcpy((u8 *)capture->buffer_cpu +
	       capture->lcd_back * capture->frame_bytes + offset, buffer, count);
	capture->lcd_written += count;
unlock:
	mutex_unlock(&capture->control_lock);
	return ret;
}

/* 1=发布刚写完的图像，0=停止本进程 LCD 会话；不是 legacy display_running=1。 */
static ssize_t lcd_commit_store(struct device *dev,
				struct device_attribute *attribute,
				const char *buffer, size_t count)
{
	struct studyboard_capture *capture = dev_get_drvdata(dev);
	bool enable;
	int ret = kstrtobool(buffer, &enable);

	if (ret)
		return ret;
	mutex_lock(&capture->control_lock);
	if (capture->lcd_owner && capture->lcd_owner != task_tgid(current))
		ret = -EBUSY;
	else if (!enable) {
		/* 没有 LCD 会话时不影响旧的诊断显示。 */
		if (capture->lcd_owner)
			studyboard_stop_display(capture);
	} else if (!capture->lcd_owner ||
		   capture->lcd_written != capture->frame_bytes)
		ret = -ENODATA;
	else
		ret = studyboard_publish_lcd(capture);
	mutex_unlock(&capture->control_lock);
	return ret ? ret : count;
}
static DEVICE_ATTR_WO(lcd_commit);

static ssize_t lcd_status_show(struct device *dev,
			       struct device_attribute *attribute, char *buffer)
{
	struct studyboard_capture *capture = dev_get_drvdata(dev);
	ssize_t ret;

	mutex_lock(&capture->control_lock);
	ret = scnprintf(buffer, PAGE_SIZE,
		"running=%u mode=continuous-copy frames=%llu staged_bytes=%zu frame_bytes=%zu front=%u back=%u owner=%d\n",
		capture->lcd_running, (unsigned long long)capture->lcd_frames,
		capture->lcd_written, capture->frame_bytes,
		capture->lcd_front, capture->lcd_back,
		pid_nr(capture->lcd_owner));
	mutex_unlock(&capture->control_lock);
	return ret;
}
static DEVICE_ATTR_RO(lcd_status);

/*
 * 以下四项显示“本驱动描述符回调”统计。
 * 请勿把 callback_count 当成实时帧计数器：进入 VDMA 硬件循环后，硬件中断
 * 可以继续增加，而软件只提交了启动阶段的有限个描述符。
 */
static ssize_t callback_count_show(struct device *dev,
				   struct device_attribute *attribute,
				   char *buffer)
{
	struct studyboard_capture *capture = dev_get_drvdata(dev);

	return scnprintf(buffer, PAGE_SIZE, "%lld\n",
			 (long long)atomic64_read(&capture->callback_count));
}
static DEVICE_ATTR_RO(callback_count);

static ssize_t last_frame_show(struct device *dev,
			       struct device_attribute *attribute, char *buffer)
{
	struct studyboard_capture *capture = dev_get_drvdata(dev);

	return scnprintf(buffer, PAGE_SIZE, "%d\n",
			 atomic_read(&capture->last_frame));
}
static DEVICE_ATTR_RO(last_frame);

static ssize_t display_callback_count_show(struct device *dev,
					   struct device_attribute *attribute,
					   char *buffer)
{
	struct studyboard_capture *capture = dev_get_drvdata(dev);

	return scnprintf(buffer, PAGE_SIZE, "%lld\n",
			 (long long)atomic64_read(
				 &capture->display_callback_count));
}
static DEVICE_ATTR_RO(display_callback_count);

static ssize_t display_last_frame_show(struct device *dev,
				       struct device_attribute *attribute,
				       char *buffer)
{
	struct studyboard_capture *capture = dev_get_drvdata(dev);

	return scnprintf(buffer, PAGE_SIZE, "%d\n",
			 atomic_read(&capture->display_last_frame));
}
static DEVICE_ATTR_RO(display_last_frame);

/* 列出每个帧缓冲的 DMA 地址、相对总缓冲区的偏移和单帧大小。 */
static ssize_t dma_buffers_show(struct device *dev,
				struct device_attribute *attribute,
				char *buffer)
{
	struct studyboard_capture *capture = dev_get_drvdata(dev);
	ssize_t length = 0;
	u32 index;

	for (index = 0; index < capture->frame_count; ++index) {
		dma_addr_t address = capture->buffer_dma +
				(index * capture->frame_bytes);

		/* %pad 是内核打印 dma_addr_t 的可移植格式。 */
		length += scnprintf(buffer + length, PAGE_SIZE - length,
				    "frame%u dma=%pad offset=0x%zx size=%zu\n",
				    index, &address,
				    index * capture->frame_bytes,
				    capture->frame_bytes);
	}

	return length;
}
static DEVICE_ATTR_RO(dma_buffers);

/* 逐帧计算稀疏摘要；运行时连续读取，画面变化时摘要通常也会变化。 */
static ssize_t sample_digests_show(struct device *dev,
				   struct device_attribute *attribute,
				   char *buffer)
{
	struct studyboard_capture *capture = dev_get_drvdata(dev);
	const u8 *base = capture->buffer_cpu;
	ssize_t length = 0;
	u32 index;

	for (index = 0; index < capture->frame_count; ++index) {
		const u8 *frame = base + (index * capture->frame_bytes);
		u32 digest = studyboard_sample_digest(frame,
						       capture->frame_bytes);

		length += scnprintf(buffer + length, PAGE_SIZE - length,
				    "frame%u=0x%08x\n", index, digest);
	}

	return length;
}
static DEVICE_ATTR_RO(sample_digests);

/*
 * snapshot_frame 是“选择并冻结哪一个 DMA 帧”的控制属性。
 *
 * 读取：尚未生成快照时输出 -1，成功后输出 0/1/2。
 * 写入：写帧下标，把对应 DMA 缓冲完整复制到 snapshot_cpu。
 *
 * 为什么要求两条 VDMA 都停止？
 *   dma_alloc_coherent() 只解决缓存一致性，不会阻止硬件在 CPU memcpy()
 *   期间改写同一帧。只有先停采集，源数据才不会边复制边变化。显示通道
 *   虽然只读 DDR，但也一并要求停止，使本阶段的缓冲所有权最简单明确。
 */
static ssize_t snapshot_frame_show(struct device *dev,
				   struct device_attribute *attribute,
				   char *buffer)
{
	struct studyboard_capture *capture = dev_get_drvdata(dev);
	int index;

	mutex_lock(&capture->control_lock);
	index = capture->snapshot_valid ? (int)capture->snapshot_index : -1;
	mutex_unlock(&capture->control_lock);

	return scnprintf(buffer, PAGE_SIZE, "%d\n", index);
}

static ssize_t snapshot_frame_store(struct device *dev,
				    struct device_attribute *attribute,
				    const char *buffer, size_t count)
{
	struct studyboard_capture *capture = dev_get_drvdata(dev);
	const u8 *source;
	u32 index;
	int ret;

	ret = kstrtou32(buffer, 0, &index);
	if (ret)
		return ret;
	if (index >= capture->frame_count)
		return -EINVAL;

	mutex_lock(&capture->control_lock);
	if (capture->running || capture->display_running ||
	    capture->v4l2_owner) {
		ret = -EBUSY;
		goto unlock;
	}

	source = (const u8 *)capture->buffer_cpu +
		 (index * capture->frame_bytes);
	memcpy(capture->snapshot_cpu, source, capture->frame_bytes);
	capture->snapshot_index = index;
	capture->snapshot_valid = true;
	ret = 0;

	dev_info(capture->dev, "froze frame%u into userspace snapshot\n",
		 index);

unlock:
	mutex_unlock(&capture->control_lock);
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(snapshot_frame);

/*
 * snapshot_raw 是 sysfs binary attribute 的读函数。
 *
 * 普通 DEVICE_ATTR 的 show() 最多返回一页文本，放不下 921600 字节图像；
 * bin_attribute 支持带 offset/count 的分段读取，因此 cat/dd 会多次调用本
 * 函数，直到读完 frame_bytes。这里只读取独立的 snapshot_cpu，不直接把
 * 正被 VDMA 使用的 coherent 内存暴露给用户。
 */
static ssize_t snapshot_raw_read(struct file *file, struct kobject *kobject,
				 struct bin_attribute *attribute, char *buffer,
				 loff_t offset, size_t count)
{
	struct studyboard_capture *capture = attribute->private;
	ssize_t copied;

	if (offset < 0)
		return -EINVAL;

	mutex_lock(&capture->control_lock);
	if (!capture->snapshot_valid) {
		copied = -ENODATA;
		goto unlock;
	}
	if ((size_t)offset >= capture->frame_bytes) {
		copied = 0;
		goto unlock;
	}

	count = min_t(size_t, count, capture->frame_bytes - (size_t)offset);
	memcpy(buffer, (const u8 *)capture->snapshot_cpu + offset, count);
	copied = count;

unlock:
	mutex_unlock(&capture->control_lock);
	return copied;
}

/*
 * 把所有属性收集为一个 group。probe() 一次性创建，remove() 一次性删除。
 * 最后的 NULL 是属性数组终止标记。
 */
static struct attribute *studyboard_capture_attributes[] = {
	&dev_attr_geometry.attr,
	&dev_attr_running.attr,
	&dev_attr_display_running.attr,
	&dev_attr_lcd_commit.attr,
	&dev_attr_lcd_status.attr,
	&dev_attr_callback_count.attr,
	&dev_attr_last_frame.attr,
	&dev_attr_display_callback_count.attr,
	&dev_attr_display_last_frame.attr,
	&dev_attr_dma_buffers.attr,
	&dev_attr_sample_digests.attr,
	&dev_attr_snapshot_frame.attr,
	NULL,
};

static const struct attribute_group studyboard_capture_attribute_group = {
	.attrs = studyboard_capture_attributes,
};

/*
 * 从设备树读取帧几何参数并计算 DDR 布局。
 *
 * 对应节点示例：
 *   studyboard-frame-capture {
 *       compatible = "studyboard,frame-capture-1.0";
 *       dmas = <&axi_vdma_0 1>, <&axi_vdma_1 0>;
 *       dma-names = "capture", "display";
 *       width = <640>;
 *       height = <480>;
 *       bytes-per-pixel = <3>;
 *       frame-count = <3>;
 *   };
 *
 * 驱动只知道字节布局，不理解 R/G/B 颜色语义；因此这里的 bpp 必须与
 * PL AXI4-Stream、VDMA 和 LCD 端的 RGB888 配置一致。
 */
static int studyboard_read_properties(struct platform_device *pdev,
				      struct studyboard_capture *capture)
{
	struct device_node *node = pdev->dev.of_node;

	/* 先放入默认值；of_property_read_u32() 找不到属性时不会覆盖它们。 */
	capture->width = STUDYBOARD_DEFAULT_WIDTH;
	capture->height = STUDYBOARD_DEFAULT_HEIGHT;
	capture->bytes_per_pixel = STUDYBOARD_DEFAULT_BYTES_PER_PIXEL;
	capture->frame_count = STUDYBOARD_DEFAULT_FRAME_COUNT;

	of_property_read_u32(node, "width", &capture->width);
	of_property_read_u32(node, "height", &capture->height);
	of_property_read_u32(node, "bytes-per-pixel",
			     &capture->bytes_per_pixel);
	of_property_read_u32(node, "frame-count", &capture->frame_count);

	/*
	 * 拒绝 0 或超出本教学驱动预期范围的参数，同时保证 frame_count 不会
	 * 越过固定长度数组。此处上限不是 Zynq/VDMA 的完整能力规格。
	 */
	if (!capture->width || capture->width > 4096U ||
	    !capture->height || capture->height > 2160U ||
	    !capture->bytes_per_pixel || capture->bytes_per_pixel > 4U ||
	    !capture->frame_count ||
	    capture->frame_count > STUDYBOARD_MAX_FRAMES) {
		dev_err(&pdev->dev, "invalid capture geometry in device tree\n");
		return -EINVAL;
	}

	/* 当前像素紧密排列：一行字节数 = 宽度 * 每像素字节数。 */
	capture->stride = capture->width * capture->bytes_per_pixel;
	capture->frame_bytes = (size_t)capture->stride * capture->height;
	capture->total_bytes = capture->frame_bytes * capture->frame_count;
	return 0;
}

/*
 * platform_driver 的 probe：设备树匹配成功后由内核调用一次。
 *
 * 执行顺序：
 *   1. 分配/初始化驱动状态；
 *   2. 读取设备树几何参数；
 *   3. 按 dma-names 取得 capture 和 display 两个 DMAEngine 通道；
 *   4. 分配三帧 coherent DDR 缓冲区；
 *   5. 分配一个只供 CPU 使用的冻结快照区；
	 *   6. 创建 sysfs 控制、诊断和二进制快照文件；
	 *   7. 初始化 videobuf2 队列并注册 /dev/videoX。
 *
 * probe 只准备资源，不自动启动摄像头或 VDMA，所以模块加载完成后
 * running/display_running 仍为 0，便于按阶段测试。
 */
static int studyboard_capture_probe(struct platform_device *pdev)
{
	struct studyboard_capture *capture;
	u32 index;
	int ret;

	/*
	 * devm_kzalloc() 返回清零后的设备托管内存；设备解除绑定时由内核自动
	 * 释放，所以 remove() 不需要 kfree(capture)。
	 */
	capture = devm_kzalloc(&pdev->dev, sizeof(*capture), GFP_KERNEL);
	if (!capture)
		return -ENOMEM;

	/* 初始化所有同步原语和“尚未完成”的统计初值。 */
	capture->dev = &pdev->dev;
	mutex_init(&capture->control_lock);
	mutex_init(&capture->v4l2_lock);
	spin_lock_init(&capture->v4l2_queued_lock);
	INIT_LIST_HEAD(&capture->v4l2_queued_buffers);
	capture->v4l2_streaming = false;
	capture->v4l2_owner = false;
	capture->v4l2_registered = false;
	init_completion(&capture->descriptor_done);
	init_completion(&capture->display_descriptor_done);
	atomic64_set(&capture->callback_count, 0);
	atomic64_set(&capture->display_callback_count, 0);
	atomic_set(&capture->last_frame, -1);
	atomic_set(&capture->display_last_frame, -1);

	ret = studyboard_read_properties(pdev, capture);
	if (ret)
		return ret;

	/*
	 * 名字 "capture" 对应设备树 dma-names 的第一项，DMA OF 层据此解析
	 * dmas phandle/通道号，并返回 VDMA0 S2MM 的 struct dma_chan。
	 */
	capture->channel = dma_request_chan(&pdev->dev, "capture");
	if (IS_ERR(capture->channel)) {
		ret = PTR_ERR(capture->channel);
		/*
		 * -EPROBE_DEFER 表示底层 VDMA 驱动尚未就绪；不打印硬错误，内核会
		 * 稍后自动重试本 probe。其他错误才值得立即报告。
		 */
		if (ret != -EPROBE_DEFER)
			dev_err(&pdev->dev,
				"failed to request VDMA capture channel: %d\n", ret);
		return ret;
	}

	/* 同理取得 VDMA1 MM2S 通道；失败时要释放已经取得的 capture 通道。 */
	capture->display_channel = dma_request_chan(&pdev->dev, "display");
	if (IS_ERR(capture->display_channel)) {
		ret = PTR_ERR(capture->display_channel);
		if (ret != -EPROBE_DEFER)
			dev_err(&pdev->dev,
				"failed to request VDMA display channel: %d\n",
				ret);
		goto release_capture_channel;
	}

	/*
	 * coherent 分配需要一个 DMA 设备，用来应用该设备的 DMA mask/映射规则。
	 * 返回的内存同时具备 CPU 地址 buffer_cpu 和 DMA 地址 buffer_dma，且在
	 * 当前平台上 CPU 与 DMA 访问保持一致，无需手工 cache flush/invalidate。
	 */
	capture->dma_dev = capture->channel->device->dev;
	capture->buffer_cpu = dma_alloc_coherent(capture->dma_dev,
						 capture->total_bytes,
						 &capture->buffer_dma,
						 GFP_KERNEL);
	if (!capture->buffer_cpu) {
		dev_err(&pdev->dev,
			"failed to allocate %zu-byte coherent frame buffer\n",
			capture->total_bytes);
		ret = -ENOMEM;
		goto release_display_channel;
	}

	/* 在第一次摄像头帧到来前把三个缓冲区初始化为全 0。 */
	memset(capture->buffer_cpu, 0, capture->total_bytes);

	/*
	 * 快照不需要物理连续，也不交给 DMA，所以用 vzalloc()：它更适合申请
	 * 约 900 KiB 的大块 CPU 内存，避免要求伙伴系统提供连续物理页。
	 */
	capture->snapshot_cpu = vzalloc(capture->frame_bytes);
	if (!capture->snapshot_cpu) {
		dev_err(&pdev->dev,
			"failed to allocate %zu-byte snapshot buffer\n",
			capture->frame_bytes);
		ret = -ENOMEM;
		goto free_buffer;
	}
	capture->snapshot_valid = false;
	capture->snapshot_index = 0;

	/* 以后所有 sysfs 回调都通过 dev_get_drvdata() 取回这个指针。 */
	platform_set_drvdata(pdev, capture);

	dev_info(&pdev->dev,
		 "allocated %zu coherent bytes at DMA address %pad\n",
		 capture->total_bytes, &capture->buffer_dma);
	for (index = 0; index < capture->frame_count; ++index) {
		dma_addr_t address = capture->buffer_dma +
				(index * capture->frame_bytes);

		dev_info(&pdev->dev, "frame%u DMA address %pad\n",
			 index, &address);
	}

	/*
	 * 成功后文件出现在：
	 * /sys/bus/platform/devices/studyboard-frame-capture/
	 */
	ret = sysfs_create_group(&pdev->dev.kobj,
				 &studyboard_capture_attribute_group);
	if (ret) {
		dev_err(&pdev->dev, "failed to create status attributes: %d\n",
			ret);
		goto free_snapshot;
	}

	/*
	 * 配置大文件 snapshot_raw。attr.size 让 sysfs 把文件长度报告为一帧；
	 * private 保存驱动实例，供 snapshot_raw_read() 找到快照缓冲区。
	 */
	sysfs_bin_attr_init(&capture->snapshot_bin_attribute);
	capture->snapshot_bin_attribute.attr.name = "snapshot_raw";
	capture->snapshot_bin_attribute.attr.mode = 0444;
	capture->snapshot_bin_attribute.size = capture->frame_bytes;
	capture->snapshot_bin_attribute.read = snapshot_raw_read;
	capture->snapshot_bin_attribute.private = capture;

	ret = sysfs_create_bin_file(&pdev->dev.kobj,
				    &capture->snapshot_bin_attribute);
	if (ret) {
		dev_err(&pdev->dev,
			"failed to create binary snapshot attribute: %d\n", ret);
		goto remove_attribute_group;
	}

	sysfs_bin_attr_init(&capture->lcd_bin_attribute);
	capture->lcd_bin_attribute.attr.name = "lcd_frame";
	capture->lcd_bin_attribute.attr.mode = 0200;
	capture->lcd_bin_attribute.size = capture->frame_bytes;
	capture->lcd_bin_attribute.write = lcd_frame_write;
	capture->lcd_bin_attribute.private = capture;
	ret = sysfs_create_bin_file(&pdev->dev.kobj, &capture->lcd_bin_attribute);
	if (ret)
		goto remove_binary_attribute;

	ret = studyboard_v4l2_register(capture);
	if (ret)
		goto remove_lcd_attribute;

	dev_info(&pdev->dev,
		 "video DMA driver ready; use sysfs diagnostics or /dev/video%d\n",
		 capture->video_device.num);
	return 0;

remove_lcd_attribute:
	sysfs_remove_bin_file(&pdev->dev.kobj, &capture->lcd_bin_attribute);
remove_binary_attribute:
	sysfs_remove_bin_file(&pdev->dev.kobj,
			      &capture->snapshot_bin_attribute);
remove_attribute_group:
	sysfs_remove_group(&pdev->dev.kobj,
			   &studyboard_capture_attribute_group);
free_snapshot:
	vfree(capture->snapshot_cpu);
free_buffer:
	/*
	 * probe 错误回滚采用倒序释放：sysfs 创建失败后依次释放缓冲、display
	 * 通道、capture 通道。每个 goto 只跳到仍需释放的那一级。
	 */
	dma_free_coherent(capture->dma_dev, capture->total_bytes,
			  capture->buffer_cpu, capture->buffer_dma);
release_display_channel:
	dma_release_channel(capture->display_channel);
release_capture_channel:
	dma_release_channel(capture->channel);
	return ret;
}

/*
 * remove：模块卸载或设备解绑时调用。
 * 顺序很重要：先阻止新的 sysfs 操作，再停 DMA，最后释放 DMA 正在使用的
 * 缓冲区和通道。若先 free 缓冲而硬件仍在搬运，会造成 use-after-free/DMA
 * 写坏其他内存。
 */
static int studyboard_capture_remove(struct platform_device *pdev)
{
	struct studyboard_capture *capture = platform_get_drvdata(pdev);

	/* 删除用户入口，阻止新的启停请求。 */
	studyboard_v4l2_unregister(capture);
	sysfs_remove_bin_file(&pdev->dev.kobj, &capture->lcd_bin_attribute);
	sysfs_remove_bin_file(&pdev->dev.kobj,
			      &capture->snapshot_bin_attribute);
	sysfs_remove_group(&pdev->dev.kobj,
			   &studyboard_capture_attribute_group);
	mutex_lock(&capture->control_lock);
	studyboard_stop_display(capture);
	studyboard_stop_capture(capture);
	mutex_unlock(&capture->control_lock);
	/* snapshot_cpu 只供 CPU 使用，对应 probe() 中的 vzalloc()。 */
	vfree(capture->snapshot_cpu);
	/* 此时两个通道均已停止，才能安全释放帧缓冲和 DMA 通道。 */
	dma_free_coherent(capture->dma_dev, capture->total_bytes,
			  capture->buffer_cpu, capture->buffer_dma);
	dma_release_channel(capture->display_channel);
	dma_release_channel(capture->channel);
	return 0;
}

/*
 * OF（Open Firmware/设备树）匹配表。
 * 内核在设备树中找到相同 compatible 后，将节点实例与本驱动绑定。
 */
static const struct of_device_id studyboard_capture_of_match[] = {
	{ .compatible = "studyboard,frame-capture-1.0" },
	{ }
};
MODULE_DEVICE_TABLE(of, studyboard_capture_of_match);

/* platform_driver 把生命周期函数和设备树匹配表组织在一起。 */
static struct platform_driver studyboard_capture_driver = {
	.probe = studyboard_capture_probe,
	.remove = studyboard_capture_remove,
	.driver = {
		.name = "studyboard-frame-capture",
		.of_match_table = studyboard_capture_of_match,
	},
};
/*
 * 自动生成模块 init/exit：
 *   insmod/modprobe 时注册 platform_driver；
 *   rmmod 时注销，进而触发 remove()。
 */
module_platform_driver(studyboard_capture_driver);

MODULE_AUTHOR("StudyBoard Linux port");
MODULE_DESCRIPTION("StudyBoard OV5640 VDMA sysfs and V4L2 capture driver");
MODULE_LICENSE("GPL");
