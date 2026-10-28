# OV5640 Linux SCCB ID test

This is the first Linux-side camera bring-up stage.  It intentionally reads
only registers `0x300a` and `0x300b`; it does not configure the sensor or start
either VDMA.

The software stack is:

```
ov5640-id -> /dev/i2c-N -> i2c-dev -> i2c-gpio
           -> zynq_gpio offsets 54/55 -> EMIO0/1 -> OV5640 SCCB
```

The Linux I2C address is the seven-bit address `0x3c`.  The bare-metal source
uses the eight-bit write address `0x78`; these identify the same device.

Required kernel settings:

```
CONFIG_GPIOLIB=y
CONFIG_GPIO_ZYNQ=y
CONFIG_I2C=y
CONFIG_I2C_CHARDEV=y
CONFIG_I2C_GPIO=y
```

Expected result:

```
OV5640 SCCB test: bus=/dev/i2c-0 address=0x3c
Attempt 1: chip ID = 0x5640 (expected 0x5640)
OV5640 detected successfully.
```

After this test passes, continue with `../ov5640_reg_tool`.  Keeping the ID
reader and register writer separate makes a later initialization failure easy
to distinguish from a basic SCCB wiring or power problem.
