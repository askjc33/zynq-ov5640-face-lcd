// SPDX-License-Identifier: MIT
// Read the OV5640 chip ID through Linux i2c-dev and the i2c-gpio SCCB bus.

#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define OV5640_I2C_ADDRESS 0x3c
#define OV5640_CHIP_ID_HIGH 0x300a
#define OV5640_CHIP_ID_LOW  0x300b

static int read_register16(int fd, uint16_t reg, uint8_t *value)
{
    uint8_t register_address[2] = {
        (uint8_t)(reg >> 8),
        (uint8_t)(reg & 0xff)
    };
    struct i2c_msg messages[2] = {
        {
            .addr = OV5640_I2C_ADDRESS,
            .flags = 0,
            .len = sizeof(register_address),
            .buf = register_address,
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

int main(int argc, char **argv)
{
    const char *device = (argc > 1) ? argv[1] : "/dev/i2c-0";
    uint8_t id_high = 0;
    uint8_t id_low = 0;
    unsigned int attempt;
    int fd;

    fd = open(device, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Cannot open %s: %s\n", device, strerror(errno));
        return 1;
    }

    printf("OV5640 SCCB test: bus=%s address=0x%02x\n",
           device, OV5640_I2C_ADDRESS);

    for (attempt = 1; attempt <= 5; ++attempt) {
        usleep(20000);

        if (read_register16(fd, OV5640_CHIP_ID_HIGH, &id_high) < 0) {
            fprintf(stderr,
                    "Attempt %u: register 0x%04x read failed: %s\n",
                    attempt, OV5640_CHIP_ID_HIGH, strerror(errno));
            continue;
        }

        if (read_register16(fd, OV5640_CHIP_ID_LOW, &id_low) < 0) {
            fprintf(stderr,
                    "Attempt %u: register 0x%04x read failed: %s\n",
                    attempt, OV5640_CHIP_ID_LOW, strerror(errno));
            continue;
        }

        printf("Attempt %u: chip ID = 0x%02x%02x (expected 0x5640)\n",
               attempt, id_high, id_low);
        if (id_high == 0x56 && id_low == 0x40) {
            close(fd);
            puts("OV5640 detected successfully.");
            return 0;
        }
    }

    close(fd);
    fprintf(stderr, "OV5640 detection failed.\n");
    return 2;
}
