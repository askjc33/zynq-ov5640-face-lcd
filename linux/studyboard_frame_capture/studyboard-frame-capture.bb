SUMMARY = "StudyBoard OV5640 staged capture/display VDMA client"
SECTION = "PETALINUX/modules"
LICENSE = "GPLv2"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/GPL-2.0;md5=801f80980d171dd6425610833a22dbe6"

inherit module

SRC_URI = "file://Makefile \
           file://studyboard-frame-capture.c \
          "

S = "${WORKDIR}"
