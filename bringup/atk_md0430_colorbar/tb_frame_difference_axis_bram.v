`timescale 1ns / 1ps

module tb_frame_difference_axis_bram;

localparam integer WIDTH  = 32;
localparam integer HEIGHT = 16;

reg clk = 1'b0;
reg rst_n = 1'b0;
reg [23:0] s_tdata = 24'd0;
reg s_tvalid = 1'b0;
wire s_tready;
reg s_tuser = 1'b0;
reg s_tlast = 1'b0;
wire [23:0] m_tdata;
wire m_tvalid;
reg m_tready = 1'b1;
wire m_tuser;
wire m_tlast;

integer output_pixels = 0;
integer white_pixels_frame3 = 0;
integer errors = 0;
integer active_frame = 0;
integer out_x = 0;
integer out_y = 0;

always #5 clk = ~clk;

frame_difference_axis_bram #(
    .IMG_WIDTH(WIDTH),
    .IMG_HEIGHT(HEIGHT),
    .SAMPLE_SHIFT(2)
) dut (
    .clk(clk),
    .rst_n(rst_n),
    .s0_axis_tdata(s_tdata),
    .s0_axis_tvalid(s_tvalid),
    .s0_axis_tready(s_tready),
    .s0_axis_tuser(s_tuser),
    .s0_axis_tlast(s_tlast),
    .s1_axis_tdata(24'd0),
    .s1_axis_tvalid(1'b0),
    .s1_axis_tready(),
    .s1_axis_tuser(1'b0),
    .s1_axis_tlast(1'b0),
    .m_axis_tdata(m_tdata),
    .m_axis_tvalid(m_tvalid),
    .m_axis_tready(m_tready),
    .m_axis_tuser(m_tuser),
    .m_axis_tlast(m_tlast)
);

always @(posedge clk) begin
    if (m_tvalid && m_tready) begin
        output_pixels <= output_pixels + 1;
        if (m_tuser) begin
            active_frame <= active_frame + 1;
            out_x <= 0;
            out_y <= 0;
        end

        // active_frame is incremented by nonblocking assignment on TUSER, so
        // it equals 3 for pixels 1..511 of the third test frame.
        if ((active_frame == 3) && (m_tdata == 24'hffffff)) begin
            white_pixels_frame3 <= white_pixels_frame3 + 1;
        end

        // The four moving sample points in frame 2 produce a box covering
        // x=4..19, y=0..15.  Frame 3 must therefore draw its left border.
        if ((active_frame == 3) && (out_x == 4) && (out_y == 8) &&
            (m_tdata != 24'hffffff)) begin
            $display("ERROR: expected white border at frame3 (4,8), got %h", m_tdata);
            errors <= errors + 1;
        end
        if ((active_frame == 3) && (out_x == 10) && (out_y == 8) &&
            (m_tdata != 24'h000000)) begin
            $display("ERROR: expected black box interior at frame3 (10,8), got %h", m_tdata);
            errors <= errors + 1;
        end

        if (m_tlast) begin
            out_x <= 0;
            out_y <= out_y + 1;
        end else begin
            out_x <= out_x + 1;
        end
    end
end

task send_pixel;
    input integer frame_number;
    input integer px;
    input integer py;
    reg bright;
    begin
        // Frame 2 changes four adjacent 4x4 sample cells.  Only their centre
        // pixels need to be bright because the detector is intentionally
        // subsampled.
        bright = (frame_number == 2) &&
                 (((px == 10) && (py == 6)) ||
                  ((px == 14) && (py == 6)) ||
                  ((px == 10) && (py == 10)) ||
                  ((px == 14) && (py == 10)));
        @(negedge clk);
        s_tdata  = bright ? 24'hffffff : 24'h000000;
        s_tuser  = (px == 0) && (py == 0);
        s_tlast  = (px == WIDTH-1);
        s_tvalid = 1'b1;
        while (!s_tready)
            @(negedge clk);
    end
endtask

task send_frame;
    input integer frame_number;
    integer x;
    integer y;
    begin
        for (y = 0; y < HEIGHT; y = y + 1)
            for (x = 0; x < WIDTH; x = x + 1)
                send_pixel(frame_number, x, y);
        @(negedge clk);
        s_tvalid = 1'b0;
        s_tuser  = 1'b0;
        s_tlast  = 1'b0;
        s_tdata  = 24'd0;
        repeat (8) @(posedge clk);
    end
endtask

initial begin
    repeat (5) @(posedge clk);
    rst_n = 1'b1;
    send_frame(1); // establish history
    send_frame(2); // create motion box
    send_frame(3); // observe the registered box overlay

    if (output_pixels != 3*WIDTH*HEIGHT) begin
        $display("ERROR: expected %0d output pixels, got %0d",
                 3*WIDTH*HEIGHT, output_pixels);
        errors = errors + 1;
    end
    if (white_pixels_frame3 == 0) begin
        $display("ERROR: no frame-3 border pixels were observed");
        errors = errors + 1;
    end

    if (errors == 0)
        $display("PASS: BRAM frame difference, box calculation, and AXI pass-through verified");
    else
        $display("FAIL: %0d error(s)", errors);
    $finish;
end

endmodule
