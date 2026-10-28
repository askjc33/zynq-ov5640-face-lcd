`timescale 1ns / 1ps

// Motion detector and AXI4-Stream pass-through for 640x480 RGB888 video.
//
// The original design compared the live stream with a VDMA stream that could
// be reading a frame store while it was being overwritten.  In free-running
// mode that made unrelated pixels look different and expanded the bounding box
// to almost the whole screen.  This implementation keeps a small 80x60 luma
// history in PL (one sample per 8x8 block), so the comparison is always between
// the same spatial sample in two consecutive camera frames.
module frame_difference_axis (
    (* X_INTERFACE_PARAMETER = "ASSOCIATED_BUSIF S0_AXIS:S1_AXIS:M_AXIS, ASSOCIATED_RESET rst_n" *)
    (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 clk CLK" *)
    input  wire        clk,

    (* X_INTERFACE_PARAMETER = "POLARITY ACTIVE_LOW" *)
    (* X_INTERFACE_INFO = "xilinx.com:signal:reset:1.0 rst_n RST" *)
    input  wire        rst_n,

    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S0_AXIS TDATA" *)
    input  wire [23:0] s0_axis_tdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S0_AXIS TVALID" *)
    input  wire        s0_axis_tvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S0_AXIS TREADY" *)
    output wire        s0_axis_tready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S0_AXIS TUSER" *)
    input  wire        s0_axis_tuser,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S0_AXIS TLAST" *)
    input  wire        s0_axis_tlast,

    // Kept for block-design compatibility.  The DDR reference stream is no
    // longer used; accepting it prevents an accidentally started MM2S channel
    // from applying backpressure.
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S1_AXIS TDATA" *)
    input  wire [23:0] s1_axis_tdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S1_AXIS TVALID" *)
    input  wire        s1_axis_tvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S1_AXIS TREADY" *)
    output wire        s1_axis_tready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S1_AXIS TUSER" *)
    input  wire        s1_axis_tuser,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S1_AXIS TLAST" *)
    input  wire        s1_axis_tlast,

    (* X_INTERFACE_PARAMETER = "TDATA_NUM_BYTES 3, TUSER_WIDTH 1, HAS_TLAST 1, HAS_TREADY 1" *)
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS TDATA" *)
    output wire [23:0] m_axis_tdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS TVALID" *)
    output wire        m_axis_tvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS TREADY" *)
    input  wire        m_axis_tready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS TUSER" *)
    output wire        m_axis_tuser,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS TLAST" *)
    output wire        m_axis_tlast
);

localparam integer IMG_WIDTH           = 640;
localparam integer IMG_HEIGHT          = 480;
localparam integer SAMPLE_SHIFT        = 3;   // 8x8 blocks
localparam integer GRID_WIDTH          = 80;
localparam integer GRID_HEIGHT         = 60;
localparam integer GRID_SIZE           = 4800;
localparam [7:0]  DIFF_THRESHOLD       = 8'd40;
localparam [12:0] MIN_MOTION_SAMPLES   = 13'd2;
localparam [12:0] MAX_MOTION_SAMPLES   = 13'd1000;
localparam [23:0] BORDER_COLOR         = 24'hff_ff_ff;

reg [9:0] x_count;
reg [9:0] y_count;
reg       history_valid;

// Asynchronous reads keep the AXI stream as a zero-latency pass-through.
// Only 4.8 KiB is required for the previous-frame luma samples.
(* ram_style = "distributed" *) reg [7:0] previous_luma [0:GRID_SIZE-1];
(* ram_style = "distributed" *) reg       difference_map [0:GRID_SIZE-1];

reg [9:0] box_left_work;
reg [9:0] box_right_work;
reg [9:0] box_top_work;
reg [9:0] box_bottom_work;
reg       box_work_valid;
reg [12:0] motion_sample_count;

reg [9:0] box_left_display;
reg [9:0] box_right_display;
reg [9:0] box_top_display;
reg [9:0] box_bottom_display;
reg       box_display_valid;

wire pixel_transfer = s0_axis_tvalid && s0_axis_tready;
wire [6:0] grid_x = x_count[9:SAMPLE_SHIFT];
wire [5:0] grid_y = y_count[9:SAMPLE_SHIFT];
wire [12:0] sample_address = (grid_y * GRID_WIDTH) + grid_x;
wire sample_position = (x_count[2:0] == 3'd4) &&
                       (y_count[2:0] == 3'd4);
wire sample_inside_roi = (grid_x > 0) && (grid_x < GRID_WIDTH-1) &&
                         (grid_y > 0) && (grid_y < GRID_HEIGHT-1);

// 0.25R + 0.5G + 0.25B: inexpensive luma approximation for motion only.
wire [7:0] current_luma = {2'b00, s0_axis_tdata[23:18]} +
                          {1'b0,  s0_axis_tdata[15:9]}  +
                          {2'b00, s0_axis_tdata[7:2]};
wire [7:0] stored_luma = previous_luma[sample_address];
wire [7:0] luma_difference = (current_luma >= stored_luma) ?
                             (current_luma - stored_luma) :
                             (stored_luma - current_luma);
wire raw_motion = history_valid && (luma_difference > DIFF_THRESHOLD);

// A sample must touch another changed sample on its left or above.  This
// rejects isolated sensor noise without needing a full morphology pipeline.
wire left_motion = (grid_x != 0) ? difference_map[sample_address-1'b1] : 1'b0;
wire above_motion = (grid_y != 0) ?
                    difference_map[sample_address-GRID_WIDTH] : 1'b0;
wire confirmed_motion = raw_motion && (left_motion || above_motion);

wire [9:0] block_left = {x_count[9:3], 3'b000};
wire [9:0] block_top  = {y_count[9:3], 3'b000};
wire [9:0] candidate_left = (block_left >= 10'd8) ?
                            (block_left - 10'd8) : 10'd0;
wire [9:0] candidate_top = (block_top >= 10'd8) ?
                           (block_top - 10'd8) : 10'd0;
wire [9:0] candidate_right = (block_left + 10'd15 < IMG_WIDTH) ?
                             (block_left + 10'd15) : IMG_WIDTH-1;
wire [9:0] candidate_bottom = (block_top + 10'd15 < IMG_HEIGHT) ?
                              (block_top + 10'd15) : IMG_HEIGHT-1;

wire on_horizontal_border =
    (x_count >= box_left_display) && (x_count <= box_right_display) &&
    ((y_count == box_top_display) ||
     (y_count == box_top_display + 1'b1) ||
     (y_count == box_bottom_display) ||
     (y_count + 1'b1 == box_bottom_display));
wire on_vertical_border =
    (y_count >= box_top_display) && (y_count <= box_bottom_display) &&
    ((x_count == box_left_display) ||
     (x_count == box_left_display + 1'b1) ||
     (x_count == box_right_display) ||
     (x_count + 1'b1 == box_right_display));
wire draw_border = box_display_valid &&
                   (on_horizontal_border || on_vertical_border);

assign s0_axis_tready = m_axis_tready;
assign s1_axis_tready = 1'b1;
assign m_axis_tvalid  = s0_axis_tvalid;
assign m_axis_tuser   = s0_axis_tuser;
assign m_axis_tlast   = s0_axis_tlast;
assign m_axis_tdata   = draw_border ? BORDER_COLOR : s0_axis_tdata;

always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        x_count             <= 10'd0;
        y_count             <= 10'd0;
        history_valid       <= 1'b0;
        box_left_work       <= IMG_WIDTH-1;
        box_right_work      <= 10'd0;
        box_top_work        <= IMG_HEIGHT-1;
        box_bottom_work     <= 10'd0;
        box_work_valid      <= 1'b0;
        motion_sample_count <= 13'd0;
        box_left_display    <= 10'd0;
        box_right_display   <= 10'd0;
        box_top_display     <= 10'd0;
        box_bottom_display  <= 10'd0;
        box_display_valid   <= 1'b0;
    end else if (pixel_transfer) begin
        // TUSER marks the first accepted pixel of a frame.  Detection samples
        // begin four pixels later, so the working state is reset first.
        if (s0_axis_tuser) begin
            x_count             <= 10'd1;
            y_count             <= 10'd0;
            box_left_work       <= IMG_WIDTH-1;
            box_right_work      <= 10'd0;
            box_top_work        <= IMG_HEIGHT-1;
            box_bottom_work     <= 10'd0;
            box_work_valid      <= 1'b0;
            motion_sample_count <= 13'd0;
        end else begin
            if (s0_axis_tlast) begin
                x_count <= 10'd0;
                y_count <= y_count + 1'b1;
            end else begin
                x_count <= x_count + 1'b1;
            end

            if (sample_position && sample_inside_roi) begin
                previous_luma[sample_address] <= current_luma;
                difference_map[sample_address] <= raw_motion;

                if (confirmed_motion) begin
                    motion_sample_count <= motion_sample_count + 1'b1;
                    box_work_valid <= 1'b1;
                    if (!box_work_valid || candidate_left < box_left_work)
                        box_left_work <= candidate_left;
                    if (!box_work_valid || candidate_right > box_right_work)
                        box_right_work <= candidate_right;
                    if (!box_work_valid || candidate_top < box_top_work)
                        box_top_work <= candidate_top;
                    if (!box_work_valid || candidate_bottom > box_bottom_work)
                        box_bottom_work <= candidate_bottom;
                end
            end

            // All 80x60 samples have already been processed before the last
            // pixel, so it is safe to publish the finished box here.
            if (s0_axis_tlast && (y_count == IMG_HEIGHT-1)) begin
                history_valid <= 1'b1;
                if (box_work_valid &&
                    (motion_sample_count >= MIN_MOTION_SAMPLES) &&
                    (motion_sample_count <= MAX_MOTION_SAMPLES)) begin
                    box_left_display   <= box_left_work;
                    box_right_display  <= box_right_work;
                    box_top_display    <= box_top_work;
                    box_bottom_display <= box_bottom_work;
                    box_display_valid  <= 1'b1;
                end else begin
                    box_display_valid <= 1'b0;
                end
            end
        end
    end
end

// Prevent unused-input warnings from obscuring meaningful build messages.
wire _unused_s1 = &{1'b0, s1_axis_tdata, s1_axis_tvalid,
                    s1_axis_tuser, s1_axis_tlast};

endmodule
