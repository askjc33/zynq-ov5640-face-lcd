# OV5640 Linux initialization stage

This is the third Linux camera bring-up stage.  It applies the same active
register-write sequence as the proven bare-metal function:

```text
SDK/ov5640_hdmi_frame_difference/src/ov5640/ov5640_init.c
```

The generated table is not an unrelated copy.  Run the checked-in generator
whenever the proven source sequence changes:

```powershell
powershell -ExecutionPolicy Bypass -File `
  scripts/generate_ov5640_linux_table.ps1
```

The generated header records the original source line for every write.  Eight
dynamic timing entries are represented by named values and resolved from the
command-line mode at runtime.

## Default mode

```sh
ov5640-init
```

is equivalent to:

```sh
ov5640-init /dev/i2c-0 640 480 2570 980
```

The program performs these steps:

1. wait 20 ms;
2. read and verify chip ID `0x5640`;
3. issue the software reset and preserve the bare-metal 10 ms delay;
4. write every register through `I2C_RDWR`, allowing up to ten attempts for a
   transient SCCB NACK and adding a 1 ms settling interval between writes;
5. wait 100 ms;
6. read back system, PLL, RGB565, ISP, test-pattern and timing registers.

The output is RGB565 (`0x4300 = 0x61`) and the test pattern is disabled
(`0x503d = 0x00`).

Changing only width/height/totals does not make every arbitrary sensor mode
valid.  PLL, crop, subsampling, exposure limits and the downstream PL/VDMA/LCD
geometry must agree.  The parameterized interface exists so later supported
modes can reuse the control path without hiding which timing values changed.

This tool does not start either VDMA and does not access a fixed physical DDR
address.  Those operations belong to the next Linux DMA-buffer stage.

The supplied `ov5640-init.bb` explicitly places the generated header in the
BitBake work directory.  Copy it over the recipe created by
`petalinux-create`; otherwise the C source will fail to find the table header.
