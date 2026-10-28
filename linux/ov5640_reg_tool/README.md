# OV5640 Linux register tool

This is the second Linux camera bring-up stage.  It keeps the known-good
`ov5640-id` diagnostic unchanged and adds explicit single-register read/write
operations through `/dev/i2c-N`.

The OV5640 protocol used here is:

```text
7-bit slave address: 0x3c
register address:    16 bits, high byte first
register value:       8 bits
```

Examples:

```sh
ov5640-reg /dev/i2c-0 read 0x300a
ov5640-reg /dev/i2c-0 read 0x300b
ov5640-reg /dev/i2c-0 read 0x503d
ov5640-reg /dev/i2c-0 write 0x503d 0x80
ov5640-reg /dev/i2c-0 write 0x503d 0x00
```

Register `0x503d` selects the sensor test pattern.  Bit 7 enables the color-bar
pattern.  During this stage, use it only for a write/readback test and restore
the original value immediately.  Do not experiment with clock, reset, PLL, or
timing registers until the complete initialization sequence is available.

Each write operation performs a separate readback and returns:

- `0` when the readback matches;
- `1` for an I/O error;
- `2` for invalid command-line input;
- `3` when the register readback differs from the requested value.

This program only configures the sensor control plane.  It does not start the
VDMA or allocate Linux-safe frame buffers.
