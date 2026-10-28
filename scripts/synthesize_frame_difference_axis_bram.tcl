set script_dir [file dirname [file normalize [info script]]]
set project_dir [file normalize [file join $script_dir ..]]
set output_dir [file join $project_dir output diagnostics]
file mkdir $output_dir

read_verilog [file join $project_dir rtl frame_difference_axis_bram.v]
synth_design -top frame_difference_axis_bram -part xc7z020clg400-2

set bram_cells [get_cells -hier -filter {REF_NAME =~ RAMB*}]
set bram_count [llength $bram_cells]
set primitive_count [llength [get_cells -hier -filter {IS_PRIMITIVE}]]

report_utilization -file [file join $output_dir frame_difference_axis_bram_utilization.rpt]
write_checkpoint -force [file join $output_dir frame_difference_axis_bram_synth.dcp]

puts "BRAM inference check: RAMB primitive count = $bram_count"
puts "Total primitive count = $primitive_count"
if {$bram_count < 1} {
    error "No RAMB primitive was inferred; refusing to use this implementation."
}
