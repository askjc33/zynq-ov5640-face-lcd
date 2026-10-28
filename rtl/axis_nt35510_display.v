`timescale 1ns / 1ps

// Stage-2 LCD display backend.
//
// Reuse of the proven color-bar design is deliberate:
//   1. it performs reset and the known-good NT35510 register sequence;
//   2. it writes one full-screen color-bar self-test;
//   3. lcd_bl rising is the hand-off point to the AXI frame writer;
//   4. the AXI writer clears the screen and then displays centered video.
//
// This gives an observable boot sequence instead of hiding initialization in a
// black box.  If color bars appear but live video does not, the fault is after
// the LCD hardware/init layer and before or inside the AXI stream layer.
module axis_nt35510_display #(
    parameter integer CLK_HZ = 100_000_000
) (
    (* X_INTERFACE_PARAMETER = "ASSOCIATED_BUSIF S_AXIS, ASSOCIATED_RESET rst_n" *)
    (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 clk CLK" *)
    input  wire        clk,
    (* X_INTERFACE_PARAMETER = "POLARITY ACTIVE_LOW" *)
    (* X_INTERFACE_INFO = "xilinx.com:signal:reset:1.0 rst_n RST" *)
    input  wire        rst_n,

    (* X_INTERFACE_PARAMETER = "TDATA_NUM_BYTES 3, TUSER_WIDTH 1, HAS_TLAST 1, HAS_TREADY 1" *)
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS TDATA" *)
    input  wire [23:0] s_axis_tdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS TVALID" *)
    input  wire        s_axis_tvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS TREADY" *)
    output wire        s_axis_tready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS TUSER" *)
    input  wire        s_axis_tuser,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS TLAST" *)
    input  wire        s_axis_tlast,

    output wire [15:0] lcd_data,
    output wire        lcd_cs_n,
    output wire        lcd_rs,
    output wire        lcd_wr_n,
    output wire        lcd_rd_n,
    output wire        lcd_rst_n,
    output wire        lcd_bl
);

wire [15:0] init_data;
wire        init_cs_n;
wire        init_rs;
wire        init_wr_n;
wire        init_rd_n;
wire        init_rst_n;
wire        init_bl;

wire [15:0] stream_data;
wire        stream_cs_n;
wire        stream_rs;
wire        stream_wr_n;

wire        clear_done;
wire        protocol_error;
wire [31:0] frame_count;

atk_md0430_colorbar_top #(
    .CLK_HZ     (CLK_HZ),
    .H_RES      (800),
    .V_RES      (480),
    .SIMULATION (0),
    .WRITE_CLK_DIV (2)
) u_known_good_init (
    .sys_clk   (clk),
    .sys_rst_n (rst_n),
    .lcd_data  (init_data),
    .lcd_cs_n  (init_cs_n),
    .lcd_rs    (init_rs),
    .lcd_wr_n  (init_wr_n),
    .lcd_rd_n  (init_rd_n),
    .lcd_rst_n (init_rst_n),
    .lcd_bl    (init_bl)
);

axis_lcd_frame_writer #(
    .INPUT_WIDTH  (640),
    .INPUT_HEIGHT (480),
    .LCD_WIDTH    (800),
    .LCD_HEIGHT   (480),
    .X_OFFSET     (80),
    .Y_OFFSET     (0)
) u_stream_writer (
    .clk                   (clk),
    .rst_n                 (rst_n),
    .enable                (init_bl),
    .s_axis_tdata          (s_axis_tdata),
    .s_axis_tvalid         (s_axis_tvalid),
    .s_axis_tready         (s_axis_tready),
    .s_axis_tuser          (s_axis_tuser),
    .s_axis_tlast          (s_axis_tlast),
    .lcd_data              (stream_data),
    .lcd_cs_n              (stream_cs_n),
    .lcd_rs                (stream_rs),
    .lcd_wr_n              (stream_wr_n),
    .init_clear_done       (clear_done),
    .axis_protocol_error   (protocol_error),
    .displayed_frame_count (frame_count)
);

// The color-bar engine is idle before it raises init_bl, so this synchronous
// hand-over never changes bus ownership in the middle of a write cycle.
assign lcd_data  = init_bl ? stream_data : init_data;
assign lcd_cs_n  = init_bl ? stream_cs_n : init_cs_n;
assign lcd_rs    = init_bl ? stream_rs   : init_rs;
assign lcd_wr_n  = init_bl ? stream_wr_n : init_wr_n;
assign lcd_rd_n  = 1'b1;
assign lcd_rst_n = init_rst_n;
assign lcd_bl    = init_bl;

endmodule
