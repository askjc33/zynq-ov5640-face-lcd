# Export completed Vivado 2018.3 implementation artifacts without rebuilding.

set script_dir [file dirname [file normalize [info script]]]
set repo_dir   [file dirname $script_dir]
set xpr        [file join $repo_dir build studyboard_frame_difference studyboard_frame_difference.xpr]
set impl_dir   [file join $repo_dir build studyboard_frame_difference \
    studyboard_frame_difference.runs impl_1]
set bit_file   [file join $impl_dir system_wrapper.bit]
set output_dir [file join $repo_dir output full]

if {![file exists $xpr]} {
    error "Project does not exist; run create_studyboard_project.tcl first"
}
if {![file exists $bit_file]} {
    error "Implemented bitstream does not exist; run build_studyboard_project.tcl first"
}

file mkdir $output_dir
open_project $xpr
open_run impl_1
file copy -force $bit_file [file join $output_dir studyboard_frame_difference.bit]
set hwdef_file [file join $output_dir system.hwdef]
write_hwdef -force $hwdef_file
write_sysdef -force -hwdef $hwdef_file -bitfile $bit_file \
    [file join $output_dir system.hdf]

puts "BITSTREAM: [file join $output_dir studyboard_frame_difference.bit]"
puts "HARDWARE:  [file join $output_dir system.hdf]"
exit
