//****************************************Copyright (c)***********************************//
//原子哥在线教学平台：www.yuanzige.com
//技术支持：www.openedv.com
//淘宝店铺：http://openedv.taobao.com
//关注微信公众平台微信号："正点原子"，免费获取ZYNQ & FPGA & STM32 & LINUX资料。
//版权所有，盗版必究。
//Copyright(C) 正点原子 2018-2028
//All rights reserved
//----------------------------------------------------------------------------------------
// File name:           main.c
// Last modified Date:  2019/07/07 15:59:46
// Last Version:        V1.0
// Descriptions:        OV5640摄像头驱动LCD示例
//----------------------------------------------------------------------------------------
// Created by:          正点原子
// Created date:        2019/07/07 15:59:52
// Version:             V1.0
// Descriptions:        The original version
//
//----------------------------------------------------------------------------------------
//****************************************************************************************//

/***************************** Include Files *********************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xil_types.h"
#include "xil_cache.h"
#include "xil_io.h"
#include "xparameters.h"
#include "sleep.h"
#include "xaxivdma.h"
#include "xaxivdma_i.h"
#include "display_ctrl_hdmi/display_ctrl.h"
#include "vdma_api/vdma_api.h"
#include "emio_sccb_cfg/emio_sccb_cfg.h"
#include "ov5640/ov5640_init.h"

//宏定义
#define DYNCLK_BASEADDR  XPAR_AXI_DYNCLK_0_BASEADDR  //动态时钟基地址
#define VDMA_ID          XPAR_AXIVDMA_0_DEVICE_ID    //VDMA器件ID
#define VDMA_ID1         XPAR_AXIVDMA_1_DEVICE_ID
#define SLCR_LOCK_ADDR          0xF8000004U
#define SLCR_UNLOCK_ADDR        0xF8000008U
#define SLCR_FPGA_RST_CTRL_ADDR 0xF8000240U
#define SLCR_LOCK_KEY           0x0000767BU
#define SLCR_UNLOCK_KEY         0x0000DF0DU
#define FPGA_RESET0_MASK        0x00000001U
#define DISP_VTC_ID      XPAR_VTC_0_DEVICE_ID        //VTC器件ID

//全局变量
//frame buffer的起始地址
unsigned int const frame_buffer_addr = (XPAR_PS7_DDR_0_S_AXI_BASEADDR
										+ 0x1000000);
XAxiVdma     vdma;
XAxiVdma     vdma1;
DisplayCtrl  dispCtrl;
VideoMode    vd_mode;

static void reset_video_fabric(void)
{
	u32 reset_value;

	reset_value = Xil_In32(SLCR_FPGA_RST_CTRL_ADDR);
	Xil_Out32(SLCR_UNLOCK_ADDR, SLCR_UNLOCK_KEY);
	Xil_Out32(SLCR_FPGA_RST_CTRL_ADDR,
			reset_value | FPGA_RESET0_MASK);
	usleep(1000);
	Xil_Out32(SLCR_FPGA_RST_CTRL_ADDR,
			reset_value & ~FPGA_RESET0_MASK);
	Xil_Out32(SLCR_LOCK_ADDR, SLCR_LOCK_KEY);

	/* Wait for proc_sys_reset and the camera clock wizard to settle. */
	usleep(100000);
	xil_printf("PL video fabric reset complete: FPGA_RST_CTRL=0x%08x\r\n",
			Xil_In32(SLCR_FPGA_RST_CTRL_ADDR));
}

int main(void)
{
	u32 status;
	u16 cam_id;
	u8 attempt;
	u16 cmos_h_pixel;                    //ov5640 DVP 输出水平像素点数
	u16 cmos_v_pixel;                    //ov5640 DVP 输出垂直像素点数
	u16 total_h_pixel;                   //ov5640 水平总像素大小
	u16 total_v_pixel;                   //ov5640 垂直总像素大小

//	cmos_h_pixel = 1280;                 //设置OV5640输出分辨率为1280*720
//	cmos_v_pixel = 720;

	cmos_h_pixel = 640;                 //设置OV5640输出分辨率为640*480
	cmos_v_pixel = 480;

	total_h_pixel = 2570;
	total_v_pixel = 980;

	xil_printf("\r\nSTUDYBOARD OV5640 PL-history motion build\r\n");
	reset_video_fabric();
	emio_init();                         //初始化EMIO
	xil_printf("SCCB idle level: SCL=%d SDA=%d (expected 1/1)\r\n",
			emio_read_scl(), emio_read_sda());

	cam_id = 0;
	for(attempt = 1; attempt <= 5; ++attempt) {
		usleep(20000);
		cam_id = ov5640_read_id();
		xil_printf("OV5640 ID attempt %d: 0x%04x (expected 0x5640)\r\n",
				attempt, cam_id);
		if(cam_id == 0x5640)
			break;
	}

	if(cam_id != 0x5640) {
		xil_printf("Camera SCCB communication failed; video pipeline not started.\r\n");
		xil_printf("Check 3.3V, RESET=1, PWDN=0, XCLK=24MHz, SCL/SDA idle high.\r\n");
		while(1) { }
	}

	status = ov5640_init( cmos_h_pixel,  //初始化ov5640
						  cmos_v_pixel,
						 total_h_pixel,
						 total_v_pixel);
	if(status == 0)
		xil_printf("OV5640 register configuration successful.\r\n");
	else {
		xil_printf("OV5640 register configuration failed: %d\r\n", status);
		while(1) { }
	}

	vd_mode = VMODE_640x480;

	//配置VDMA
	status = run_vdma_frame_buffer(&vdma, VDMA_ID,
			vd_mode.width, vd_mode.height,
			frame_buffer_addr,0,0,ONLY_WRITE);
	if(status != XST_SUCCESS) {
		xil_printf("FATAL: VDMA0 initialization/start failed.\r\n");
		while(1) { }
	}
	xil_printf("VDMA0 started (processed-video write only).\r\n");

	//配置VDMA1
    //初始化Display controller
	status = DisplayInitialize(&dispCtrl, DISP_VTC_ID, DYNCLK_BASEADDR);
	if(status != XST_SUCCESS) {
		xil_printf("FATAL: HDMI display initialization failed.\r\n");
		while(1) { }
	}
    //设置VideoMode
	status = DisplaySetMode(&dispCtrl, &vd_mode);
	if(status != XST_SUCCESS) {
		xil_printf("FATAL: HDMI mode setup failed.\r\n");
		while(1) { }
	}
	status = DisplayStart(&dispCtrl);
	if(status != XST_SUCCESS) {
		xil_printf("FATAL: HDMI display start failed.\r\n");
		while(1) { }
	}
	xil_printf("HDMI display pipeline started at 640x480.\r\n");
	usleep(100000);

	/* Start the HDMI reader only after its pixel clock and VTC are stable. */
	status = run_vdma_frame_buffer(&vdma1, VDMA_ID1,
			vd_mode.width, vd_mode.height,
			frame_buffer_addr,0,0,ONLY_READ);
	if(status != XST_SUCCESS) {
		xil_printf("FATAL: VDMA1 initialization/start failed.\r\n");
		while(1) { }
	}
	xil_printf("VDMA1 started (HDMI read).\r\n");

	/* Let several frames pass, then report persistent DMA/data-path errors. */
	usleep(1000000);
	xil_printf("VDMA0 runtime: S2MM_SR=0x%08x ERR=0x%03x\r\n",
			XAxiVdma_GetStatus(&vdma, XAXIVDMA_WRITE),
			XAxiVdma_GetDmaChannelErrors(&vdma, XAXIVDMA_WRITE));
	xil_printf("VDMA1 runtime: MM2S_SR=0x%08x ERR=0x%03x\r\n",
			XAxiVdma_GetStatus(&vdma1, XAXIVDMA_READ),
			XAxiVdma_GetDmaChannelErrors(&vdma1, XAXIVDMA_READ));

    return 0;
}
 
