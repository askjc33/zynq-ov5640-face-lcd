# Rebuild the OV5640 -> frame difference -> DDR -> ATK-MD0430 project.
# Vivado 2018.3, STUDYBOARD Zynq-7020 (XC7Z020-2CLG400).
#
# This is intentionally a separate project from the known-good HDMI design.
# The camera, frame-difference, VDMA and DDR architecture remains unchanged;
# only the final display backend is replaced by axis_nt35510_display.

set script_dir  [file dirname [file normalize [info script]]]
set repo_dir    [file dirname $script_dir]
if {![info exists project_name]} {
    set project_name studyboard_frame_difference_lcd
}
if {![info exists project_dir]} {
    set project_dir [file join $repo_dir build $project_name]
}
if {![info exists export_hdf]} {
    # Vivado 2018.3 baseline: retain HDF output by default.
    set export_hdf 1
}

create_project -force $project_name $project_dir -part xc7z020clg400-2
set_property target_language Verilog [current_project]
set_property simulator_language Mixed [current_project]
set_property ip_repo_paths [list [file join $repo_dir ip_repo]] [current_project]
update_ip_catalog

set frame_rtl_dir [file join $repo_dir ov5640_hdmi.srcs sources_1 imports project]
add_files [list \
    [file join $frame_rtl_dir RGB888_YCbCr444.v] \
    [file join $frame_rtl_dir AXI_VIP_Frame_Difference.v] \
    [file join $repo_dir rtl frame_difference_axis_bram.v] \
    [file join $repo_dir bringup atk_md0430_colorbar lcd8080_write_cycle.v] \
    [file join $repo_dir bringup atk_md0430_colorbar atk_md0430_colorbar_top.v] \
    [file join $repo_dir rtl axis_lcd_frame_writer.v] \
    [file join $repo_dir rtl axis_nt35510_display.v]]

create_bd_design system

set DDR [create_bd_intf_port -mode Master -vlnv xilinx.com:interface:ddrx_rtl:1.0 DDR]
set FIXED_IO [create_bd_intf_port -mode Master \
    -vlnv xilinx.com:display_processing_system7:fixedio_rtl:1.0 FIXED_IO]

# OV5640 physical pins.
set cam_data  [create_bd_port -dir I -from 7 -to 0 cam_data]
set cam_href  [create_bd_port -dir I cam_href]
set cam_pclk  [create_bd_port -dir I -type clk cam_pclk]
set_property CONFIG.FREQ_HZ 72000000 $cam_pclk
set cam_vsync [create_bd_port -dir I cam_vsync]
set cam_rst_n [create_bd_port -dir O cam_rst_n]
set cam_pwdn  [create_bd_port -dir O cam_pwdn]
set cam_xclk  [create_bd_port -dir O -type clk cam_xclk]

# 16-bit Intel-8080 LCD physical pins.
set lcd_data  [create_bd_port -dir O -from 15 -to 0 lcd_data]
set lcd_cs_n  [create_bd_port -dir O lcd_cs_n]
set lcd_rs    [create_bd_port -dir O lcd_rs]
set lcd_wr_n  [create_bd_port -dir O lcd_wr_n]
set lcd_rd_n  [create_bd_port -dir O lcd_rd_n]
set lcd_rst_n [create_bd_port -dir O lcd_rst_n]
set lcd_bl    [create_bd_port -dir O lcd_bl]

# Zynq PS settings recovered from the working STUDYBOARD design.
set ps7 [create_bd_cell -type ip -vlnv xilinx.com:ip:processing_system7:5.5 ps7]
set_property -dict [list \
    CONFIG.PCW_APU_PERIPHERAL_FREQMHZ {767.0} \
    CONFIG.PCW_ARMPLL_CTRL_FBDIV {46} \
    CONFIG.PCW_CPU_PERIPHERAL_DIVISOR0 {2} \
    CONFIG.PCW_DDRPLL_CTRL_FBDIV {32} \
    CONFIG.PCW_DDR_PERIPHERAL_DIVISOR0 {2} \
    CONFIG.PCW_IOPLL_CTRL_FBDIV {30} \
    CONFIG.PCW_FPGA0_PERIPHERAL_FREQMHZ {100.0} \
    CONFIG.PCW_FCLK0_PERIPHERAL_DIVISOR0 {5} \
    CONFIG.PCW_FCLK0_PERIPHERAL_DIVISOR1 {2} \
    CONFIG.PCW_FPGA_FCLK0_ENABLE {1} \
    CONFIG.PCW_EN_UART0 {1} \
    CONFIG.PCW_UART0_PERIPHERAL_ENABLE {1} \
    CONFIG.PCW_UART0_UART0_IO {MIO 14 .. 15} \
    CONFIG.PCW_EN_QSPI {1} \
    CONFIG.PCW_QSPI_PERIPHERAL_ENABLE {1} \
    CONFIG.PCW_QSPI_QSPI_IO {MIO 1 .. 6} \
    CONFIG.PCW_QSPI_GRP_SINGLE_SS_ENABLE {1} \
    CONFIG.PCW_QSPI_GRP_SINGLE_SS_IO {MIO 1 .. 6} \
    CONFIG.PCW_SINGLE_QSPI_DATA_MODE {x4} \
    CONFIG.PCW_EN_GPIO {1} \
    CONFIG.PCW_EN_EMIO_GPIO {1} \
    CONFIG.PCW_GPIO_EMIO_GPIO_ENABLE {1} \
    CONFIG.PCW_GPIO_EMIO_GPIO_IO {2} \
    CONFIG.PCW_GPIO_EMIO_GPIO_WIDTH {2} \
    CONFIG.PCW_USE_FABRIC_INTERRUPT {1} \
    CONFIG.PCW_IRQ_F2P_INTR {1} \
    CONFIG.PCW_USE_M_AXI_GP0 {1} \
    CONFIG.PCW_USE_S_AXI_HP0 {1} \
    CONFIG.PCW_USE_S_AXI_HP1 {1} \
    CONFIG.PCW_S_AXI_HP0_DATA_WIDTH {64} \
    CONFIG.PCW_S_AXI_HP1_DATA_WIDTH {64} \
    CONFIG.PCW_DDR_RAM_HIGHADDR {0x3FFFFFFF} \
    CONFIG.PCW_UIPARAM_DDR_MEMORY_TYPE {DDR 3 (Low Voltage)} \
    CONFIG.PCW_UIPARAM_DDR_PARTNO {MT41K256M16 RE-125} \
    CONFIG.PCW_UIPARAM_DDR_DEVICE_CAPACITY {4096 MBits} \
    CONFIG.PCW_UIPARAM_DDR_DRAM_WIDTH {16 Bits} \
    CONFIG.PCW_UIPARAM_DDR_ROW_ADDR_COUNT {15} \
    CONFIG.PCW_UIPARAM_DDR_COL_ADDR_COUNT {10} \
    CONFIG.PCW_UIPARAM_DDR_BANK_ADDR_COUNT {3} \
    CONFIG.PCW_UIPARAM_DDR_SPEED_BIN {DDR3_1066F} \
    CONFIG.PCW_UIPARAM_DDR_CL {7} \
    CONFIG.PCW_UIPARAM_DDR_CWL {6} \
    CONFIG.PCW_UIPARAM_DDR_T_RCD {7} \
    CONFIG.PCW_UIPARAM_DDR_T_RP {7} \
    CONFIG.PCW_UIPARAM_DDR_T_RC {48.75} \
    CONFIG.PCW_UIPARAM_DDR_T_RAS_MIN {35.0} \
    CONFIG.PCW_UIPARAM_DDR_T_FAW {40.0}] $ps7

connect_bd_intf_net [get_bd_intf_ports DDR] [get_bd_intf_pins ps7/DDR]
connect_bd_intf_net [get_bd_intf_ports FIXED_IO] [get_bd_intf_pins ps7/FIXED_IO]
make_bd_intf_pins_external [get_bd_intf_pins ps7/GPIO_0]
set_property name emio_sccb [get_bd_intf_ports GPIO_0_0]

# FCLK0=100 MHz clocks the AXI fabric and LCD writer.  A Clocking Wizard makes
# the independent 24 MHz OV5640 XCLK.
set rst_100 [create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 rst_100]
set rst_cam [create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 rst_cam]
set cam_clk [create_bd_cell -type ip -vlnv xilinx.com:ip:clk_wiz:6.0 cam_clk]
set_property -dict [list \
    CONFIG.PRIM_IN_FREQ {100.000} \
    CONFIG.CLKOUT1_REQUESTED_OUT_FREQ {24.000} \
    CONFIG.USE_RESET {true} \
    CONFIG.RESET_TYPE {ACTIVE_LOW}] $cam_clk

connect_bd_net [get_bd_pins ps7/FCLK_CLK0] \
    [get_bd_pins ps7/M_AXI_GP0_ACLK] \
    [get_bd_pins ps7/S_AXI_HP0_ACLK] \
    [get_bd_pins ps7/S_AXI_HP1_ACLK] \
    [get_bd_pins rst_100/slowest_sync_clk] \
    [get_bd_pins cam_clk/clk_in1]
connect_bd_net [get_bd_pins ps7/FCLK_RESET0_N] \
    [get_bd_pins rst_100/ext_reset_in] \
    [get_bd_pins rst_cam/ext_reset_in] \
    [get_bd_pins cam_clk/resetn]
connect_bd_net [get_bd_ports cam_pclk] [get_bd_pins rst_cam/slowest_sync_clk]
connect_bd_net [get_bd_pins cam_clk/clk_out1] [get_bd_ports cam_xclk]

# Camera DVP -> AXI4-Stream RGB888.
set capture [create_bd_cell -type ip \
    -vlnv alientek.com:user:ov5640_capture_data:1.0 ov5640_capture_data_0]
set vid_in [create_bd_cell -type ip -vlnv xilinx.com:ip:v_vid_in_axi4s:4.0 v_vid_in_axi4s_0]
set_property -dict [list \
    CONFIG.C_M_AXIS_VIDEO_FORMAT {2} \
    CONFIG.C_HAS_ASYNC_CLK {1} \
    CONFIG.C_ADDR_WIDTH {11}] $vid_in

connect_bd_net [get_bd_ports cam_data]  [get_bd_pins ov5640_capture_data_0/cam_data]
connect_bd_net [get_bd_ports cam_href]  [get_bd_pins ov5640_capture_data_0/cam_href]
connect_bd_net [get_bd_ports cam_pclk]  [get_bd_pins ov5640_capture_data_0/cam_pclk]
connect_bd_net [get_bd_ports cam_vsync] [get_bd_pins ov5640_capture_data_0/cam_vsync]
connect_bd_net [get_bd_pins ov5640_capture_data_0/cam_rst_n] [get_bd_ports cam_rst_n]
connect_bd_net [get_bd_pins ov5640_capture_data_0/cam_pwdn]  [get_bd_ports cam_pwdn]
connect_bd_net [get_bd_pins rst_cam/peripheral_aresetn] [get_bd_pins ov5640_capture_data_0/rst_n]
connect_bd_intf_net [get_bd_intf_pins ov5640_capture_data_0/cmos_rgb] [get_bd_intf_pins v_vid_in_axi4s_0/vid_io_in]
connect_bd_net [get_bd_pins ov5640_capture_data_0/cmos_frame_clk] [get_bd_pins v_vid_in_axi4s_0/vid_io_in_clk]
connect_bd_net [get_bd_pins ov5640_capture_data_0/cmos_frame_ce] [get_bd_pins v_vid_in_axi4s_0/vid_io_in_ce]
connect_bd_net [get_bd_pins rst_cam/peripheral_reset] [get_bd_pins v_vid_in_axi4s_0/vid_io_in_reset]
connect_bd_net [get_bd_pins ps7/FCLK_CLK0] [get_bd_pins v_vid_in_axi4s_0/aclk]
connect_bd_net [get_bd_pins rst_100/peripheral_aresetn] [get_bd_pins v_vid_in_axi4s_0/aresetn]

# Motion processing and frame buffers.  VDMA0 writes the processed camera
# stream to DDR.  VDMA1 independently reads the same frame stores for LCD.
set frame_diff [create_bd_cell -type module -reference frame_difference_axis_bram frame_difference]
set vdma0 [create_bd_cell -type ip -vlnv xilinx.com:ip:axi_vdma:6.3 axi_vdma_0]
set_property -dict [list \
    CONFIG.c_include_sg {0} \
    CONFIG.c_include_mm2s {1} \
    CONFIG.c_include_s2mm {1} \
    CONFIG.c_num_fstores {3} \
    CONFIG.c_m_axis_mm2s_tdata_width {24}] $vdma0
set vdma1 [create_bd_cell -type ip -vlnv xilinx.com:ip:axi_vdma:6.3 axi_vdma_1]
set_property -dict [list \
    CONFIG.c_include_sg {0} \
    CONFIG.c_include_mm2s {1} \
    CONFIG.c_include_s2mm {0} \
    CONFIG.c_num_fstores {3} \
    CONFIG.c_m_axis_mm2s_tdata_width {24}] $vdma1

# Linux's Xilinx VDMA DMAEngine driver requests one interrupt for every
# enabled channel.  The original bare-metal design polled the status
# registers, so its interrupt outputs were intentionally left unconnected.
# Route all three enabled channels to PS IRQ_F2P for Linux:
#   In0 / IRQ_F2P[0]: VDMA0 MM2S (history/compatibility read channel)
#   In1 / IRQ_F2P[1]: VDMA0 S2MM (processed camera capture channel)
#   In2 / IRQ_F2P[2]: VDMA1 MM2S (LCD display read channel)
set vdma_irq_concat [create_bd_cell -type ip -vlnv xilinx.com:ip:xlconcat:2.1 vdma_irq_concat]
set_property -dict [list CONFIG.NUM_PORTS {3}] $vdma_irq_concat
connect_bd_net [get_bd_pins axi_vdma_0/mm2s_introut] \
    [get_bd_pins vdma_irq_concat/In0]
connect_bd_net [get_bd_pins axi_vdma_0/s2mm_introut] \
    [get_bd_pins vdma_irq_concat/In1]
connect_bd_net [get_bd_pins axi_vdma_1/mm2s_introut] \
    [get_bd_pins vdma_irq_concat/In2]
connect_bd_net [get_bd_pins vdma_irq_concat/dout] \
    [get_bd_pins ps7/IRQ_F2P]

connect_bd_intf_net [get_bd_intf_pins v_vid_in_axi4s_0/video_out] [get_bd_intf_pins frame_difference/S0_AXIS]
connect_bd_intf_net [get_bd_intf_pins axi_vdma_0/M_AXIS_MM2S] [get_bd_intf_pins frame_difference/S1_AXIS]
connect_bd_intf_net [get_bd_intf_pins frame_difference/M_AXIS] [get_bd_intf_pins axi_vdma_0/S_AXIS_S2MM]
connect_bd_net [get_bd_pins ps7/FCLK_CLK0] \
    [get_bd_pins frame_difference/clk] \
    [get_bd_pins axi_vdma_0/m_axis_mm2s_aclk] \
    [get_bd_pins axi_vdma_0/s_axis_s2mm_aclk]
connect_bd_net [get_bd_pins rst_100/peripheral_aresetn] \
    [get_bd_pins frame_difference/rst_n] \
    [get_bd_pins axi_vdma_0/axi_resetn] \
    [get_bd_pins axi_vdma_1/axi_resetn]

# VDMA1 AXI stream -> RGB565 -> 8080 LCD writes.
set lcd [create_bd_cell -type module -reference axis_nt35510_display lcd_display]
connect_bd_intf_net [get_bd_intf_pins axi_vdma_1/M_AXIS_MM2S] [get_bd_intf_pins lcd_display/S_AXIS]
connect_bd_net [get_bd_pins ps7/FCLK_CLK0] \
    [get_bd_pins axi_vdma_1/m_axis_mm2s_aclk] \
    [get_bd_pins lcd_display/clk]
connect_bd_net [get_bd_pins rst_100/peripheral_aresetn] [get_bd_pins lcd_display/rst_n]
connect_bd_net [get_bd_pins lcd_display/lcd_data]  [get_bd_ports lcd_data]
connect_bd_net [get_bd_pins lcd_display/lcd_cs_n]  [get_bd_ports lcd_cs_n]
connect_bd_net [get_bd_pins lcd_display/lcd_rs]    [get_bd_ports lcd_rs]
connect_bd_net [get_bd_pins lcd_display/lcd_wr_n]  [get_bd_ports lcd_wr_n]
connect_bd_net [get_bd_pins lcd_display/lcd_rd_n]  [get_bd_ports lcd_rd_n]
connect_bd_net [get_bd_pins lcd_display/lcd_rst_n] [get_bd_ports lcd_rst_n]
connect_bd_net [get_bd_pins lcd_display/lcd_bl]    [get_bd_ports lcd_bl]

# PS GP0 controls the two VDMAs.
set ctrl_ic [create_bd_cell -type ip -vlnv xilinx.com:ip:axi_interconnect:2.1 ctrl_ic]
set_property -dict [list CONFIG.NUM_MI {2}] $ctrl_ic
connect_bd_intf_net [get_bd_intf_pins ps7/M_AXI_GP0] [get_bd_intf_pins ctrl_ic/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins ctrl_ic/M00_AXI] [get_bd_intf_pins axi_vdma_0/S_AXI_LITE]
connect_bd_intf_net [get_bd_intf_pins ctrl_ic/M01_AXI] [get_bd_intf_pins axi_vdma_1/S_AXI_LITE]
connect_bd_net [get_bd_pins ps7/FCLK_CLK0] \
    [get_bd_pins ctrl_ic/ACLK] \
    [get_bd_pins ctrl_ic/S00_ACLK] \
    [get_bd_pins ctrl_ic/M00_ACLK] \
    [get_bd_pins ctrl_ic/M01_ACLK] \
    [get_bd_pins axi_vdma_0/s_axi_lite_aclk] \
    [get_bd_pins axi_vdma_1/s_axi_lite_aclk]
connect_bd_net [get_bd_pins rst_100/interconnect_aresetn] [get_bd_pins ctrl_ic/ARESETN]
connect_bd_net [get_bd_pins rst_100/peripheral_aresetn] \
    [get_bd_pins ctrl_ic/S00_ARESETN] \
    [get_bd_pins ctrl_ic/M00_ARESETN] \
    [get_bd_pins ctrl_ic/M01_ARESETN]

# HP0 carries VDMA0 capture writes. Its legacy MM2S path is wired but is not
# started by this application's ONLY_WRITE mode; history resides in PL BRAM.
set mem_ic [create_bd_cell -type ip -vlnv xilinx.com:ip:axi_interconnect:2.1 mem_ic]
set_property -dict [list CONFIG.NUM_SI {2} CONFIG.NUM_MI {1}] $mem_ic
connect_bd_intf_net [get_bd_intf_pins axi_vdma_0/M_AXI_MM2S] [get_bd_intf_pins mem_ic/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins axi_vdma_0/M_AXI_S2MM] [get_bd_intf_pins mem_ic/S01_AXI]
connect_bd_intf_net [get_bd_intf_pins mem_ic/M00_AXI] [get_bd_intf_pins ps7/S_AXI_HP0]
connect_bd_net [get_bd_pins ps7/FCLK_CLK0] \
    [get_bd_pins mem_ic/ACLK] \
    [get_bd_pins mem_ic/S00_ACLK] \
    [get_bd_pins mem_ic/S01_ACLK] \
    [get_bd_pins mem_ic/M00_ACLK] \
    [get_bd_pins axi_vdma_0/m_axi_mm2s_aclk] \
    [get_bd_pins axi_vdma_0/m_axi_s2mm_aclk]
connect_bd_net [get_bd_pins rst_100/interconnect_aresetn] [get_bd_pins mem_ic/ARESETN]
connect_bd_net [get_bd_pins rst_100/peripheral_aresetn] \
    [get_bd_pins mem_ic/S00_ARESETN] \
    [get_bd_pins mem_ic/S01_ARESETN] \
    [get_bd_pins mem_ic/M00_ARESETN]

# HP1 is dedicated to the LCD reader so simultaneous capture/display traffic
# does not share the HP0 interconnect response path.
set lcd_mem_ic [create_bd_cell -type ip -vlnv xilinx.com:ip:axi_interconnect:2.1 lcd_mem_ic]
set_property -dict [list CONFIG.NUM_SI {1} CONFIG.NUM_MI {1}] $lcd_mem_ic
connect_bd_intf_net [get_bd_intf_pins axi_vdma_1/M_AXI_MM2S] [get_bd_intf_pins lcd_mem_ic/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins lcd_mem_ic/M00_AXI] [get_bd_intf_pins ps7/S_AXI_HP1]
connect_bd_net [get_bd_pins ps7/FCLK_CLK0] \
    [get_bd_pins lcd_mem_ic/ACLK] \
    [get_bd_pins lcd_mem_ic/S00_ACLK] \
    [get_bd_pins lcd_mem_ic/M00_ACLK] \
    [get_bd_pins axi_vdma_1/m_axi_mm2s_aclk]
connect_bd_net [get_bd_pins rst_100/interconnect_aresetn] [get_bd_pins lcd_mem_ic/ARESETN]
connect_bd_net [get_bd_pins rst_100/peripheral_aresetn] \
    [get_bd_pins lcd_mem_ic/S00_ARESETN] \
    [get_bd_pins lcd_mem_ic/M00_ARESETN]

assign_bd_address -offset 0x43000000 -range 0x00010000 \
    -target_address_space [get_bd_addr_spaces ps7/Data] \
    [get_bd_addr_segs axi_vdma_0/S_AXI_LITE/Reg]
assign_bd_address -offset 0x43010000 -range 0x00010000 \
    -target_address_space [get_bd_addr_spaces ps7/Data] \
    [get_bd_addr_segs axi_vdma_1/S_AXI_LITE/Reg]

foreach addr_space [list axi_vdma_0/Data_MM2S axi_vdma_0/Data_S2MM] {
    assign_bd_address -offset 0x00000000 -range 0x40000000 \
        -target_address_space [get_bd_addr_spaces $addr_space] \
        [get_bd_addr_segs ps7/S_AXI_HP0/HP0_DDR_LOWOCM]
}
assign_bd_address -offset 0x00000000 -range 0x40000000 \
    -target_address_space [get_bd_addr_spaces axi_vdma_1/Data_MM2S] \
    [get_bd_addr_segs ps7/S_AXI_HP1/HP1_DDR_LOWOCM]

validate_bd_design
save_bd_design

set bd_file [get_files [file join $project_dir \
    ${project_name}.srcs sources_1 bd system system.bd]]
if {[llength $bd_file] != 1} {
    error "Expected exactly one generated system.bd in project '$project_name', got: $bd_file"
}
generate_target all $bd_file
set wrapper_file [make_wrapper -files $bd_file -top]
add_files -norecurse $wrapper_file
add_files -fileset constrs_1 [file join $repo_dir constraints studyboard_lcd_top.xdc]
set_property top system_wrapper [current_fileset]
update_compile_order -fileset sources_1

# Vivado 2018.3 power_opt_design can crash on the frame-difference module's
# large distributed-RAM mux network (Pwropt HACOOException / access violation).
# This step is optional and does not affect functional implementation.
set_property STEPS.OPT_DESIGN.ARGS.DIRECTIVE NoBramPowerOpt [get_runs impl_1]
set_property STEPS.POWER_OPT_DESIGN.IS_ENABLED false [get_runs impl_1]

if {$export_hdf} {
    write_hwdef -force -file [file join $project_dir system.hdf]
    puts "HARDWARE: [file join $project_dir system.hdf]"
}
puts "PROJECT: [file join $project_dir ${project_name}.xpr]"
exit
