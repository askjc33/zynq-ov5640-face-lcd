`timescale 1ns / 1ps

// AXI4-Stream RGB888 to NT35510 Intel-8080 RGB565 frame writer.
//
// This module intentionally does not contain the NT35510 power/gamma
// initialization.  The board-level wrapper first runs the already proven
// color-bar initializer, then enables this writer.
//
// Display policy used by the stage-2 experiment:
//   * clear all 800 x 480 GRAM pixels to black once after initialization;
//   * place each 640 x 480 AXI frame at X=80..719, Y=0..479;
//   * wait for AXI TUSER (start-of-frame) before every LCD frame;
//   * use AXI TLAST as an observable line-boundary consistency check.
//
// Back-pressure is legal here: VDMA1 is a memory-to-stream source and holds
// TVALID/TDATA stable while TREADY is low.  In particular, the first pixel of
// a frame is held while the LCD address window and RAM-write command are sent.
module axis_lcd_frame_writer #(
    parameter integer INPUT_WIDTH   = 640,
    parameter integer INPUT_HEIGHT  = 480,
    parameter integer LCD_WIDTH     = 800,
    parameter integer LCD_HEIGHT    = 480,
    parameter integer X_OFFSET      = 80,
    parameter integer Y_OFFSET      = 0
) (
    input  wire        clk,
    input  wire        rst_n,
    input  wire        enable,

    input  wire [23:0] s_axis_tdata,
    input  wire        s_axis_tvalid,
    output wire        s_axis_tready,
    input  wire        s_axis_tuser,
    input  wire        s_axis_tlast,

    output wire [15:0] lcd_data,
    output wire        lcd_cs_n,
    output wire        lcd_rs,
    output wire        lcd_wr_n,

    // These registers are also useful ILA probe points in later experiments.
    output reg         init_clear_done,
    output reg         axis_protocol_error,
    output reg  [31:0] displayed_frame_count
);

localparam integer CLEAR_PIXELS = LCD_WIDTH * LCD_HEIGHT;
localparam integer VIDEO_PIXELS = INPUT_WIDTH * INPUT_HEIGHT;
localparam integer VIDEO_X_END  = X_OFFSET + INPUT_WIDTH - 1;
localparam integer VIDEO_Y_END  = Y_OFFSET + INPUT_HEIGHT - 1;

localparam [3:0] ST_DISABLED     = 4'd0;
localparam [3:0] ST_CLEAR_WINDOW = 4'd1;
localparam [3:0] ST_CLEAR_CMD    = 4'd2;
localparam [3:0] ST_CLEAR_PIXELS = 4'd3;
localparam [3:0] ST_WAIT_SOF     = 4'd4;
localparam [3:0] ST_VIDEO_WINDOW = 4'd5;
localparam [3:0] ST_VIDEO_CMD    = 4'd6;
localparam [3:0] ST_VIDEO_PIXELS = 4'd7;

reg [3:0]  state;
reg [3:0]  window_index;
reg        register_phase;
reg [18:0] clear_pixel_count;
reg [18:0] video_pixel_count;
reg [9:0]  video_x;

reg        tx_valid;
wire       tx_ready;
reg        tx_rs;
reg [15:0] tx_data;
wire       tx_done;
reg        tx_waiting;

reg        accepted_tuser;
reg        accepted_tlast;

wire [15:0] rgb565 = {
    s_axis_tdata[23:19], // R[7:3]
    s_axis_tdata[15:10], // G[7:2]
    s_axis_tdata[7:3]    // B[7:3]
};

// While waiting for SOF, non-SOF words are consumed and discarded so a
// malformed/restarted stream can re-synchronize.  A SOF word itself is held
// until the LCD window header is complete.
assign s_axis_tready =
    ((state == ST_WAIT_SOF) && s_axis_tvalid && !s_axis_tuser) ? 1'b1 :
    ((state == ST_VIDEO_PIXELS) && !tx_waiting && tx_ready)     ? 1'b1 :
                                                                  1'b0;

// Preserve stage-1 physical setup/hold/WR widths on the 100 MHz fabric.
lcd8080_write_cycle #(.CYCLE_DIV(2)) u_write_cycle (
    .clk       (clk),
    .rst_n     (rst_n),
    .tx_valid  (tx_valid),
    .tx_ready  (tx_ready),
    .tx_rs     (tx_rs),
    .tx_data   (tx_data),
    .tx_done   (tx_done),
    .lcd_data  (lcd_data),
    .lcd_cs_n  (lcd_cs_n),
    .lcd_rs    (lcd_rs),
    .lcd_wr_n  (lcd_wr_n)
);

// The NT35510 accepts a 16-bit register address when RS=0 and an 8-bit value
// in the low byte of a 16-bit transfer when RS=1.
function [15:0] window_reg;
    input [3:0] index;
    begin
        case (index)
            4'd0: window_reg = 16'h2A00;
            4'd1: window_reg = 16'h2A01;
            4'd2: window_reg = 16'h2A02;
            4'd3: window_reg = 16'h2A03;
            4'd4: window_reg = 16'h2B00;
            4'd5: window_reg = 16'h2B01;
            4'd6: window_reg = 16'h2B02;
            4'd7: window_reg = 16'h2B03;
            default: window_reg = 16'h0000;
        endcase
    end
endfunction

function [15:0] clear_window_data;
    input [3:0] index;
    begin
        case (index)
            4'd0: clear_window_data = 16'h0000; // X start high
            4'd1: clear_window_data = 16'h0000; // X start low
            4'd2: clear_window_data = (LCD_WIDTH - 1) >> 8;
            4'd3: clear_window_data = (LCD_WIDTH - 1) & 16'h00ff;
            4'd4: clear_window_data = 16'h0000; // Y start high
            4'd5: clear_window_data = 16'h0000; // Y start low
            4'd6: clear_window_data = (LCD_HEIGHT - 1) >> 8;
            4'd7: clear_window_data = (LCD_HEIGHT - 1) & 16'h00ff;
            default: clear_window_data = 16'h0000;
        endcase
    end
endfunction

function [15:0] video_window_data;
    input [3:0] index;
    begin
        case (index)
            4'd0: video_window_data = X_OFFSET >> 8;
            4'd1: video_window_data = X_OFFSET & 16'h00ff;
            4'd2: video_window_data = VIDEO_X_END >> 8;
            4'd3: video_window_data = VIDEO_X_END & 16'h00ff;
            4'd4: video_window_data = Y_OFFSET >> 8;
            4'd5: video_window_data = Y_OFFSET & 16'h00ff;
            4'd6: video_window_data = VIDEO_Y_END >> 8;
            4'd7: video_window_data = VIDEO_Y_END & 16'h00ff;
            default: video_window_data = 16'h0000;
        endcase
    end
endfunction

always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        state                 <= ST_DISABLED;
        window_index          <= 4'd0;
        register_phase        <= 1'b0;
        clear_pixel_count     <= 19'd0;
        video_pixel_count     <= 19'd0;
        video_x               <= 10'd0;
        tx_valid              <= 1'b0;
        tx_rs                 <= 1'b0;
        tx_data               <= 16'h0000;
        tx_waiting            <= 1'b0;
        accepted_tuser        <= 1'b0;
        accepted_tlast        <= 1'b0;
        init_clear_done       <= 1'b0;
        axis_protocol_error   <= 1'b0;
        displayed_frame_count <= 32'd0;
    end else begin
        tx_valid <= 1'b0;

        if (!enable) begin
            state                 <= ST_DISABLED;
            window_index          <= 4'd0;
            register_phase        <= 1'b0;
            clear_pixel_count     <= 19'd0;
            video_pixel_count     <= 19'd0;
            video_x               <= 10'd0;
            tx_waiting            <= 1'b0;
            init_clear_done       <= 1'b0;
            axis_protocol_error   <= 1'b0;
            displayed_frame_count <= 32'd0;
        end else begin
            case (state)
                ST_DISABLED: begin
                    window_index      <= 4'd0;
                    register_phase   <= 1'b0;
                    clear_pixel_count <= 19'd0;
                    tx_waiting       <= 1'b0;
                    state            <= ST_CLEAR_WINDOW;
                end

                ST_CLEAR_WINDOW,
                ST_VIDEO_WINDOW: begin
                    if (!tx_waiting && tx_ready) begin
                        tx_valid   <= 1'b1;
                        tx_rs      <= register_phase;
                        tx_data    <= register_phase ?
                            ((state == ST_CLEAR_WINDOW) ?
                                clear_window_data(window_index) :
                                video_window_data(window_index)) :
                            window_reg(window_index);
                        tx_waiting <= 1'b1;
                    end

                    if (tx_waiting && tx_done) begin
                        tx_waiting <= 1'b0;
                        if (!register_phase) begin
                            register_phase <= 1'b1;
                        end else begin
                            register_phase <= 1'b0;
                            if (window_index == 4'd7) begin
                                window_index <= 4'd0;
                                state <= (state == ST_CLEAR_WINDOW) ?
                                    ST_CLEAR_CMD : ST_VIDEO_CMD;
                            end else begin
                                window_index <= window_index + 1'b1;
                            end
                        end
                    end
                end

                ST_CLEAR_CMD,
                ST_VIDEO_CMD: begin
                    if (!tx_waiting && tx_ready) begin
                        tx_valid   <= 1'b1;
                        tx_rs      <= 1'b0;
                        tx_data    <= 16'h2C00; // memory write
                        tx_waiting <= 1'b1;
                    end
                    if (tx_waiting && tx_done) begin
                        tx_waiting <= 1'b0;
                        if (state == ST_CLEAR_CMD) begin
                            clear_pixel_count <= 19'd0;
                            state <= ST_CLEAR_PIXELS;
                        end else begin
                            video_pixel_count <= 19'd0;
                            video_x <= 10'd0;
                            state <= ST_VIDEO_PIXELS;
                        end
                    end
                end

                ST_CLEAR_PIXELS: begin
                    if (!tx_waiting && tx_ready) begin
                        tx_valid   <= 1'b1;
                        tx_rs      <= 1'b1;
                        tx_data    <= 16'h0000;
                        tx_waiting <= 1'b1;
                    end
                    if (tx_waiting && tx_done) begin
                        tx_waiting <= 1'b0;
                        if (clear_pixel_count == CLEAR_PIXELS - 1) begin
                            init_clear_done <= 1'b1;
                            state <= ST_WAIT_SOF;
                        end else begin
                            clear_pixel_count <= clear_pixel_count + 1'b1;
                        end
                    end
                end

                ST_WAIT_SOF: begin
                    // Do not acknowledge the SOF pixel yet.  AXI requires the
                    // source to hold it while we send the LCD frame header.
                    if (s_axis_tvalid && s_axis_tuser) begin
                        window_index    <= 4'd0;
                        register_phase <= 1'b0;
                        state          <= ST_VIDEO_WINDOW;
                    end
                end

                ST_VIDEO_PIXELS: begin
                    if (s_axis_tvalid && s_axis_tready) begin
                        tx_valid        <= 1'b1;
                        tx_rs           <= 1'b1;
                        tx_data         <= rgb565;
                        tx_waiting      <= 1'b1;
                        accepted_tuser  <= s_axis_tuser;
                        accepted_tlast  <= s_axis_tlast;
                    end

                    if (tx_waiting && tx_done) begin
                        tx_waiting <= 1'b0;

                        if ((video_x == INPUT_WIDTH - 1) != accepted_tlast)
                            axis_protocol_error <= 1'b1;
                        if ((video_pixel_count != 0) && accepted_tuser)
                            axis_protocol_error <= 1'b1;

                        if (video_x == INPUT_WIDTH - 1)
                            video_x <= 10'd0;
                        else
                            video_x <= video_x + 1'b1;

                        if (video_pixel_count == VIDEO_PIXELS - 1) begin
                            video_pixel_count <= 19'd0;
                            displayed_frame_count <= displayed_frame_count + 1'b1;
                            state <= ST_WAIT_SOF;
                        end else begin
                            video_pixel_count <= video_pixel_count + 1'b1;
                        end
                    end
                end

                default: state <= ST_DISABLED;
            endcase
        end
    end
end

endmodule
