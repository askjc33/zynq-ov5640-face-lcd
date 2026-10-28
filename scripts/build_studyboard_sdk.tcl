# Build the standalone OV5640 application with Xilinx SDK/XSCT 2018.3.

set script_dir [file dirname [file normalize [info script]]]
set repo_dir   [file dirname $script_dir]
set hdf        [file join $repo_dir output full system.hdf]
set workspace  [file join $repo_dir build sdk_hp1_workspace]
set source_dir [file join $repo_dir SDK ov5640_hdmi_frame_difference src]

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
    error "Hardware platform not found; export output/full/system.hdf first"
}

setws $workspace

if {[lsearch -exact [getprojects] hw_platform_0] < 0} {
    createhw -name hw_platform_0 -hwspec $hdf
}
if {[lsearch -exact [getprojects] standalone_bsp_0] < 0} {
    createbsp -name standalone_bsp_0 -hwproject hw_platform_0 \
        -proc ps7_cortexa9_0 -os standalone
}
if {[lsearch -exact [getprojects] ov5640_frame_difference] < 0} {
    createapp -name ov5640_frame_difference -hwproject hw_platform_0 \
        -proc ps7_cortexa9_0 -os standalone -lang C \
        -app {Empty Application} -bsp standalone_bsp_0
}
copy_source_tree $source_dir \
    [file join $workspace ov5640_frame_difference src]

projects -build
set elf_file [file join $workspace ov5640_frame_difference Debug ov5640_frame_difference.elf]
if {![file exists $elf_file]} {
    error "SDK build did not produce the expected ELF"
}
set output_dir [file join $repo_dir output full]
file mkdir $output_dir
file copy -force $elf_file [file join $output_dir ov5640_frame_difference.elf]
puts "ELF: [file join $output_dir ov5640_frame_difference.elf]"
exit
