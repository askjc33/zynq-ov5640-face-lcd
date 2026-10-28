`timescale 1ns / 1ps

module tb_axis_lcd_frame_writer;

localparam integer INPUT_WIDTH  = 4;
localparam integer INPUT_HEIGHT = 2;
localparam integer LCD_WIDTH    = 8;
localparam integer LCD_HEIGHT   = 2;

reg clk = 1'b0;
reg rst_n = 1'b0;
reg enable = 1'b0;
reg [23:0] tdata = 24'h000000;
reg tvalid = 1'b0;
wire tready;
reg tuser = 1'b0;
reg tlast = 1'b0;

wire [15:0] lcd_data;
wire lcd_cs_n;
wire lcd_rs;
wire lcd_wr_n;
wire clear_done;
wire protocol_error;
wire [31:0] frame_count;

integer transfer_count = 0;
integer pixel_index = 0;
reg [15:0] observed_data [0:80];
reg        observed_rs   [0:80];

always #5 clk = ~clk;

axis_lcd_frame_writer #(
    .INPUT_WIDTH  (INPUT_WIDTH),
    .INPUT_HEIGHT (INPUT_HEIGHT),
    .LCD_WIDTH    (LCD_WIDTH),
    .LCD_HEIGHT   (LCD_HEIGHT),
    .X_OFFSET     (2),
    .Y_OFFSET     (0)
) dut (
    .clk(clk),
    .rst_n(rst_n),
    .enable(enable),
    .s_axis_tdata(tdata),
    .s_axis_tvalid(tvalid),
    .s_axis_tready(tready),
    .s_axis_tuser(tuser),
    .s_axis_tlast(tlast),
    .lcd_data(lcd_data),
    .lcd_cs_n(lcd_cs_n),
    .lcd_rs(lcd_rs),
    .lcd_wr_n(lcd_wr_n),
    .init_clear_done(clear_done),
    .axis_protocol_error(protocol_error),
    .displayed_frame_count(frame_count)
);

// The low-level engine pulses tx_done after every completed LCD transfer.
always @(posedge clk) begin
    if (dut.u_write_cycle.tx_done) begin
        observed_data[transfer_count] <= lcd_data;
        observed_rs[transfer_count] <= lcd_rs;
        transfer_count <= transfer_count + 1;
    end
end

task check_transfer;
    input integer index;
    input expected_rs;
    input [15:0] expected_data;
    begin
        if (observed_rs[index] !== expected_rs ||
            observed_data[index] !== expected_data) begin
            $display("FAIL transfer %0d: got RS=%b DATA=%04h expected RS=%b DATA=%04h",
                index, observed_rs[index], observed_data[index],
                expected_rs, expected_data);
            $finish;
        end
    end
endtask

always @(posedge clk) begin
    if (tvalid && tready) begin
        pixel_index <= pixel_index + 1;
        case (pixel_index + 1)
            1: tdata <= 24'h00ff00; // green -> 07e0
            2: tdata <= 24'h0000ff; // blue  -> 001f
            3: tdata <= 24'hffffff; // white -> ffff
            4: tdata <= 24'h000000; // black -> 0000
            5: tdata <= 24'hffff00; // yellow -> ffe0
            6: tdata <= 24'h00ffff; // cyan -> 07ff
            7: tdata <= 24'hff00ff; // magenta -> f81f
            default: tdata <= 24'h000000;
        endcase
        tuser <= 1'b0;
        tlast <= ((pixel_index + 1) == 3) || ((pixel_index + 1) == 7);
    end
end

initial begin
    repeat (5) @(posedge clk);
    rst_n <= 1'b1;
    repeat (2) @(posedge clk);
    enable <= 1'b1;

    wait (clear_done);
    // Present the first SOF pixel and obey AXI stability while TREADY is low.
    @(posedge clk);
    tdata <= 24'hff0000; // red -> f800
    tuser <= 1'b1;
    tlast <= 1'b0;
    tvalid <= 1'b1;

    wait (frame_count == 1);
    @(posedge clk);
    tvalid <= 1'b0;

    // 17 transfers set the clear window/RAM command, then 16 black pixels.
    check_transfer(0,  1'b0, 16'h2a00);
    check_transfer(1,  1'b1, 16'h0000);
    check_transfer(4,  1'b0, 16'h2a02);
    check_transfer(5,  1'b1, 16'h0000);
    check_transfer(7,  1'b1, 16'h0007);
    check_transfer(15, 1'b1, 16'h0001);
    check_transfer(16, 1'b0, 16'h2c00);
    check_transfer(17, 1'b1, 16'h0000);
    check_transfer(32, 1'b1, 16'h0000);

    // Video window begins at transfer 33: X=2..5, Y=0..1.
    check_transfer(33, 1'b0, 16'h2a00);
    check_transfer(35, 1'b0, 16'h2a01);
    check_transfer(36, 1'b1, 16'h0002);
    check_transfer(40, 1'b1, 16'h0005);
    check_transfer(49, 1'b0, 16'h2c00);

    // Eight RGB888 source pixels must become the expected RGB565 sequence.
    check_transfer(50, 1'b1, 16'hf800);
    check_transfer(51, 1'b1, 16'h07e0);
    check_transfer(52, 1'b1, 16'h001f);
    check_transfer(53, 1'b1, 16'hffff);
    check_transfer(54, 1'b1, 16'h0000);
    check_transfer(55, 1'b1, 16'hffe0);
    check_transfer(56, 1'b1, 16'h07ff);
    check_transfer(57, 1'b1, 16'hf81f);

    if (protocol_error) begin
        $display("FAIL: AXI line protocol checker reported an error");
        $finish;
    end

    $display("PASS: clear, centered window, AXI hold, and RGB888-to-RGB565 verified");
    $finish;
end

initial begin
    #200000;
    $display("FAIL: timeout");
    $finish;
end

endmodule
