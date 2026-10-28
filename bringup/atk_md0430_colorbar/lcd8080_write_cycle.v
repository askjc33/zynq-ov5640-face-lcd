`timescale 1ns / 1ps

// One Intel-8080 write transaction.
//
// The upper layer supplies tx_rs and tx_data for one transfer.  This module
// deliberately uses a conservative four-clock bus cycle at the 50 MHz board
// clock so that flying-wire bring-up is easier:
//
//   SETUP  : data/RS/CS become valid while WR remains high
//   WR_LOW : WR is asserted for two complete clock periods
//   RELEASE: WR returns high; the LCD samples the bus on that edge
//   IDLE   : CS returns high and tx_done pulses
//
// At 50 MHz this gives about 20 ns setup and 40 ns WR-low time.  The cycle can
// be shortened later, after the hardware is stable and the datasheet timing
// has been checked with an oscilloscope/ILA.
module lcd8080_write_cycle #(
    // 1 at the stage-1 50 MHz clock; 2 at the stage-2 100 MHz clock.
    parameter integer CYCLE_DIV = 1
) (
    input  wire        clk,
    input  wire        rst_n,

    input  wire        tx_valid,
    output wire        tx_ready,
    input  wire        tx_rs,       // 0: command/register address, 1: data
    input  wire [15:0] tx_data,
    output reg         tx_done,

    output reg  [15:0] lcd_data,
    output reg         lcd_cs_n,
    output reg         lcd_rs,
    output reg         lcd_wr_n
);

localparam [2:0] ST_IDLE    = 3'd0;
localparam [2:0] ST_SETUP   = 3'd1;
localparam [2:0] ST_WR_LOW1 = 3'd2;
localparam [2:0] ST_WR_LOW2 = 3'd3;
localparam [2:0] ST_RELEASE = 3'd4;

reg [2:0] state;
reg [7:0] phase_count;

assign tx_ready = (state == ST_IDLE);

always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        state    <= ST_IDLE;
        phase_count <= 0;
        tx_done  <= 1'b0;
        lcd_data <= 16'h0000;
        lcd_cs_n <= 1'b1;
        lcd_rs   <= 1'b0;
        lcd_wr_n <= 1'b1;
    end else begin
        tx_done <= 1'b0;
        if (state == ST_IDLE || phase_count == CYCLE_DIV-1) begin
        phase_count <= 0;
        case (state)
            ST_IDLE: begin
                lcd_cs_n <= 1'b1;
                lcd_wr_n <= 1'b1;
                if (tx_valid) begin
                    lcd_data <= tx_data;
                    lcd_rs   <= tx_rs;
                    lcd_cs_n <= 1'b0;
                    state    <= ST_SETUP;
                end
            end

            ST_SETUP: begin
                lcd_wr_n <= 1'b0;
                state    <= ST_WR_LOW1;
            end

            ST_WR_LOW1: begin
                lcd_wr_n <= 1'b0;
                state    <= ST_WR_LOW2;
            end

            ST_WR_LOW2: begin
                lcd_wr_n <= 1'b1;
                state    <= ST_RELEASE;
            end

            ST_RELEASE: begin
                lcd_cs_n <= 1'b1;
                tx_done  <= 1'b1;
                state    <= ST_IDLE;
            end

            default: state <= ST_IDLE;
        endcase
        end else begin
            phase_count <= phase_count + 1'b1;
        end
    end
end

endmodule
