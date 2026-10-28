set script_dir  [file dirname [file normalize [info script]]]
set repo_dir    [file dirname $script_dir]
if {![info exists project_name]} {
    set project_name studyboard_frame_difference_lcd
}
if {![info exists project_dir]} {
    set project_dir [file join $repo_dir build $project_name]
}
if {![info exists output_dir]} {
    set output_dir [file join $repo_dir output lcd_full]
}
if {![info exists export_xsa]} {
    # Vivado 2018.3 baseline: retain HDF output by default.
    set export_xsa 0
}
set xpr [file join $project_dir ${project_name}.xpr]

if {![file exists $xpr]} {
    error "LCD project does not exist; run create_studyboard_lcd_project.tcl first"
}

file mkdir $output_dir
open_project $xpr
open_bd_design [get_files system.bd]
update_compile_order -fileset sources_1
validate_bd_design
save_bd_design
generate_target all [get_files system.bd]
set_property STEPS.OPT_DESIGN.ARGS.DIRECTIVE NoBramPowerOpt [get_runs impl_1]
set_property STEPS.POWER_OPT_DESIGN.IS_ENABLED false [get_runs impl_1]
# Reset every OOC synthesis run as well as the top run: source edits must not
# silently reuse an older module checkpoint.
foreach synth_run [get_runs -filter {IS_SYNTHESIS == 1}] {
    reset_run $synth_run
}
launch_runs synth_1 -jobs 4
wait_on_run synth_1
if {[get_property STATUS [get_runs synth_1]] ne "synth_design Complete!"} {
    error "LCD-design synthesis failed"
}

launch_runs impl_1 -to_step write_bitstream -jobs 4
wait_on_run impl_1
if {[get_property STATUS [get_runs impl_1]] ne "write_bitstream Complete!"} {
    error "LCD-design implementation failed"
}

set bit_file [file join $project_dir \
    ${project_name}.runs impl_1 system_wrapper.bit]
file copy -force $bit_file [file join $output_dir ${project_name}.bit]

open_run impl_1
if {$export_xsa} {
    # PetaLinux 2020.2 consumes an XSA exported by Vivado 2020.2.  The
    # include-bit option packages the implemented system_wrapper.bit in it.
    set xsa_file [file join $output_dir ${project_name}.xsa]
    write_hw_platform -fixed -include_bit -force -file $xsa_file
    puts "XSA: $xsa_file"
} else {
    set hwdef_file [file join $output_dir system.hwdef]
    write_hwdef -force $hwdef_file
    write_sysdef -force -hwdef $hwdef_file -bitfile $bit_file \
        [file join $output_dir system.hdf]
}
report_timing_summary -file [file join $output_dir timing_summary.rpt]
report_utilization -file [file join $output_dir utilization.rpt]
puts "BUILD: [file join $output_dir ${project_name}.bit]"
exit
