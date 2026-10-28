SUMMARY = "Studyboard V4L2 MMAP capture and integrity test"
SECTION = "PETALINUX/apps"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = "file://studyboard-v4l2-test.c \
           file://studyboard-v4l2-latest.c \
           file://studyboard-v4l2-preprocess.c \
           file://Makefile \
          "

S = "${WORKDIR}"

do_compile() {
    oe_runmake
}

do_install() {
    install -d ${D}${bindir}
    install -m 0755 studyboard-v4l2-test ${D}${bindir}
    install -m 0755 studyboard-v4l2-latest ${D}${bindir}
    install -m 0755 studyboard-v4l2-preprocess ${D}${bindir}
}
