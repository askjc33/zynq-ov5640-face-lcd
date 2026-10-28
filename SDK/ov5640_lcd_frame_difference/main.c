// STUDYBOARD OV5640 + frame difference + ATK-MD0430 stage-2 application.
//
// The PS only configures SCCB and the two VDMAs.  LCD initialization,
// RGB888->RGB565 conversion, centering, and 8080 writes are implemented in PL.

#include "xil_types.h"
#include "xil_io.h"
#include "xil_cache.h"
#include <string.h>
#include "xparameters.h"
#include "sleep.h"
#include "xaxivdma.h"
#include "vdma_api/vdma_api.h"
#include "emio_sccb_cfg/emio_sccb_cfg.h"
#include "ov5640/ov5640_init.h"

#define VDMA_CAPTURE_ID XPAR_AXIVDMA_0_DEVICE_ID
#define VDMA_LCD_ID     XPAR_AXIVDMA_1_DEVICE_ID

#define VIDEO_WIDTH     640U
#define VIDEO_HEIGHT    480U

#define SLCR_LOCK_ADDR          0xF8000004U
#define SLCR_UNLOCK_ADDR        0xF8000008U
#define SLCR_FPGA_RST_CTRL_ADDR 0xF8000240U
#define SLCR_LOCK_KEY           0x0000767BU
#define SLCR_UNLOCK_KEY         0x0000DF0DU
#define FPGA_RESET0_MASK        0x00000001U

// Three RGB888 frame stores occupy 640*480*3*3 = 2,764,800 bytes.
// Keeping them 16 MiB above DDR base leaves ample room for the application.
static const u32 frame_buffer_addr =
        XPAR_PS7_DDR_0_S_AXI_BASEADDR + 0x01000000U;

static XAxiVdma capture_vdma;
static XAxiVdma lcd_vdma;

static void reset_video_fabric(void)
{
    u32 reset_value = Xil_In32(SLCR_FPGA_RST_CTRL_ADDR);

    Xil_Out32(SLCR_UNLOCK_ADDR, SLCR_UNLOCK_KEY);
    Xil_Out32(SLCR_FPGA_RST_CTRL_ADDR, reset_value | FPGA_RESET0_MASK);
    usleep(1000);
    Xil_Out32(SLCR_FPGA_RST_CTRL_ADDR, reset_value & ~FPGA_RESET0_MASK);
    Xil_Out32(SLCR_LOCK_ADDR, SLCR_LOCK_KEY);

    // proc_sys_reset and the 24 MHz camera Clocking Wizard need time to settle.
    usleep(100000);
    xil_printf("PL video fabric reset complete: FPGA_RST_CTRL=0x%08x\r\n",
            Xil_In32(SLCR_FPGA_RST_CTRL_ADDR));
}

static int initialize_camera(void)
{
    u16 cam_id = 0;
    u8 attempt;
    int status;

    emio_init();
    xil_printf("SCCB idle level: SCL=%d SDA=%d (expected 1/1)\r\n",
            emio_read_scl(), emio_read_sda());

    for (attempt = 1; attempt <= 5; ++attempt) {
        usleep(20000);
        cam_id = ov5640_read_id();
        xil_printf("OV5640 ID attempt %d: 0x%04x (expected 0x5640)\r\n",
                attempt, cam_id);
        if (cam_id == 0x5640)
            break;
    }

    if (cam_id != 0x5640) {
        xil_printf("FATAL: camera SCCB communication failed.\r\n");
        xil_printf("Check 3.3V, RESET=1, PWDN=0, XCLK=24MHz and SCL/SDA.\r\n");
        return XST_FAILURE;
    }

    // Active video is 640x480.  The larger totals control blanking/frame rate
    // in the existing, already proven OV5640 register routine.
    status = ov5640_init(VIDEO_WIDTH, VIDEO_HEIGHT, 2570, 980);
    if (status != 0) {
        xil_printf("FATAL: OV5640 register configuration failed: %d\r\n",
                status);
        return XST_FAILURE;
    }

    xil_printf("OV5640 register configuration successful (640x480).\r\n");
    return XST_SUCCESS;
}

int main(void)
{
    int status;

    xil_printf("\r\nSTUDYBOARD OV5640 frame-difference LCD stage-2 build\r\n");
    xil_printf("LCD boot sequence: color bars -> black clear -> live 640x480.\r\n");

    reset_video_fabric();
    if (initialize_camera() != XST_SUCCESS)
        while (1) { }

    // Make unread frame stores deterministic during initial capture. Flush
    // CPU cache before DMA starts; CPU does not touch these buffers afterward.
    memset((void *)frame_buffer_addr, 0, VIDEO_WIDTH * VIDEO_HEIGHT * 3U * 3U);
    Xil_DCacheFlushRange(frame_buffer_addr, VIDEO_WIDTH * VIDEO_HEIGHT * 3U * 3U);

    // S2MM: processed AXI video -> three RGB888 frame buffers in DDR.
    status = run_vdma_frame_buffer(&capture_vdma, VDMA_CAPTURE_ID,
            VIDEO_WIDTH, VIDEO_HEIGHT, frame_buffer_addr,
            0, 0, ONLY_WRITE);
    if (status != XST_SUCCESS) {
        xil_printf("FATAL: VDMA0 capture initialization/start failed.\r\n");
        while (1) { }
    }
    xil_printf("VDMA0 started: frame-difference output -> DDR (HP0).\r\n");

    // MM2S: the same frame stores -> PL LCD writer.  During NT35510 init and
    // screen clear, TREADY is low; AXI guarantees the VDMA holds the first
    // start-of-frame word until the LCD backend is ready.
    status = run_vdma_frame_buffer(&lcd_vdma, VDMA_LCD_ID,
            VIDEO_WIDTH, VIDEO_HEIGHT, frame_buffer_addr,
            0, 0, ONLY_READ);
    if (status != XST_SUCCESS) {
        xil_printf("FATAL: VDMA1 LCD initialization/start failed.\r\n");
        while (1) { }
    }
    xil_printf("VDMA1 started: DDR (HP1) -> centered 8080 LCD stream.\r\n");

    // The PL self-test + LCD clear takes roughly 0.6 s at 100 MHz.  Delay long
    // enough to report persistent rather than startup-transient DMA status.
    usleep(1500000);
    xil_printf("VDMA0 runtime: S2MM_SR=0x%08x ERR=0x%03x\r\n",
            XAxiVdma_GetStatus(&capture_vdma, XAXIVDMA_WRITE),
            XAxiVdma_GetDmaChannelErrors(&capture_vdma, XAXIVDMA_WRITE));
    xil_printf("VDMA1 runtime: MM2S_SR=0x%08x ERR=0x%03x\r\n",
            XAxiVdma_GetStatus(&lcd_vdma, XAXIVDMA_READ),
            XAxiVdma_GetDmaChannelErrors(&lcd_vdma, XAXIVDMA_READ));

    xil_printf("LCD pipeline is free-running; CPU setup is complete.\r\n");
    while (1) { usleep(1000000); }
}
