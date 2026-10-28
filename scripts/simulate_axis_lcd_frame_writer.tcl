set script_dir [file dirname [file normalize [info script]]]
set repo_dir   [file dirname $script_dir]
set sim_dir    [file join $repo_dir build sim_axis_lcd_frame_writer]

create_project -force sim_axis_lcd_frame_writer $sim_dir -part xc7z020clg400-2
set_property target_language Verilog [current_project]

add_files [list \
    [file join $repo_dir bringup atk_md0430_colorbar lcd8080_write_cycle.v] \
    [file join $repo_dir rtl axis_lcd_frame_writer.v]]
add_files -fileset sim_1 [file join $repo_dir bringup atk_md0430_colorbar tb_axis_lcd_frame_writer.v]
set_property top tb_axis_lcd_frame_writer [get_filesets sim_1]

launch_simulation -simset sim_1 -mode behavioral
run all
close_sim
exit
