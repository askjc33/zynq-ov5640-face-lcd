# STUDYBOARD (schematic revision 2024-06-21) constraints
# Device/package: XC7Z020-2CLG400, recovered from the official board project.

# -----------------------------------------------------------------------------
# OV5640 DVP camera interface - Bank 34, 3.3 V
# -----------------------------------------------------------------------------

set_property -dict {PACKAGE_PIN P14 IOSTANDARD LVCMOS33} [get_ports cam_rst_n]
set_property -dict {PACKAGE_PIN V15 IOSTANDARD LVCMOS33} [get_ports cam_pwdn]

set_property -dict {PACKAGE_PIN R14 IOSTANDARD LVCMOS33} [get_ports {cam_data[0]}]
set_property -dict {PACKAGE_PIN U13 IOSTANDARD LVCMOS33} [get_ports {cam_data[1]}]
set_property -dict {PACKAGE_PIN V13 IOSTANDARD LVCMOS33} [get_ports {cam_data[2]}]
set_property -dict {PACKAGE_PIN U15 IOSTANDARD LVCMOS33} [get_ports {cam_data[3]}]
set_property -dict {PACKAGE_PIN U14 IOSTANDARD LVCMOS33} [get_ports {cam_data[4]}]
set_property -dict {PACKAGE_PIN W13 IOSTANDARD LVCMOS33} [get_ports {cam_data[5]}]
set_property -dict {PACKAGE_PIN V12 IOSTANDARD LVCMOS33} [get_ports {cam_data[6]}]
set_property -dict {PACKAGE_PIN Y14 IOSTANDARD LVCMOS33} [get_ports {cam_data[7]}]

set_property -dict {PACKAGE_PIN T12 IOSTANDARD LVCMOS33} [get_ports cam_href]
set_property -dict {PACKAGE_PIN W14 IOSTANDARD LVCMOS33} [get_ports cam_pclk]
set_property -dict {PACKAGE_PIN U12 IOSTANDARD LVCMOS33} [get_ports cam_vsync]

# Camera reference clock. Drive this port with a 24 MHz PS FCLK or Clocking Wizard
# output if the fitted OV5640 module does not provide its own oscillator.
set_property -dict {PACKAGE_PIN W15 IOSTANDARD LVCMOS33 SLEW SLOW} [get_ports cam_xclk]

# SCCB is driven by the PS GPIO EMIO bit-bang driver used by the SDK application.
# The board already contains 5.1 kohm pull-ups on both nets.
set_property -dict {PACKAGE_PIN T10 IOSTANDARD LVCMOS33} [get_ports {emio_sccb_tri_io[0]}]
set_property -dict {PACKAGE_PIN T11 IOSTANDARD LVCMOS33} [get_ports {emio_sccb_tri_io[1]}]

# The configured OV5640 mode in the original application uses a 72 MHz PCLK.
create_clock -period 13.888 -name cam_pclk [get_ports cam_pclk]

# W14 is not a clock-capable input. This matches the board routing and is required
# for the camera PCLK to enter the fabric. Review timing after implementation.
set_property CLOCK_DEDICATED_ROUTE FALSE [get_nets -of_objects [get_ports cam_pclk]]

# -----------------------------------------------------------------------------
# HDMI/DVI transmitter - Bank 35, direct TMDS output
# -----------------------------------------------------------------------------

set_property -dict {PACKAGE_PIN J18 IOSTANDARD TMDS_33} [get_ports TMDS_tmds_clk_p]
set_property -dict {PACKAGE_PIN H18 IOSTANDARD TMDS_33} [get_ports TMDS_tmds_clk_n]

set_property -dict {PACKAGE_PIN G19 IOSTANDARD TMDS_33} [get_ports {TMDS_tmds_data_p[0]}]
set_property -dict {PACKAGE_PIN G20 IOSTANDARD TMDS_33} [get_ports {TMDS_tmds_data_n[0]}]
set_property -dict {PACKAGE_PIN K19 IOSTANDARD TMDS_33} [get_ports {TMDS_tmds_data_p[1]}]
set_property -dict {PACKAGE_PIN J19 IOSTANDARD TMDS_33} [get_ports {TMDS_tmds_data_n[1]}]
set_property -dict {PACKAGE_PIN J20 IOSTANDARD TMDS_33} [get_ports {TMDS_tmds_data_p[2]}]
set_property -dict {PACKAGE_PIN H20 IOSTANDARD TMDS_33} [get_ports {TMDS_tmds_data_n[2]}]

# This STUDYBOARD has no HDMI tmds_oen signal. Do not export the DVI IP's
# constant-high tmds_oen output from the top-level design.
