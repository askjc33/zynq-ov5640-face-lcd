`timescale 1ns / 1ps

// STUDYBOARD + ATK-MD0430 V2.0 first-stage bring-up design.
//
// This design has no PS, DDR, VDMA, camera, or HDMI dependency.  It performs:
//   1. hardware reset of the NT35510;
//   2. controller power/gamma/display initialization;
//   3. an 800 x 480 landscape address-window setup;
//   4. one complete RGB565 color-bar write into the LCD's internal GRAM.
//
// The picture remains visible after the single write because the NT35510 owns
// the display RAM and continuously refreshes the glass.  A later project stage
// will replace the color generator with the live video stream.
module atk_md0430_colorbar_top #(
    parameter integer CLK_HZ     = 50_000_000,
    parameter integer H_RES      = 800,
    parameter integer V_RES      = 480,
    parameter integer SIMULATION = 0,
    parameter integer WRITE_CLK_DIV = 1
) (
    input  wire        sys_clk,
    input  wire        sys_rst_n,

    output wire [15:0] lcd_data,
    output wire        lcd_cs_n,
    output wire        lcd_rs,
    output wire        lcd_wr_n,
    output wire        lcd_rd_n,
    output reg         lcd_rst_n,
    output reg         lcd_bl
);

// Conservative delays.  In simulation they collapse to a few clocks so the
// testbench does not have to wait hundreds of milliseconds.
localparam integer RESET_LOW_CYCLES = SIMULATION ? 4 : (CLK_HZ / 50);       // 20 ms
localparam integer RESET_WAIT_CYCLES = SIMULATION ? 4 : (CLK_HZ / 10);      // 100 ms
localparam integer SLEEP_OUT_CYCLES = SIMULATION ? 4 : ((CLK_HZ / 1000) * 120);
localparam integer DISPLAY_WAIT_CYCLES = SIMULATION ? 4 : (CLK_HZ / 50);    // 20 ms
localparam integer PIXEL_COUNT = H_RES * V_RES;
localparam integer BAR_WIDTH   = H_RES / 8;

localparam [3:0] ST_RESET_LOW   = 4'd0;
localparam [3:0] ST_RESET_WAIT  = 4'd1;
localparam [3:0] ST_PAGE1       = 4'd2;
localparam [3:0] ST_GAMMA       = 4'd3;
localparam [3:0] ST_PAGE0       = 4'd4;
localparam [3:0] ST_SLEEP_OUT   = 4'd5;
localparam [3:0] ST_SLEEP_WAIT  = 4'd6;
localparam [3:0] ST_DISPLAY_ON  = 4'd7;
localparam [3:0] ST_DISPLAY_WAIT = 4'd8;
localparam [3:0] ST_WINDOW      = 4'd9;
localparam [3:0] ST_RAM_CMD     = 4'd10;
localparam [3:0] ST_PIXELS      = 4'd11;
localparam [3:0] ST_DONE        = 4'd12;

reg [3:0]  state;
reg [31:0] delay_count;
reg [8:0]  table_index;
reg        register_phase; // 0 sends register address, 1 sends register data
reg [2:0]  gamma_bank;     // D1..D6: positive/negative R/G/B gamma tables
reg [5:0]  gamma_index;    // 0..51 bytes per gamma table
reg [9:0]  pixel_x;
reg [8:0]  pixel_y;
reg [18:0] pixel_count;
reg        frame_done;

reg        tx_valid;
wire       tx_ready;
reg        tx_rs;
reg [15:0] tx_data;
wire       tx_done;
reg        tx_waiting;

assign lcd_rd_n = 1'b1; // first-stage design is write-only

lcd8080_write_cycle #(.CYCLE_DIV(WRITE_CLK_DIV)) u_write_cycle (
    .clk       (sys_clk),
    .rst_n     (sys_rst_n),
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

// Page-1 power settings from the widely used ALIENTEK NT35510 sequence.
// Gamma data is generated separately because the same 52-byte curve is sent
// to six consecutive register banks (D1 through D6).
function [15:0] page1_reg;
    input [5:0] index;
    begin
        case (index)
             0: page1_reg = 16'hF000;  1: page1_reg = 16'hF001;
             2: page1_reg = 16'hF002;  3: page1_reg = 16'hF003;
             4: page1_reg = 16'hF004;
             5: page1_reg = 16'hB000;  6: page1_reg = 16'hB001;
             7: page1_reg = 16'hB002;
             8: page1_reg = 16'hB600;  9: page1_reg = 16'hB601;
            10: page1_reg = 16'hB602;
            11: page1_reg = 16'hB100; 12: page1_reg = 16'hB101;
            13: page1_reg = 16'hB102;
            14: page1_reg = 16'hB700; 15: page1_reg = 16'hB701;
            16: page1_reg = 16'hB702;
            17: page1_reg = 16'hB200; 18: page1_reg = 16'hB201;
            19: page1_reg = 16'hB202;
            20: page1_reg = 16'hB800; 21: page1_reg = 16'hB801;
            22: page1_reg = 16'hB802;
            23: page1_reg = 16'hBF00;
            24: page1_reg = 16'hB300; 25: page1_reg = 16'hB301;
            26: page1_reg = 16'hB302;
            27: page1_reg = 16'hB900; 28: page1_reg = 16'hB901;
            29: page1_reg = 16'hB902;
            30: page1_reg = 16'hB500; 31: page1_reg = 16'hB501;
            32: page1_reg = 16'hB502;
            33: page1_reg = 16'hC200;
            34: page1_reg = 16'hBA00; 35: page1_reg = 16'hBA01;
            36: page1_reg = 16'hBA02;
            37: page1_reg = 16'hBC00; 38: page1_reg = 16'hBC01;
            39: page1_reg = 16'hBC02;
            40: page1_reg = 16'hBD00; 41: page1_reg = 16'hBD01;
            42: page1_reg = 16'hBD02;
            43: page1_reg = 16'hBE00; 44: page1_reg = 16'hBE01;
            default: page1_reg = 16'h0000;
        endcase
    end
endfunction

function [15:0] page1_data;
    input [5:0] index;
    begin
        case (index)
             0: page1_data = 16'h0055;  1: page1_data = 16'h00AA;
             2: page1_data = 16'h0052;  3: page1_data = 16'h0008;
             4: page1_data = 16'h0001;
             5, 6, 7: page1_data = 16'h000D;       // AVDD 5.2 V
             8, 9,10: page1_data = 16'h0034;       // AVDD ratio
            11,12,13: page1_data = 16'h000D;       // AVEE -5.2 V
            14,15,16: page1_data = 16'h0034;       // AVEE ratio
            17,18,19: page1_data = 16'h0000;       // VCL -2.5 V
            20,21,22: page1_data = 16'h0024;       // VCL ratio
            23:       page1_data = 16'h0001;       // free-pump VGH
            24,25,26: page1_data = 16'h000F;
            27,28,29: page1_data = 16'h0034;       // VGH ratio
            30,31,32: page1_data = 16'h0008;       // VGL
            33:       page1_data = 16'h0003;
            34,35,36: page1_data = 16'h0024;       // VGLX ratio
            37: page1_data = 16'h0000; 38: page1_data = 16'h0078;
            39: page1_data = 16'h0000;
            40: page1_data = 16'h0000; 41: page1_data = 16'h0078;
            42: page1_data = 16'h0000;
            43: page1_data = 16'h0000; 44: page1_data = 16'h0064;
            default: page1_data = 16'h0000;
        endcase
    end
endfunction

function [7:0] gamma_data;
    input [5:0] index;
    begin
        case (index)
             0: gamma_data=8'h00;  1: gamma_data=8'h33;
             2: gamma_data=8'h00;  3: gamma_data=8'h34;
             4: gamma_data=8'h00;  5: gamma_data=8'h3A;
             6: gamma_data=8'h00;  7: gamma_data=8'h4A;
             8: gamma_data=8'h00;  9: gamma_data=8'h5C;
            10: gamma_data=8'h00; 11: gamma_data=8'h81;
            12: gamma_data=8'h00; 13: gamma_data=8'hA6;
            14: gamma_data=8'h00; 15: gamma_data=8'hE5;
            16: gamma_data=8'h01; 17: gamma_data=8'h13;
            18: gamma_data=8'h01; 19: gamma_data=8'h54;
            20: gamma_data=8'h01; 21: gamma_data=8'h82;
            22: gamma_data=8'h01; 23: gamma_data=8'hCA;
            24: gamma_data=8'h02; 25: gamma_data=8'h00;
            26: gamma_data=8'h02; 27: gamma_data=8'h01;
            28: gamma_data=8'h02; 29: gamma_data=8'h34;
            30: gamma_data=8'h02; 31: gamma_data=8'h67;
            32: gamma_data=8'h02; 33: gamma_data=8'h84;
            34: gamma_data=8'h02; 35: gamma_data=8'hA4;
            36: gamma_data=8'h02; 37: gamma_data=8'hB7;
            38: gamma_data=8'h02; 39: gamma_data=8'hCF;
            40: gamma_data=8'h02; 41: gamma_data=8'hDE;
            42: gamma_data=8'h02; 43: gamma_data=8'hF2;
            44: gamma_data=8'h02; 45: gamma_data=8'hFE;
            46: gamma_data=8'h03; 47: gamma_data=8'h10;
            48: gamma_data=8'h03; 49: gamma_data=8'h33;
            50: gamma_data=8'h03; 51: gamma_data=8'h6D;
            default: gamma_data=8'h00;
        endcase
    end
endfunction

function [15:0] page0_reg;
    input [4:0] index;
    begin
        case (index)
             0: page0_reg=16'hF000;  1: page0_reg=16'hF001;
             2: page0_reg=16'hF002;  3: page0_reg=16'hF003;
             4: page0_reg=16'hF004;
             5: page0_reg=16'hB100;  6: page0_reg=16'hB101;
             7: page0_reg=16'hB600;
             8: page0_reg=16'hB700;  9: page0_reg=16'hB701;
            10: page0_reg=16'hB800; 11: page0_reg=16'hB801;
            12: page0_reg=16'hB802; 13: page0_reg=16'hB803;
            14: page0_reg=16'hBC00; 15: page0_reg=16'hBC01;
            16: page0_reg=16'hBC02;
            17: page0_reg=16'hC900; 18: page0_reg=16'hC901;
            19: page0_reg=16'hC902; 20: page0_reg=16'hC903;
            21: page0_reg=16'hC904;
            22: page0_reg=16'h3500; // tearing-effect output control
            23: page0_reg=16'h3A00; // RGB565 pixel format
            24: page0_reg=16'h3600; // landscape scan direction
            default: page0_reg=16'h0000;
        endcase
    end
endfunction

function [15:0] page0_data;
    input [4:0] index;
    begin
        case (index)
             0: page0_data=16'h0055;  1: page0_data=16'h00AA;
             2: page0_data=16'h0052;  3: page0_data=16'h0008;
             4: page0_data=16'h0000;
             5: page0_data=16'h00CC;  6: page0_data=16'h0000;
             7: page0_data=16'h0005;
             8: page0_data=16'h0070;  9: page0_data=16'h0070;
            10: page0_data=16'h0001; 11: page0_data=16'h0003;
            12: page0_data=16'h0003; 13: page0_data=16'h0003;
            14: page0_data=16'h0002; 15: page0_data=16'h0000;
            16: page0_data=16'h0000;
            17: page0_data=16'h00D0; 18: page0_data=16'h0002;
            19: page0_data=16'h0050; 20: page0_data=16'h0050;
            21: page0_data=16'h0050;
            22: page0_data=16'h0000;
            23: page0_data=16'h0055;
            24: page0_data=16'h00A0; // MV=1 and MY=1 => 800x480 landscape
            default: page0_data=16'h0000;
        endcase
    end
endfunction

function [15:0] window_reg;
    input [3:0] index;
    begin
        case (index)
            0: window_reg=16'h2A00; 1: window_reg=16'h2A01;
            2: window_reg=16'h2A02; 3: window_reg=16'h2A03;
            4: window_reg=16'h2B00; 5: window_reg=16'h2B01;
            6: window_reg=16'h2B02; 7: window_reg=16'h2B03;
            default: window_reg=16'h0000;
        endcase
    end
endfunction

function [15:0] window_data;
    input [3:0] index;
    begin
        case (index)
            0,1,4,5: window_data=16'h0000;
            2: window_data=(H_RES-1) >> 8;
            3: window_data=(H_RES-1) & 16'h00FF;
            6: window_data=(V_RES-1) >> 8;
            7: window_data=(V_RES-1) & 16'h00FF;
            default: window_data=16'h0000;
        endcase
    end
endfunction

function [15:0] color_bar;
    input [9:0] x;
    begin
        if      (x < BAR_WIDTH*1) color_bar = 16'hFFFF; // white
        else if (x < BAR_WIDTH*2) color_bar = 16'hFFE0; // yellow
        else if (x < BAR_WIDTH*3) color_bar = 16'h07FF; // cyan
        else if (x < BAR_WIDTH*4) color_bar = 16'h07E0; // green
        else if (x < BAR_WIDTH*5) color_bar = 16'hF81F; // magenta
        else if (x < BAR_WIDTH*6) color_bar = 16'hF800; // red
        else if (x < BAR_WIDTH*7) color_bar = 16'h001F; // blue
        else                      color_bar = 16'h0000; // black
    end
endfunction

// Helper pattern used in table states:
// register_phase=0 sends the 16-bit NT35510 register address with RS=0;
// register_phase=1 sends its value with RS=1.
always @(posedge sys_clk or negedge sys_rst_n) begin
    if (!sys_rst_n) begin
        state          <= ST_RESET_LOW;
        delay_count    <= 32'd0;
        table_index    <= 9'd0;
        register_phase <= 1'b0;
        gamma_bank     <= 3'd0;
        gamma_index    <= 6'd0;
        pixel_x        <= 10'd0;
        pixel_y        <= 9'd0;
        pixel_count    <= 19'd0;
        frame_done     <= 1'b0;
        tx_valid       <= 1'b0;
        tx_rs          <= 1'b0;
        tx_data        <= 16'h0000;
        tx_waiting     <= 1'b0;
        lcd_rst_n      <= 1'b0;
        lcd_bl         <= 1'b0;
    end else begin
        tx_valid <= 1'b0;

        case (state)
            ST_RESET_LOW: begin
                lcd_rst_n <= 1'b0;
                lcd_bl    <= 1'b0;
                if (delay_count == RESET_LOW_CYCLES-1) begin
                    delay_count <= 32'd0;
                    lcd_rst_n   <= 1'b1;
                    state       <= ST_RESET_WAIT;
                end else begin
                    delay_count <= delay_count + 1'b1;
                end
            end

            ST_RESET_WAIT: begin
                if (delay_count == RESET_WAIT_CYCLES-1) begin
                    delay_count    <= 32'd0;
                    table_index    <= 9'd0;
                    register_phase <= 1'b0;
                    state          <= ST_PAGE1;
                end else begin
                    delay_count <= delay_count + 1'b1;
                end
            end

            ST_PAGE1: begin
                if (!tx_waiting && tx_ready) begin
                    tx_valid   <= 1'b1;
                    tx_rs      <= register_phase;
                    tx_data    <= register_phase ? page1_data(table_index[5:0])
                                                 : page1_reg(table_index[5:0]);
                    tx_waiting <= 1'b1;
                end else if (tx_waiting && tx_done) begin
                    tx_waiting <= 1'b0;
                    if (!register_phase) begin
                        register_phase <= 1'b1;
                    end else begin
                        register_phase <= 1'b0;
                        if (table_index == 9'd44) begin
                            table_index <= 9'd0;
                            gamma_bank  <= 3'd0;
                            gamma_index <= 6'd0;
                            state       <= ST_GAMMA;
                        end else begin
                            table_index <= table_index + 1'b1;
                        end
                    end
                end
            end

            ST_GAMMA: begin
                if (!tx_waiting && tx_ready) begin
                    tx_valid <= 1'b1;
                    tx_rs    <= register_phase;
                    tx_data  <= register_phase
                              ? {8'h00, gamma_data(gamma_index)}
                              : {8'hD1 + gamma_bank, 2'b00, gamma_index};
                    tx_waiting <= 1'b1;
                end else if (tx_waiting && tx_done) begin
                    tx_waiting <= 1'b0;
                    if (!register_phase) begin
                        register_phase <= 1'b1;
                    end else begin
                        register_phase <= 1'b0;
                        if (gamma_index == 6'd51) begin
                            gamma_index <= 6'd0;
                            if (gamma_bank == 3'd5) begin
                                gamma_bank  <= 3'd0;
                                table_index <= 9'd0;
                                state       <= ST_PAGE0;
                            end else begin
                                gamma_bank <= gamma_bank + 1'b1;
                            end
                        end else begin
                            gamma_index <= gamma_index + 1'b1;
                        end
                    end
                end
            end

            ST_PAGE0: begin
                if (!tx_waiting && tx_ready) begin
                    tx_valid   <= 1'b1;
                    tx_rs      <= register_phase;
                    tx_data    <= register_phase ? page0_data(table_index[4:0])
                                                 : page0_reg(table_index[4:0]);
                    tx_waiting <= 1'b1;
                end else if (tx_waiting && tx_done) begin
                    tx_waiting <= 1'b0;
                    if (!register_phase) begin
                        register_phase <= 1'b1;
                    end else begin
                        register_phase <= 1'b0;
                        if (table_index == 9'd24) begin
                            state <= ST_SLEEP_OUT;
                        end else begin
                            table_index <= table_index + 1'b1;
                        end
                    end
                end
            end

            ST_SLEEP_OUT: begin
                if (!tx_waiting && tx_ready) begin
                    tx_valid   <= 1'b1;
                    tx_rs      <= 1'b0;
                    tx_data    <= 16'h1100;
                    tx_waiting <= 1'b1;
                end else if (tx_waiting && tx_done) begin
                    tx_waiting  <= 1'b0;
                    delay_count <= 32'd0;
                    state       <= ST_SLEEP_WAIT;
                end
            end

            ST_SLEEP_WAIT: begin
                if (delay_count == SLEEP_OUT_CYCLES-1) begin
                    delay_count <= 32'd0;
                    state       <= ST_DISPLAY_ON;
                end else begin
                    delay_count <= delay_count + 1'b1;
                end
            end

            ST_DISPLAY_ON: begin
                if (!tx_waiting && tx_ready) begin
                    tx_valid   <= 1'b1;
                    tx_rs      <= 1'b0;
                    tx_data    <= 16'h2900;
                    tx_waiting <= 1'b1;
                end else if (tx_waiting && tx_done) begin
                    tx_waiting  <= 1'b0;
                    delay_count <= 32'd0;
                    table_index <= 9'd0;
                    state       <= ST_DISPLAY_WAIT;
                end
            end

            ST_DISPLAY_WAIT: begin
                if (delay_count == DISPLAY_WAIT_CYCLES-1) begin
                    delay_count    <= 32'd0;
                    table_index    <= 9'd0;
                    register_phase <= 1'b0;
                    state          <= ST_WINDOW;
                end else begin
                    delay_count <= delay_count + 1'b1;
                end
            end

            ST_WINDOW: begin
                if (!tx_waiting && tx_ready) begin
                    tx_valid   <= 1'b1;
                    tx_rs      <= register_phase;
                    tx_data    <= register_phase ? window_data(table_index[3:0])
                                                 : window_reg(table_index[3:0]);
                    tx_waiting <= 1'b1;
                end else if (tx_waiting && tx_done) begin
                    tx_waiting <= 1'b0;
                    if (!register_phase) begin
                        register_phase <= 1'b1;
                    end else begin
                        register_phase <= 1'b0;
                        if (table_index == 9'd7) begin
                            state <= ST_RAM_CMD;
                        end else begin
                            table_index <= table_index + 1'b1;
                        end
                    end
                end
            end

            ST_RAM_CMD: begin
                if (!tx_waiting && tx_ready) begin
                    tx_valid   <= 1'b1;
                    tx_rs      <= 1'b0;
                    tx_data    <= 16'h2C00;
                    tx_waiting <= 1'b1;
                end else if (tx_waiting && tx_done) begin
                    tx_waiting  <= 1'b0;
                    pixel_x     <= 10'd0;
                    pixel_y     <= 9'd0;
                    pixel_count <= 19'd0;
                    state       <= ST_PIXELS;
                end
            end

            ST_PIXELS: begin
                if (!tx_waiting && tx_ready) begin
                    tx_valid   <= 1'b1;
                    tx_rs      <= 1'b1;
                    tx_data    <= color_bar(pixel_x);
                    tx_waiting <= 1'b1;
                end else if (tx_waiting && tx_done) begin
                    tx_waiting <= 1'b0;
                    if (pixel_count == PIXEL_COUNT-1) begin
                        frame_done <= 1'b1;
                        lcd_bl     <= 1'b1;
                        state      <= ST_DONE;
                    end else begin
                        pixel_count <= pixel_count + 1'b1;
                        if (pixel_x == H_RES-1) begin
                            pixel_x <= 10'd0;
                            pixel_y <= pixel_y + 1'b1;
                        end else begin
                            pixel_x <= pixel_x + 1'b1;
                        end
                    end
                end
            end

            ST_DONE: begin
                // NT35510 refreshes its own GRAM; no repeated write is needed
                // for this static bring-up image.
                lcd_bl <= 1'b1;
            end

            default: state <= ST_RESET_LOW;
        endcase
    end
end

endmodule
