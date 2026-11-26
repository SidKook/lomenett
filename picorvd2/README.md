# PicoRVD 2

Fork of [aappleby/picorvd](https://github.com/aappleby/picorvd) for the RP2350 (Pico 2, incl. RP2354 with internal flash). PicoRVD is a GDB server that debugs/flashes a CH32V003 over WCH's single-wire SWIO using one GPIO, no WCH-Link needed. All the core work is Austin Appleby's. Erase, program and readback are verified on hardware.

## Why stock picorvd fails on the RP2350

- The PIO clock divider assumes 125 MHz; the RP2350 boots at 150 MHz, so SWIO bits run ~17% short.
- The debug-reset pulse is a CPU-cycle delay loop; the M33 runs it ~2x faster (~3 us instead of 8 us).

Another fork (picorvd-pico2w) uses a fractional divider at 150 MHz; reads work but writes store garbage (likely divider jitter, since the CH32 decodes write bits by pulse width). This fork instead drops the clock to 125 MHz and keeps the integer divider, so the waveform matches the RP2040 exactly.

## Other changes

- USB starts first and every wait has a timeout. If the target is dead at boot, the serial port prints the raw debug registers (CPBR, CFGR, DMSTATUS, ABSTRACTCS).
- Opening the port at 1200 baud reboots into the UF2 bootloader.
- SWIO pin is a build option.
- Builds from a fresh clone (uses the SDK-generated `singlewire.pio.h`).

## Wiring

CH32V003 PD1 to the SWIO pin (default GP28), common ground, 1k pull-up from SWIO to 3.3 V. Running the CH32 at ~5 V with the pull-up at 3.3 V is out of spec (VIH 3.5 V) but has been reliable here.

## Build

```
mkdir build && cd build
cmake ..                                            # RP2350 / Pico 2
cmake -DPICORVD_SWIO_PIN=17 ..                      # different SWIO pin
cmake -DPICO_BOARD=pico -DPICO_PLATFORM=rp2040 ..   # original Pico
make -j
```

Needs cmake and arm-none-eabi-gcc; fetches the Pico SDK if `PICO_SDK_PATH` is unset. Only rp2350 is hardware-tested. Flash `build/picorvd.uf2` via BOOTSEL, later via the 1200-baud trick.

## Use

```
gdb-multiarch firmware.elf
(gdb) target extended-remote /dev/ttyACM0
(gdb) load
```

RAM/register access, flash erase/program, breakpoints (software-emulated) and stepping work; `monitor reset` resets the target. Custom clients must send the GDB `+` ack after every reply, or they get one reply and then silence.

## Internals

Same modules as upstream (PicoSWIO, RVDebug, WCHFlash, SoftBreak, GDBServer, Console); see the [original README](https://github.com/aappleby/picorvd#modules). References: [QingKeV2 Debug Manual](https://github.com/openwch/ch32v003/blob/main/RISC-V%20QingKeV2%20Microprocessor%20Debug%20Manual.pdf), [RISC-V debug spec](https://github.com/riscv/riscv-debug-spec/blob/master/riscv-debug-stable.pdf), [CH32V003 reference manual](http://www.wch-ic.com/downloads/CH32V003RM_PDF.html).
