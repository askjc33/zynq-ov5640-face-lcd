# Build the LCD standalone application with Xilinx SDK/XSCT 2018.3.

set script_dir     [file dirname [file normalize [info script]]]
set repo_dir       [file dirname $script_dir]
set hdf            [file join $repo_dir output lcd_full system.hdf]
set workspace      [file join $repo_dir build sdk_lcd_workspace]
set common_sources [file join $repo_dir SDK ov5640_hdmi_frame_difference src]
set lcd_main       [file join $repo_dir SDK ov5640_lcd_frame_difference main.c]
set output_dir     [file join $repo_dir output lcd_full]

proc copy_source_tree {source destination} {
    file mkdir $destination
    foreach item [glob -nocomplain -directory $source *] {
        set target [file join $destination [file tail $item]]
        if {[file isdirectory $item]} {
            copy_source_tree $item $target
        } else {
            file copy -force $item $target
        }
    }
}

if {![file exists $hdf]} {
    error "LCD hardware platform not found; build_studyboard_lcd_project.tcl first"
}

setws $workspace
if {[lsearch -exact [getprojects] hw_platform_lcd] < 0} {
    createhw -name hw_platform_lcd -hwspec $hdf
}
if {[lsearch -exact [getprojects] standalone_bsp_lcd] < 0} {
    createbsp -name standalone_bsp_lcd -hwproject hw_platform_lcd \
        -proc ps7_cortexa9_0 -os standalone
}
if {[lsearch -exact [getprojects] ov5640_lcd_frame_difference] < 0} {
    createapp -name ov5640_lcd_frame_difference -hwproject hw_platform_lcd \
        -proc ps7_cortexa9_0 -os standalone -lang C \
        -app {Empty Application} -bsp standalone_bsp_lcd
}

set app_src [file join $workspace ov5640_lcd_frame_difference src]
foreach source_dir {vdma_api emio_sccb_cfg ov5640} {
    copy_source_tree [file join $common_sources $source_dir] [file join $app_src $source_dir]
}
file copy -force [file join $common_sources lscript.ld] [file join $app_src lscript.ld]
file copy -force $lcd_main [file join $app_src main.c]

projects -build
set elf_file [file join $workspace ov5640_lcd_frame_difference Debug \
    ov5640_lcd_frame_difference.elf]
if {![file exists $elf_file]} {
    error "SDK build did not produce the expected LCD ELF"
}
file mkdir $output_dir
file copy -force $elf_file [file join $output_dir ov5640_lcd_frame_difference.elf]
puts "ELF: [file join $output_dir ov5640_lcd_frame_difference.elf]"
exit
