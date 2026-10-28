set script_dir  [file dirname [file normalize [info script]]]
set repo_dir    [file dirname $script_dir]
set project_dir [file join $repo_dir build hdmi_colorbar]
set output_dir  [file join $repo_dir output bit]

file mkdir $project_dir
file mkdir $output_dir

create_project -force studyboard_hdmi_colorbar $project_dir -part xc7z020clg400-2

set dvi_src_dir [file join $repo_dir ip_repo DVI_TX ip src]
add_files [list \
    [file join $dvi_src_dir asyn_rst_syn.v] \
    [file join $dvi_src_dir dvi_encoder.v] \
    [file join $dvi_src_dir serializer_10_to_1.v] \
    [file join $dvi_src_dir dvi_transmitter_top.v] \
    [file join $repo_dir bringup hdmi_colorbar hdmi_colorbar_top.v]]

add_files -fileset constrs_1 \
    [file join $repo_dir bringup hdmi_colorbar hdmi_colorbar.xdc]

set_property top hdmi_colorbar_top [current_fileset]

create_ip -name clk_wiz -vendor xilinx.com -library ip -version 6.0 \
    -module_name hdmi_clk_wiz
set_property -dict [list \
    CONFIG.PRIM_IN_FREQ {50.000} \
    CONFIG.CLKOUT1_REQUESTED_OUT_FREQ {25.000} \
    CONFIG.CLKOUT2_USED {true} \
    CONFIG.CLKOUT2_REQUESTED_OUT_FREQ {125.000} \
    CONFIG.USE_RESET {true} \
    CONFIG.RESET_TYPE {ACTIVE_LOW}] [get_ips hdmi_clk_wiz]
generate_target all [get_ips hdmi_clk_wiz]

update_compile_order -fileset sources_1

launch_runs synth_1 -jobs 4
wait_on_run synth_1
if {[get_property STATUS [get_runs synth_1]] ne "synth_design Complete!"} {
    error "HDMI color-bar synthesis failed"
}

launch_runs impl_1 -to_step write_bitstream -jobs 4
wait_on_run impl_1
if {[get_property STATUS [get_runs impl_1]] ne "write_bitstream Complete!"} {
    error "HDMI color-bar implementation failed"
}

set bit_file [file join $project_dir studyboard_hdmi_colorbar.runs impl_1 hdmi_colorbar_top.bit]
if {![file exists $bit_file]} {
    error "Expected bitstream was not generated: $bit_file"
}

file copy -force $bit_file [file join $output_dir studyboard_hdmi_colorbar.bit]
puts "BUILD: bitstream [file join $output_dir studyboard_hdmi_colorbar.bit]"
exit
