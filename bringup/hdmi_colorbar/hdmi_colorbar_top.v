`timescale 1ns / 1ps

module hdmi_colorbar_top (
    input  wire       sys_clk,
    input  wire       sys_rst_n,
    output wire       TMDS_tmds_clk_p,
    output wire       TMDS_tmds_clk_n,
    output wire [2:0] TMDS_tmds_data_p,
    output wire [2:0] TMDS_tmds_data_n
);

wire pixel_clk;
wire pixel_clk_x5;
wire clock_locked;

hdmi_clk_wiz u_hdmi_clk_wiz (
    .clk_in1  (sys_clk),
    .resetn   (sys_rst_n),
    .clk_out1 (pixel_clk),
    .clk_out2 (pixel_clk_x5),
    .locked   (clock_locked)
);

localparam integer H_SYNC   = 96;
localparam integer H_BACK   = 48;
localparam integer H_ACTIVE = 640;
localparam integer H_FRONT  = 16;
localparam integer H_TOTAL  = H_SYNC + H_BACK + H_ACTIVE + H_FRONT;

localparam integer V_SYNC   = 2;
localparam integer V_BACK   = 33;
localparam integer V_ACTIVE = 480;
localparam integer V_FRONT  = 10;
localparam integer V_TOTAL  = V_SYNC + V_BACK + V_ACTIVE + V_FRONT;

reg [9:0] h_count = 10'd0;
reg [9:0] v_count = 10'd0;

always @(posedge pixel_clk or negedge clock_locked) begin
    if (!clock_locked) begin
        h_count <= 10'd0;
        v_count <= 10'd0;
    end else if (h_count == H_TOTAL - 1) begin
        h_count <= 10'd0;
        if (v_count == V_TOTAL - 1)
            v_count <= 10'd0;
        else
            v_count <= v_count + 1'b1;
    end else begin
        h_count <= h_count + 1'b1;
    end
end

wire video_hsync = (h_count >= H_SYNC);
wire video_vsync = (v_count >= V_SYNC);
wire video_de = (h_count >= H_SYNC + H_BACK) &&
                (h_count <  H_SYNC + H_BACK + H_ACTIVE) &&
                (v_count >= V_SYNC + V_BACK) &&
                (v_count <  V_SYNC + V_BACK + V_ACTIVE);

wire [9:0] active_x = h_count - (H_SYNC + H_BACK);
reg  [23:0] video_rgb;

always @* begin
    if (!video_de)
        video_rgb = 24'h000000;
    else if (active_x < 10'd80)
        video_rgb = 24'hFFFFFF;
    else if (active_x < 10'd160)
        video_rgb = 24'hFFFF00;
    else if (active_x < 10'd240)
        video_rgb = 24'h00FFFF;
    else if (active_x < 10'd320)
        video_rgb = 24'h00FF00;
    else if (active_x < 10'd400)
        video_rgb = 24'hFF00FF;
    else if (active_x < 10'd480)
        video_rgb = 24'hFF0000;
    else if (active_x < 10'd560)
        video_rgb = 24'h0000FF;
    else
        video_rgb = 24'h000000;
end

wire unused_tmds_oen;

dvi_transmitter_top u_dvi_transmitter (
    .pclk        (pixel_clk),
    .pclk_x5     (pixel_clk_x5),
    .reset_n     (sys_rst_n & clock_locked),
    .video_din   (video_rgb),
    .video_hsync (video_hsync),
    .video_vsync (video_vsync),
    .video_de    (video_de),
    .tmds_clk_p  (TMDS_tmds_clk_p),
    .tmds_clk_n  (TMDS_tmds_clk_n),
    .tmds_data_p (TMDS_tmds_data_p),
    .tmds_data_n (TMDS_tmds_data_n),
    .tmds_oen    (unused_tmds_oen)
);

endmodule

