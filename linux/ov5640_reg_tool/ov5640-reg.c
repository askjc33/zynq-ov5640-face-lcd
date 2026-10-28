// SPDX-License-Identifier: MIT
// Read or write one OV5640 register through Linux i2c-dev.

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

#define OV5640_I2C_ADDRESS 0x3c

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

static int parse_value(const char *text, unsigned long maximum,
                       unsigned long *value)
{
    char *end = NULL;
    unsigned long parsed;

    errno = 0;
    parsed = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || parsed > maximum)
        return -1;

    *value = parsed;
    return 0;
}

static void print_usage(const char *program)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s /dev/i2c-N read  REGISTER\n"
            "  %s /dev/i2c-N write REGISTER VALUE\n"
            "\n"
            "REGISTER is 0..0xffff and VALUE is 0..0xff.\n"
            "Decimal and 0x-prefixed hexadecimal numbers are accepted.\n",
            program, program);
}

int main(int argc, char **argv)
{
    const char *device;
    const char *operation;
    unsigned long parsed_register;
    unsigned long parsed_value = 0;
    uint16_t reg;
    uint8_t value;
    int fd;
    int result = 1;

    if (argc != 4 && argc != 5) {
        print_usage(argv[0]);
        return 2;
    }

    device = argv[1];
    operation = argv[2];
    if (parse_value(argv[3], 0xffff, &parsed_register) < 0) {
        fprintf(stderr, "Invalid register address: %s\n", argv[3]);
        return 2;
    }
    reg = (uint16_t)parsed_register;

    if (strcmp(operation, "read") == 0) {
        if (argc != 4) {
            print_usage(argv[0]);
            return 2;
        }
    } else if (strcmp(operation, "write") == 0) {
        if (argc != 5 || parse_value(argv[4], 0xff, &parsed_value) < 0) {
            fprintf(stderr, "Invalid 8-bit value: %s\n",
                    (argc == 5) ? argv[4] : "<missing>");
            return 2;
        }
    } else {
        fprintf(stderr, "Unknown operation: %s\n", operation);
        print_usage(argv[0]);
        return 2;
    }

    fd = open(device, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Cannot open %s: %s\n", device, strerror(errno));
        return 1;
    }

    if (strcmp(operation, "read") == 0) {
        if (read_register16(fd, reg, &value) < 0) {
            fprintf(stderr, "Read register 0x%04x failed: %s\n",
                    reg, strerror(errno));
            goto out;
        }

        printf("OV5640[0x%04x] = 0x%02x\n", reg, value);
        result = 0;
        goto out;
    }

    value = (uint8_t)parsed_value;
    if (write_register16(fd, reg, value) < 0) {
        fprintf(stderr, "Write register 0x%04x failed: %s\n",
                reg, strerror(errno));
        goto out;
    }

    if (read_register16(fd, reg, &value) < 0) {
        fprintf(stderr,
                "Register 0x%04x was written, but readback failed: %s\n",
                reg, strerror(errno));
        goto out;
    }

    printf("OV5640[0x%04x] wrote 0x%02lx, read back 0x%02x: %s\n",
           reg, parsed_value, value,
           (value == (uint8_t)parsed_value) ? "PASS" : "MISMATCH");
    result = (value == (uint8_t)parsed_value) ? 0 : 3;

out:
    close(fd);
    return result;
}
