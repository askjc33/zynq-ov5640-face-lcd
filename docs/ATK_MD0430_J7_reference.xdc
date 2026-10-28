# Wiring reference: STUDYBOARD J7 -> ATK-MD0430 V2.0 34-pin LCD header.
# Not added to the existing HDMI project. Add only to a top with these ports.
# Sources: board schematic 2024-06-21 pp. 3,6,11; LCD V2.0 schematic 2023-12-06.
# Bank 35 is powered at 3.3 V. LCD connector pin numbers are NOT FPGA ball names.

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

set_property -dict {PACKAGE_PIN L16 IOSTANDARD LVCMOS33} [get_ports lcd_cs_n]
set_property -dict {PACKAGE_PIN K14 IOSTANDARD LVCMOS33} [get_ports lcd_rs]
set_property -dict {PACKAGE_PIN L17 IOSTANDARD LVCMOS33} [get_ports lcd_wr_n]
set_property -dict {PACKAGE_PIN J14 IOSTANDARD LVCMOS33} [get_ports lcd_rd_n]
set_property -dict {PACKAGE_PIN M17 IOSTANDARD LVCMOS33} [get_ports lcd_rst_n]
set_property -dict {PACKAGE_PIN F16 IOSTANDARD LVCMOS33} [get_ports lcd_bl]

# Conservative initial output strength/slew; verify the actual LCD WR/data timing.
set_property DRIVE 8 [get_ports {lcd_data[*] lcd_cs_n lcd_rs lcd_wr_n lcd_rd_n lcd_rst_n lcd_bl}]
set_property SLEW SLOW [get_ports {lcd_data[*] lcd_cs_n lcd_rs lcd_wr_n lcd_rd_n lcd_rst_n lcd_bl}]
# CS/RD/WR are inactive high; BL has a board-level pulldown on the LCD module.
set_property PULLUP TRUE [get_ports {lcd_cs_n lcd_rd_n lcd_wr_n}]

# Optional capacitive touch. Enable only after adding the corresponding ports.
# set_property -dict {PACKAGE_PIN M18 IOSTANDARD LVCMOS33} [get_ports touch_scl]
# set_property -dict {PACKAGE_PIN F17 IOSTANDARD LVCMOS33} [get_ports touch_sda]
# set_property -dict {PACKAGE_PIN K16 IOSTANDARD LVCMOS33} [get_ports touch_int]
# set_property -dict {PACKAGE_PIN E19 IOSTANDARD LVCMOS33} [get_ports touch_rst_n]

# Pin constraints only: add timing constraints with the implemented 8080 controller.
# An LCD read cycle must tristate lcd_data before asserting lcd_rd_n.
