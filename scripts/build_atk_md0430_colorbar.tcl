# Build the isolated ATK-MD0430 V2.0 color-bar bring-up design in Vivado 2018.3.
# The script creates a separate project and never modifies the working HDMI
# frame-difference project.

set script_dir  [file dirname [file normalize [info script]]]
set repo_dir    [file dirname $script_dir]
set project_dir [file join $repo_dir build atk_md0430_colorbar]
set output_dir  [file join $repo_dir output bit]

file mkdir $project_dir
file mkdir $output_dir

create_project -force studyboard_atk_md0430_colorbar $project_dir \
    -part xc7z020clg400-2
set_property target_language Verilog [current_project]

add_files [list \
    [file join $repo_dir bringup atk_md0430_colorbar lcd8080_write_cycle.v] \
    [file join $repo_dir bringup atk_md0430_colorbar atk_md0430_colorbar_top.v]]
add_files -fileset constrs_1 \
    [file join $repo_dir bringup atk_md0430_colorbar atk_md0430_colorbar.xdc]

set_property top atk_md0430_colorbar_top [current_fileset]
update_compile_order -fileset sources_1

launch_runs synth_1 -jobs 4
wait_on_run synth_1
if {[get_property STATUS [get_runs synth_1]] ne "synth_design Complete!"} {
    error "ATK-MD0430 color-bar synthesis failed"
}

launch_runs impl_1 -to_step write_bitstream -jobs 4
wait_on_run impl_1
if {[get_property STATUS [get_runs impl_1]] ne "write_bitstream Complete!"} {
    error "ATK-MD0430 color-bar implementation failed"
}

set bit_file [file join $project_dir \
    studyboard_atk_md0430_colorbar.runs impl_1 atk_md0430_colorbar_top.bit]
if {![file exists $bit_file]} {
    error "Expected bitstream was not generated: $bit_file"
}

file copy -force $bit_file \
    [file join $output_dir studyboard_atk_md0430_colorbar.bit]

open_run impl_1
report_timing_summary -file \
    [file join $output_dir studyboard_atk_md0430_colorbar_timing.rpt]
report_utilization -file \
    [file join $output_dir studyboard_atk_md0430_colorbar_utilization.rpt]

puts "BUILD: [file join $output_dir studyboard_atk_md0430_colorbar.bit]"
puts "REPORT: [file join $output_dir studyboard_atk_md0430_colorbar_timing.rpt]"
puts "REPORT: [file join $output_dir studyboard_atk_md0430_colorbar_utilization.rpt]"
exit
