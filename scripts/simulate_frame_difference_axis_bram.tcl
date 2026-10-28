set script_dir [file dirname [file normalize [info script]]]
set project_dir [file normalize [file join $script_dir ..]]
set sim_dir [file join $project_dir build sim_frame_difference_axis_bram]

file mkdir $sim_dir
cd $sim_dir

set rtl_file [file join $project_dir rtl frame_difference_axis_bram.v]
set tb_file  [file join $project_dir bringup atk_md0430_colorbar tb_frame_difference_axis_bram.v]

exec xvlog $rtl_file $tb_file
exec xelab tb_frame_difference_axis_bram -s tb_frame_difference_axis_bram_sim
set simulation_output [exec xsim tb_frame_difference_axis_bram_sim -runall]
puts $simulation_output
if {[string first "PASS:" $simulation_output] < 0 ||
    [string first "FAIL:" $simulation_output] >= 0} {
    error "Frame-difference behavior simulation did not pass"
}

puts "Frame-difference BRAM simulation completed."
