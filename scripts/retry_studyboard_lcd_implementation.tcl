# Resume from a successful synthesis after Vivado 2018.3 power_opt_design
# crashes.  This avoids repeating the long frame-difference OOC synthesis.

set script_dir  [file dirname [file normalize [info script]]]
set repo_dir    [file dirname $script_dir]
set project_dir [file join $repo_dir build studyboard_frame_difference_lcd]
set xpr         [file join $project_dir studyboard_frame_difference_lcd.xpr]
set output_dir  [file join $repo_dir output lcd_full]

open_project $xpr
if {[get_property STATUS [get_runs synth_1]] ne "synth_design Complete!"} {
    error "Synthesis is not complete; run build_studyboard_lcd_project.tcl"
}

set_property STEPS.OPT_DESIGN.ARGS.DIRECTIVE NoBramPowerOpt [get_runs impl_1]
set_property STEPS.POWER_OPT_DESIGN.IS_ENABLED false [get_runs impl_1]
reset_run impl_1
launch_runs impl_1 -to_step write_bitstream -jobs 4
wait_on_run impl_1
if {[get_property STATUS [get_runs impl_1]] ne "write_bitstream Complete!"} {
    error "LCD-design implementation retry failed"
}

file mkdir $output_dir
set bit_file [file join $project_dir \
    studyboard_frame_difference_lcd.runs impl_1 system_wrapper.bit]
file copy -force $bit_file [file join $output_dir studyboard_frame_difference_lcd.bit]
set hwdef_file [file join $output_dir system.hwdef]
write_hwdef -force $hwdef_file
write_sysdef -force -hwdef $hwdef_file -bitfile $bit_file \
    [file join $output_dir system.hdf]

open_run impl_1
report_timing_summary -file [file join $output_dir timing_summary.rpt]
report_utilization -file [file join $output_dir utilization.rpt]
puts "BUILD: [file join $output_dir studyboard_frame_difference_lcd.bit]"
exit
