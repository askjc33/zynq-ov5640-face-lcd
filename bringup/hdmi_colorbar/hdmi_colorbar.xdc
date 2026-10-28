# STUDYBOARD PL clock and reset key
set_property -dict {PACKAGE_PIN U18 IOSTANDARD LVCMOS33} [get_ports sys_clk]
set_property -dict {PACKAGE_PIN J15 IOSTANDARD LVCMOS33 PULLUP true} [get_ports sys_rst_n]
create_clock -period 20.000 -name sys_clk [get_ports sys_clk]

# STUDYBOARD HDMI output
set_property -dict {PACKAGE_PIN J18 IOSTANDARD TMDS_33} [get_ports TMDS_tmds_clk_p]
set_property -dict {PACKAGE_PIN H18 IOSTANDARD TMDS_33} [get_ports TMDS_tmds_clk_n]

set_property -dict {PACKAGE_PIN G19 IOSTANDARD TMDS_33} [get_ports {TMDS_tmds_data_p[0]}]
set_property -dict {PACKAGE_PIN G20 IOSTANDARD TMDS_33} [get_ports {TMDS_tmds_data_n[0]}]
set_property -dict {PACKAGE_PIN K19 IOSTANDARD TMDS_33} [get_ports {TMDS_tmds_data_p[1]}]
set_property -dict {PACKAGE_PIN J19 IOSTANDARD TMDS_33} [get_ports {TMDS_tmds_data_n[1]}]
set_property -dict {PACKAGE_PIN J20 IOSTANDARD TMDS_33} [get_ports {TMDS_tmds_data_p[2]}]
set_property -dict {PACKAGE_PIN H20 IOSTANDARD TMDS_33} [get_ports {TMDS_tmds_data_n[2]}]

