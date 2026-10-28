`timescale 1ns / 1ps

module tb_atk_md0430_colorbar;

localparam integer H_RES = 8;
localparam integer V_RES = 4;

reg sys_clk   = 1'b0;
reg sys_rst_n = 1'b0;

wire [15:0] lcd_data;
wire lcd_cs_n;
wire lcd_rs;
wire lcd_wr_n;
wire lcd_rd_n;
wire lcd_rst_n;
wire lcd_bl;

integer pixel_seen = 0;
integer error_count = 0;
integer x_pos;
reg ram_write_seen = 1'b0;
reg sleep_out_seen = 1'b0;
reg display_on_seen = 1'b0;
reg [15:0] expected_color;

always #10 sys_clk = ~sys_clk; // 50 MHz

atk_md0430_colorbar_top #(
    .CLK_HZ(50_000_000),
    .H_RES(H_RES),
    .V_RES(V_RES),
    .SIMULATION(1)
) dut (
    .sys_clk(sys_clk),
    .sys_rst_n(sys_rst_n),
    .lcd_data(lcd_data),
    .lcd_cs_n(lcd_cs_n),
    .lcd_rs(lcd_rs),
    .lcd_wr_n(lcd_wr_n),
    .lcd_rd_n(lcd_rd_n),
    .lcd_rst_n(lcd_rst_n),
    .lcd_bl(lcd_bl)
);

function [15:0] expected_bar;
    input integer x;
    begin
        case (x)
            0: expected_bar = 16'hFFFF;
            1: expected_bar = 16'hFFE0;
            2: expected_bar = 16'h07FF;
            3: expected_bar = 16'h07E0;
            4: expected_bar = 16'hF81F;
            5: expected_bar = 16'hF800;
            6: expected_bar = 16'h001F;
            default: expected_bar = 16'h0000;
        endcase
    end
endfunction

// The LCD latches data on the rising edge of WR.  Observing that exact edge
// makes this monitor match what the real controller sees at its pins.
always @(posedge lcd_wr_n) begin
    if (sys_rst_n && !lcd_cs_n) begin
        if (!lcd_rs) begin
            if (lcd_data == 16'h1100) sleep_out_seen = 1'b1;
            if (lcd_data == 16'h2900) display_on_seen = 1'b1;
            if (lcd_data == 16'h2C00) ram_write_seen = 1'b1;
        end else if (ram_write_seen) begin
            x_pos = pixel_seen % H_RES;
            expected_color = expected_bar(x_pos);
            if (lcd_data !== expected_color) begin
                $display("ERROR pixel=%0d x=%0d got=%04x expected=%04x",
                         pixel_seen, x_pos, lcd_data, expected_color);
                error_count = error_count + 1;
            end
            pixel_seen = pixel_seen + 1;
        end
    end
end

initial begin
    #100;
    sys_rst_n = 1'b1;

    // The reduced-delay simulation should finish well inside this timeout.
    fork
        begin
            wait (lcd_bl == 1'b1);
            #100;
            if (!sleep_out_seen) begin
                $display("ERROR: sleep-out command 0x1100 was not observed");
                error_count = error_count + 1;
            end
            if (!display_on_seen) begin
                $display("ERROR: display-on command 0x2900 was not observed");
                error_count = error_count + 1;
            end
            if (!ram_write_seen) begin
                $display("ERROR: RAM-write command 0x2C00 was not observed");
                error_count = error_count + 1;
            end
            if (pixel_seen != H_RES*V_RES) begin
                $display("ERROR: pixel count=%0d expected=%0d",
                         pixel_seen, H_RES*V_RES);
                error_count = error_count + 1;
            end
            if (lcd_rd_n !== 1'b1) begin
                $display("ERROR: RD must stay inactive in this write-only test");
                error_count = error_count + 1;
            end

            if (error_count == 0)
                $display("PASS: initialization commands and %0d RGB565 pixels verified",
                         pixel_seen);
            else
                $display("FAIL: %0d error(s)", error_count);
            $finish;
        end

        begin
            #2000000;
            $display("FAIL: simulation timeout");
            $finish;
        end
    join
end

endmodule

