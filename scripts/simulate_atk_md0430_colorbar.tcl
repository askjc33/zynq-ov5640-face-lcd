# Behavioral verification of the LCD write protocol and color-bar pixel count.
set script_dir  [file dirname [file normalize [info script]]]
set repo_dir    [file dirname $script_dir]
set sim_dir     [file join $repo_dir build atk_md0430_colorbar_sim]

create_project -force studyboard_atk_md0430_colorbar_sim $sim_dir \
    -part xc7z020clg400-2

add_files [list \
    [file join $repo_dir bringup atk_md0430_colorbar lcd8080_write_cycle.v] \
    [file join $repo_dir bringup atk_md0430_colorbar atk_md0430_colorbar_top.v]]
add_files -fileset sim_1 \
    [file join $repo_dir bringup atk_md0430_colorbar tb_atk_md0430_colorbar.v]

set_property top tb_atk_md0430_colorbar [get_filesets sim_1]
update_compile_order -fileset sources_1
update_compile_order -fileset sim_1

launch_simulation -mode behavioral
run all
close_sim
exit
