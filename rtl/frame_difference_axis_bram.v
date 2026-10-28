`timescale 1ns / 1ps

// Source-rebuildable motion detector using synchronous block RAM.
//
// One luma sample is kept for every 2^SAMPLE_SHIFT square block.  For the
// default 640x480/8x8 configuration this is only 80x60 = 4800 bytes.  The
// sample read is deliberately synchronous so Vivado infers BRAM instead of a
// 4800-to-1 asynchronous LUT mux.  Detection is completed one clock after a
// sample is captured; samples are 8 pixels apart, so this does not stall or
// delay the pass-through video stream.
module frame_difference_axis_bram #(
    parameter integer IMG_WIDTH    = 640,
    parameter integer IMG_HEIGHT   = 480,
    parameter integer SAMPLE_SHIFT = 3
) (
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

    // Retained only for block-design compatibility with the historical path.
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

localparam integer GRID_WIDTH  = IMG_WIDTH  >> SAMPLE_SHIFT;
localparam integer GRID_HEIGHT = IMG_HEIGHT >> SAMPLE_SHIFT;
localparam integer GRID_SIZE   = GRID_WIDTH * GRID_HEIGHT;
localparam integer SAMPLE_CENTER = 1 << (SAMPLE_SHIFT - 1);

localparam [7:0]  DIFF_THRESHOLD     = 8'd40;
localparam [12:0] MIN_MOTION_SAMPLES = 13'd2;
localparam [12:0] MAX_MOTION_SAMPLES = 13'd1000;
localparam [23:0] BORDER_COLOR       = 24'hff_ff_ff;

reg [9:0] x_count;
reg [9:0] y_count;
reg       history_valid;

// A same-address read and write in this clocked block is READ_FIRST: the
// registered output receives the preceding frame's sample while the BRAM is
// updated with the current frame's sample.
(* ram_style = "block" *) reg [7:0] previous_luma [0:GRID_SIZE-1];
reg [7:0] stored_luma_d;
reg [7:0] current_luma_d;
reg [6:0] grid_x_d;
reg [5:0] grid_y_d;
reg       sample_inside_roi_d;
reg       sample_pending;

// Only one previous/current row of binary motion samples is required for the
// left-or-above noise filter.  This replaces the old 4800-bit asynchronous
// difference_map and its large mux network.
reg [GRID_WIDTH-1:0] previous_row_motion;
reg [GRID_WIDTH-1:0] current_row_motion;
reg                  left_raw_motion;

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
wire [6:0] grid_x = x_count >> SAMPLE_SHIFT;
wire [5:0] grid_y = y_count >> SAMPLE_SHIFT;
wire [12:0] sample_address = (grid_y * GRID_WIDTH) + grid_x;
wire sample_position =
    (x_count[SAMPLE_SHIFT-1:0] == SAMPLE_CENTER) &&
    (y_count[SAMPLE_SHIFT-1:0] == SAMPLE_CENTER);
wire sample_inside_roi = (grid_x > 0) && (grid_x < GRID_WIDTH-1) &&
                         (grid_y > 0) && (grid_y < GRID_HEIGHT-1);

// 0.25R + 0.5G + 0.25B, matching the original detector.
wire [7:0] current_luma = {2'b00, s0_axis_tdata[23:18]} +
                          {1'b0,  s0_axis_tdata[15:9]}  +
                          {2'b00, s0_axis_tdata[7:2]};
wire [7:0] delayed_difference =
    (current_luma_d >= stored_luma_d) ?
        (current_luma_d - stored_luma_d) :
        (stored_luma_d - current_luma_d);
wire delayed_raw_motion = history_valid &&
                          (delayed_difference > DIFF_THRESHOLD);
wire delayed_above_motion = previous_row_motion[grid_x_d];
wire delayed_confirmed_motion = delayed_raw_motion &&
    (left_raw_motion || delayed_above_motion);

wire [9:0] delayed_block_left = grid_x_d << SAMPLE_SHIFT;
wire [9:0] delayed_block_top  = grid_y_d << SAMPLE_SHIFT;
wire [9:0] delayed_candidate_left =
    (delayed_block_left >= (1 << SAMPLE_SHIFT)) ?
        delayed_block_left - (1 << SAMPLE_SHIFT) : 0;
wire [9:0] delayed_candidate_top =
    (delayed_block_top >= (1 << SAMPLE_SHIFT)) ?
        delayed_block_top - (1 << SAMPLE_SHIFT) : 0;
wire [9:0] delayed_candidate_right =
    (delayed_block_left + ((2 << SAMPLE_SHIFT) - 1) < IMG_WIDTH) ?
        delayed_block_left + ((2 << SAMPLE_SHIFT) - 1) : IMG_WIDTH-1;
wire [9:0] delayed_candidate_bottom =
    (delayed_block_top + ((2 << SAMPLE_SHIFT) - 1) < IMG_HEIGHT) ?
        delayed_block_top + ((2 << SAMPLE_SHIFT) - 1) : IMG_HEIGHT-1;

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

// BRAM has no asynchronous reset. Keep the memory process separate from
// resettable control; history_valid suppresses the uninitialized first read.
always @(posedge clk) begin
    if (rst_n && pixel_transfer && !s0_axis_tuser && sample_position) begin
        stored_luma_d <= previous_luma[sample_address];
        previous_luma[sample_address] <= current_luma;
    end
end

assign s0_axis_tready = m_axis_tready;
assign s1_axis_tready = 1'b1;
assign m_axis_tvalid  = s0_axis_tvalid;
assign m_axis_tuser   = s0_axis_tuser;
assign m_axis_tlast   = s0_axis_tlast;
assign m_axis_tdata   = draw_border ? BORDER_COLOR : s0_axis_tdata;

always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        x_count              <= 10'd0;
        y_count              <= 10'd0;
        history_valid        <= 1'b0;
        current_luma_d       <= 8'd0;
        grid_x_d             <= 7'd0;
        grid_y_d             <= 6'd0;
        sample_inside_roi_d  <= 1'b0;
        sample_pending       <= 1'b0;
        previous_row_motion  <= {GRID_WIDTH{1'b0}};
        current_row_motion   <= {GRID_WIDTH{1'b0}};
        left_raw_motion      <= 1'b0;
        box_left_work        <= IMG_WIDTH-1;
        box_right_work       <= 10'd0;
        box_top_work         <= IMG_HEIGHT-1;
        box_bottom_work      <= 10'd0;
        box_work_valid       <= 1'b0;
        motion_sample_count  <= 13'd0;
        box_left_display     <= 10'd0;
        box_right_display    <= 10'd0;
        box_top_display      <= 10'd0;
        box_bottom_display   <= 10'd0;
        box_display_valid    <= 1'b0;
    end else begin
        sample_pending <= 1'b0;

        // Process the registered BRAM result independently of video output.
        if (sample_pending) begin
            if (grid_x_d == GRID_WIDTH-1) begin
                previous_row_motion <= {
                    delayed_raw_motion,
                    current_row_motion[GRID_WIDTH-2:0]
                };
                current_row_motion <= {GRID_WIDTH{1'b0}};
                left_raw_motion <= 1'b0;
            end else begin
                current_row_motion[grid_x_d] <= delayed_raw_motion;
                left_raw_motion <= delayed_raw_motion;
            end

            if (sample_inside_roi_d && delayed_confirmed_motion) begin
                motion_sample_count <= motion_sample_count + 1'b1;
                box_work_valid <= 1'b1;
                if (!box_work_valid || delayed_candidate_left < box_left_work)
                    box_left_work <= delayed_candidate_left;
                if (!box_work_valid || delayed_candidate_right > box_right_work)
                    box_right_work <= delayed_candidate_right;
                if (!box_work_valid || delayed_candidate_top < box_top_work)
                    box_top_work <= delayed_candidate_top;
                if (!box_work_valid || delayed_candidate_bottom > box_bottom_work)
                    box_bottom_work <= delayed_candidate_bottom;
            end
        end

        if (pixel_transfer) begin
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
                    y_count <= (y_count == IMG_HEIGHT-1) ? 10'd0 : y_count + 1'b1;
                end else begin
                    x_count <= x_count + 1'b1;
                end

                if (sample_position) begin
                    current_luma_d      <= current_luma;
                    grid_x_d            <= grid_x;
                    grid_y_d            <= grid_y;
                    sample_inside_roi_d <= sample_inside_roi;
                    sample_pending      <= 1'b1;
                end

                // The final sampled block was processed well before the final
                // active pixel, so the work box is complete at frame end.
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
end

wire _unused_s1 = &{1'b0, s1_axis_tdata, s1_axis_tvalid,
                    s1_axis_tuser, s1_axis_tlast};

endmodule
