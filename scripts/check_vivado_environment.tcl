puts "CHECK: Vivado [version -short]"

create_project -in_memory -part xc7z020clg400-1
update_ip_catalog

set required_parts {
    xc7z020clg400-1
    xc7z020clg400-2
}

foreach part_name $required_parts {
    if {[llength [get_parts -quiet $part_name]] == 1} {
        puts "CHECK: part $part_name available"
    } else {
        puts "ERROR: part $part_name unavailable"
    }
}

set required_ip_patterns {
    xilinx.com:ip:processing_system7:*
    xilinx.com:ip:axi_vdma:*
    xilinx.com:ip:v_vid_in_axi4s:*
    xilinx.com:ip:v_axi4s_vid_out:*
    xilinx.com:ip:v_tc:*
    xilinx.com:ip:fifo_generator:*
    xilinx.com:ip:proc_sys_reset:*
    xilinx.com:ip:axis_data_fifo:*
}

foreach ip_pattern $required_ip_patterns {
    set matches [get_ipdefs -all -quiet $ip_pattern]
    if {[llength $matches] > 0} {
        puts "CHECK: IP $ip_pattern -> [lindex $matches end]"
    } else {
        puts "ERROR: IP $ip_pattern unavailable"
    }
}

exit
