set script_dir [file dirname [file normalize [info script]]]
set repo_dir   [file dirname $script_dir]
set project_dir [file join $repo_dir build studyboard_frame_difference_hp1]
set xpr        [file join $project_dir studyboard_frame_difference.xpr]
set output_dir [file join $repo_dir output full]

if {![file exists $xpr]} {
    error "Project does not exist; run create_studyboard_project.tcl first"
}

file mkdir $output_dir
open_project $xpr
reset_run synth_1
launch_runs synth_1 -jobs 4
wait_on_run synth_1
if {[get_property STATUS [get_runs synth_1]] ne "synth_design Complete!"} {
    error "Full-design synthesis failed"
}

launch_runs impl_1 -to_step write_bitstream -jobs 4
wait_on_run impl_1
if {[get_property STATUS [get_runs impl_1]] ne "write_bitstream Complete!"} {
    error "Full-design implementation failed"
}

set bit_file [file join $project_dir \
    studyboard_frame_difference.runs impl_1 system_wrapper.bit]
file copy -force $bit_file [file join $output_dir studyboard_frame_difference.bit]

set hwdef_file [file join $output_dir system.hwdef]
write_hwdef -force $hwdef_file
write_sysdef -force -hwdef $hwdef_file -bitfile $bit_file \
    [file join $output_dir system.hdf]
puts "BUILD: [file join $output_dir studyboard_frame_difference.bit]"
exit
