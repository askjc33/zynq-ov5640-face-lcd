# Vivado 2020.2 build entry point.
#
# Build the separate migration project and export an XSA containing the
# implemented bitstream for PetaLinux 2020.2.  Do not run this from Vivado
# 2018.3: write_hw_platform is a Vivado 2020.2 XSA command.

set wrapper_dir  [file dirname [file normalize [info script]]]
set wrapper_repo [file dirname $wrapper_dir]

set project_name lcd20
set project_dir  [file join $wrapper_repo build v20]
set output_dir   [file join $wrapper_repo output lcd_2020_2]
set export_xsa   1

puts "Building Vivado 2020.2 project: $project_dir"
source [file join $wrapper_dir build_studyboard_lcd_project.tcl]
