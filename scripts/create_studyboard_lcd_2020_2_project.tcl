# Safe Vivado 2020.2 migration entry point.
#
# It reuses the source-rebuildable LCD design, but writes a separate project.
# The Vivado 2018.3 project and its known-good bitstream are never opened or
# overwritten by this script.

set wrapper_dir  [file dirname [file normalize [info script]]]
set wrapper_repo [file dirname $wrapper_dir]

# Keep the generated project path short. Vivado 2020.2 OOC synthesis creates
# deeply nested .Xil paths and fails on Windows when the full path exceeds
# 260 characters. The descriptive output directory is retained separately.
set project_name lcd20
set project_dir  [file join $wrapper_repo build v20]
set export_hdf   0

puts "Creating isolated Vivado 2020.2 project: $project_dir"
source [file join $wrapper_dir create_studyboard_lcd_project.tcl]
