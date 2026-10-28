# STUDYBOARD clock/reset.  The on-board PL oscillator is 50 MHz.
set_property -dict {PACKAGE_PIN U18 IOSTANDARD LVCMOS33} [get_ports sys_clk]
set_property -dict {PACKAGE_PIN J15 IOSTANDARD LVCMOS33 PULLUP true} [get_ports sys_rst_n]
create_clock -period 20.000 -name sys_clk [get_ports sys_clk]

# ATK-MD0430 V2.0 D0..D15 connected to STUDYBOARD J7-15..J7-30.
set_property -dict {PACKAGE_PIN H15 IOSTANDARD LVCMOS33} [get_ports {lcd_data[0]}]
set_property -dict {PACKAGE_PIN H16 IOSTANDARD LVCMOS33} [get_ports {lcd_data[1]}]
set_property -dict {PACKAGE_PIN G15 IOSTANDARD LVCMOS33} [get_ports {lcd_data[2]}]
set_property -dict {PACKAGE_PIN H17 IOSTANDARD LVCMOS33} [get_ports {lcd_data[3]}]
set_property -dict {PACKAGE_PIN N15 IOSTANDARD LVCMOS33} [get_ports {lcd_data[4]}]
set_property -dict {PACKAGE_PIN E17 IOSTANDARD LVCMOS33} [get_ports {lcd_data[5]}]
set_property -dict {PACKAGE_PIN N16 IOSTANDARD LVCMOS33} [get_ports {lcd_data[6]}]
set_property -dict {PACKAGE_PIN D18 IOSTANDARD LVCMOS33} [get_ports {lcd_data[7]}]
set_property -dict {PACKAGE_PIN M14 IOSTANDARD LVCMOS33} [get_ports {lcd_data[8]}]
set_property -dict {PACKAGE_PIN F19 IOSTANDARD LVCMOS33} [get_ports {lcd_data[9]}]
set_property -dict {PACKAGE_PIN M15 IOSTANDARD LVCMOS33} [get_ports {lcd_data[10]}]
set_property -dict {PACKAGE_PIN F20 IOSTANDARD LVCMOS33} [get_ports {lcd_data[11]}]
set_property -dict {PACKAGE_PIN L14 IOSTANDARD LVCMOS33} [get_ports {lcd_data[12]}]
set_property -dict {PACKAGE_PIN K17 IOSTANDARD LVCMOS33} [get_ports {lcd_data[13]}]
set_property -dict {PACKAGE_PIN L15 IOSTANDARD LVCMOS33} [get_ports {lcd_data[14]}]
set_property -dict {PACKAGE_PIN K18 IOSTANDARD LVCMOS33} [get_ports {lcd_data[15]}]

# LCD control connected to J7-31..J7-36.
set_property -dict {PACKAGE_PIN L16 IOSTANDARD LVCMOS33} [get_ports lcd_cs_n]
set_property -dict {PACKAGE_PIN K14 IOSTANDARD LVCMOS33} [get_ports lcd_rs]
set_property -dict {PACKAGE_PIN L17 IOSTANDARD LVCMOS33} [get_ports lcd_wr_n]
set_property -dict {PACKAGE_PIN J14 IOSTANDARD LVCMOS33} [get_ports lcd_rd_n]
set_property -dict {PACKAGE_PIN M17 IOSTANDARD LVCMOS33} [get_ports lcd_rst_n]
set_property -dict {PACKAGE_PIN F16 IOSTANDARD LVCMOS33} [get_ports lcd_bl]

# Conservative signal integrity for a short flying-wire test harness.
set_property DRIVE 8 [get_ports {lcd_data[*] lcd_cs_n lcd_rs lcd_wr_n lcd_rd_n lcd_rst_n lcd_bl}]
set_property SLEW SLOW [get_ports {lcd_data[*] lcd_cs_n lcd_rs lcd_wr_n lcd_rd_n lcd_rst_n lcd_bl}]

# Inactive levels are safe while the FPGA is configuring.
set_property PULLUP TRUE [get_ports {lcd_cs_n lcd_rd_n lcd_wr_n lcd_rst_n}]
set_property PULLDOWN TRUE [get_ports lcd_bl]

# This first design uses a deliberately slow, register-driven 8080 cycle.
# Vivado checks every internal path against the 50 MHz sys_clk.  External
# setup/hold is enforced structurally in lcd8080_write_cycle.v; a future faster
# controller should add a generated WR clock plus explicit output delays.

