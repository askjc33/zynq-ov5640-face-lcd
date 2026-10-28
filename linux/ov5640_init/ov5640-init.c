// SPDX-License-Identifier: MIT
// Configure OV5640 through Linux i2c-dev using the proven bare-metal table.

#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "ov5640-init-table.h"

#define OV5640_I2C_ADDRESS 0x3c
#define OV5640_CHIP_ID_HIGH 0x300a
#define OV5640_CHIP_ID_LOW  0x300b
#define OV5640_EXPECTED_ID  0x5640
#define OV5640_WRITE_RETRIES 10
#define OV5640_WRITE_RETRY_DELAY_US 5000
#define OV5640_INTER_WRITE_DELAY_US 1000

struct video_mode {
    uint16_t width;
    uint16_t height;
    uint16_t htotal;
    uint16_t vtotal;
};

static int read_register16(int fd, uint16_t reg, uint8_t *value)
{
    uint8_t address[2] = {
        (uint8_t)(reg >> 8),
        (uint8_t)(reg & 0xff),
    };
    struct i2c_msg messages[2] = {
        {
            .addr = OV5640_I2C_ADDRESS,
            .flags = 0,
            .len = sizeof(address),
            .buf = address,
        },
        {
            .addr = OV5640_I2C_ADDRESS,
            .flags = I2C_M_RD,
            .len = 1,
            .buf = value,
        },
    };
    struct i2c_rdwr_ioctl_data transaction = {
        .msgs = messages,
        .nmsgs = 2,
    };

    return ioctl(fd, I2C_RDWR, &transaction);
}

static int write_register16(int fd, uint16_t reg, uint8_t value)
{
    uint8_t payload[3] = {
        (uint8_t)(reg >> 8),
        (uint8_t)(reg & 0xff),
        value,
    };
    struct i2c_msg message = {
        .addr = OV5640_I2C_ADDRESS,
        .flags = 0,
        .len = sizeof(payload),
        .buf = payload,
    };
    struct i2c_rdwr_ioctl_data transaction = {
        .msgs = &message,
        .nmsgs = 1,
    };

    return ioctl(fd, I2C_RDWR, &transaction);
}

static int write_register16_retry(int fd, uint16_t reg, uint8_t value,
                                  unsigned int *attempts_used)
{
    unsigned int attempt;
    int saved_errno = EIO;

    for (attempt = 1; attempt <= OV5640_WRITE_RETRIES; ++attempt) {
        if (write_register16(fd, reg, value) >= 0) {
            if (attempts_used)
                *attempts_used = attempt;
            return 0;
        }

        saved_errno = errno;
        if (attempt < OV5640_WRITE_RETRIES)
            usleep(OV5640_WRITE_RETRY_DELAY_US);
    }

    if (attempts_used)
        *attempts_used = OV5640_WRITE_RETRIES;
    errno = saved_errno;
    return -1;
}

static int read_chip_id(int fd, uint16_t *chip_id)
{
    uint8_t high;
    uint8_t low;

    if (read_register16(fd, OV5640_CHIP_ID_HIGH, &high) < 0)
        return -1;
    if (read_register16(fd, OV5640_CHIP_ID_LOW, &low) < 0)
        return -1;

    *chip_id = ((uint16_t)high << 8) | low;
    return 0;
}

static int parse_u16(const char *text, uint16_t *value)
{
    char *end = NULL;
    unsigned long parsed;

    errno = 0;
    parsed = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' ||
        parsed == 0 || parsed > 0xffff)
        return -1;

    *value = (uint16_t)parsed;
    return 0;
}

static uint8_t resolve_value(uint16_t value_or_source,
                             const struct video_mode *mode)
{
    if (value_or_source <= 0xff)
        return (uint8_t)value_or_source;

    switch (value_or_source) {
    case OV5640_VALUE_WIDTH_HI:
        return (uint8_t)(mode->width >> 8);
    case OV5640_VALUE_WIDTH_LO:
        return (uint8_t)(mode->width & 0xff);
    case OV5640_VALUE_HEIGHT_HI:
        return (uint8_t)(mode->height >> 8);
    case OV5640_VALUE_HEIGHT_LO:
        return (uint8_t)(mode->height & 0xff);
    case OV5640_VALUE_HTOTAL_HI:
        return (uint8_t)(mode->htotal >> 8);
    case OV5640_VALUE_HTOTAL_LO:
        return (uint8_t)(mode->htotal & 0xff);
    case OV5640_VALUE_VTOTAL_HI:
        return (uint8_t)(mode->vtotal >> 8);
    case OV5640_VALUE_VTOTAL_LO:
        return (uint8_t)(mode->vtotal & 0xff);
    default:
        return 0;
    }
}

static int read_pair(int fd, uint16_t high_reg, uint16_t low_reg,
                     uint16_t *value)
{
    uint8_t high;
    uint8_t low;

    if (read_register16(fd, high_reg, &high) < 0)
        return -1;
    if (read_register16(fd, low_reg, &low) < 0)
        return -1;

    *value = ((uint16_t)high << 8) | low;
    return 0;
}

static int verify_register(int fd, uint16_t reg, uint8_t expected,
                           const char *name)
{
    uint8_t actual;

    if (read_register16(fd, reg, &actual) < 0) {
        fprintf(stderr, "Verification read %s (0x%04x) failed: %s\n",
                name, reg, strerror(errno));
        return -1;
    }

    printf("  %-13s reg=0x%04x expected=0x%02x actual=0x%02x %s\n",
           name, (unsigned int)reg, (unsigned int)expected,
           (unsigned int)actual,
           (actual == expected) ? "PASS" : "MISMATCH");
    return (actual == expected) ? 0 : -1;
}

static int verify_mode(int fd, const struct video_mode *expected)
{
    struct video_mode actual;
    int failures = 0;

    if (read_pair(fd, 0x3808, 0x3809, &actual.width) < 0 ||
        read_pair(fd, 0x380a, 0x380b, &actual.height) < 0 ||
        read_pair(fd, 0x380c, 0x380d, &actual.htotal) < 0 ||
        read_pair(fd, 0x380e, 0x380f, &actual.vtotal) < 0) {
        fprintf(stderr, "Mode register readback failed: %s\n",
                strerror(errno));
        return -1;
    }

    printf("  active size   expected=%ux%u actual=%ux%u %s\n",
           (unsigned int)expected->width, (unsigned int)expected->height,
           (unsigned int)actual.width, (unsigned int)actual.height,
           (actual.width == expected->width &&
            actual.height == expected->height) ? "PASS" : "MISMATCH");
    printf("  frame totals  expected=%ux%u actual=%ux%u %s\n",
           (unsigned int)expected->htotal, (unsigned int)expected->vtotal,
           (unsigned int)actual.htotal, (unsigned int)actual.vtotal,
           (actual.htotal == expected->htotal &&
            actual.vtotal == expected->vtotal) ? "PASS" : "MISMATCH");

    if (actual.width != expected->width || actual.height != expected->height)
        ++failures;
    if (actual.htotal != expected->htotal || actual.vtotal != expected->vtotal)
        ++failures;

    return failures ? -1 : 0;
}

static void print_usage(const char *program)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s\n"
            "  %s DEVICE\n"
            "  %s DEVICE WIDTH HEIGHT HTOTAL VTOTAL\n"
            "\n"
            "Defaults: DEVICE=/dev/i2c-0, mode=640x480, totals=2570x980.\n",
            program, program, program);
}

int main(int argc, char **argv)
{
    const char *device = "/dev/i2c-0";
    struct video_mode mode = {
        .width = 640,
        .height = 480,
        .htotal = 2570,
        .vtotal = 980,
    };
    uint16_t chip_id;
    size_t index;
    int fd;
    int verification_failed = 0;

    if (argc == 2) {
        device = argv[1];
    } else if (argc == 6) {
        device = argv[1];
        if (parse_u16(argv[2], &mode.width) < 0 ||
            parse_u16(argv[3], &mode.height) < 0 ||
            parse_u16(argv[4], &mode.htotal) < 0 ||
            parse_u16(argv[5], &mode.vtotal) < 0) {
            fprintf(stderr, "Invalid non-zero 16-bit mode value.\n");
            return 2;
        }
    } else if (argc != 1) {
        print_usage(argv[0]);
        return 2;
    }

    if (mode.htotal < mode.width || mode.vtotal < mode.height) {
        fprintf(stderr,
                "Invalid timing: totals must not be smaller than active size.\n");
        return 2;
    }

    printf("OV5640 Linux initialization\n");
    printf("  bus/address: %s / 0x%02x\n", device, OV5640_I2C_ADDRESS);
    printf("  active size: %ux%u\n",
           (unsigned int)mode.width, (unsigned int)mode.height);
    printf("  frame totals: %ux%u\n",
           (unsigned int)mode.htotal, (unsigned int)mode.vtotal);
    printf("  table writes: %u\n", (unsigned int)OV5640_INIT_SEQUENCE_COUNT);

    fd = open(device, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Cannot open %s: %s\n", device, strerror(errno));
        return 1;
    }

    usleep(20000);
    if (read_chip_id(fd, &chip_id) < 0) {
        fprintf(stderr, "Chip ID read failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    printf("  chip ID: 0x%04x (expected 0x%04x)\n",
           (unsigned int)chip_id, (unsigned int)OV5640_EXPECTED_ID);
    if (chip_id != OV5640_EXPECTED_ID) {
        fprintf(stderr, "Unexpected sensor ID; initialization aborted.\n");
        close(fd);
        return 3;
    }

    for (index = 0; index < OV5640_INIT_SEQUENCE_COUNT; ++index) {
        const struct ov5640_register_write *entry =
                &ov5640_init_sequence[index];
        uint8_t value = resolve_value(entry->value_or_source, &mode);
        unsigned int attempts_used = 0;

        if (write_register16_retry(fd, entry->reg, value,
                                   &attempts_used) < 0) {
            fprintf(stderr,
                    "Write %u/%u failed after %u attempts: "
                    "reg=0x%04x value=0x%02x: %s\n",
                    (unsigned int)(index + 1),
                    (unsigned int)OV5640_INIT_SEQUENCE_COUNT,
                    attempts_used,
                    (unsigned int)entry->reg, (unsigned int)value,
                    strerror(errno));
            close(fd);
            return 4;
        }

        if (attempts_used > 1) {
            fprintf(stderr,
                    "  write %u/%u recovered on attempt %u: reg=0x%04x\n",
                    (unsigned int)(index + 1),
                    (unsigned int)OV5640_INIT_SEQUENCE_COUNT,
                    attempts_used, (unsigned int)entry->reg);
        }

        if (entry->delay_after_us) {
            usleep(entry->delay_after_us);
        } else {
            /*
             * OV5640 uses SCCB and may briefly NAK immediately after clock
             * tree or analogue-register changes.  The original GPIO
             * bit-banged implementation was naturally slower and ignored
             * ACK; keep Linux transactions reliable without hiding a
             * persistent failure.
             */
            usleep(OV5640_INTER_WRITE_DELAY_US);
        }

        if ((index + 1) % 50 == 0 ||
            index + 1 == OV5640_INIT_SEQUENCE_COUNT) {
            printf("  configured %u/%u registers\n",
                   (unsigned int)(index + 1),
                   (unsigned int)OV5640_INIT_SEQUENCE_COUNT);
        }
    }

    usleep(100000);
    puts("Key-register readback:");
    verification_failed |= verify_register(fd, 0x3008, 0x02, "system ctrl") < 0;
    verification_failed |= verify_register(fd, 0x3035, 0x11, "clock div") < 0;
    verification_failed |= verify_register(fd, 0x3036, 0x5a, "PLL mult") < 0;
    verification_failed |= verify_register(fd, 0x4300, 0x61, "RGB565 fmt") < 0;
    verification_failed |= verify_register(fd, 0x501f, 0x01, "ISP RGB") < 0;
    verification_failed |= verify_register(fd, 0x503d, 0x00, "test pattern") < 0;
    verification_failed |= verify_mode(fd, &mode) < 0;

    close(fd);
    if (verification_failed) {
        fprintf(stderr, "OV5640 initialization completed with readback errors.\n");
        return 5;
    }

    puts("OV5640 initialization and readback verification successful.");
    puts("The sensor now streams RGB565 DVP; VDMA is not started by this tool.");
    return 0;
}
