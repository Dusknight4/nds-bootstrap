#!/bin/bash
# Builds dstt_sd.dldi (DLDI driver for DSTT-type R4 cards' microSD, for the R4 save copy) into
# retail/nitrofiles. Run in devkitPro's MSYS2 shell.
set -e
cd "$(dirname "$0")"
export PATH="/opt/devkitpro/devkitARM/bin:$PATH"
INC=/c/wmbuild/ext/libnds-1.8.3/include
FLAGS="-march=armv4t -mtune=arm7tdmi -marm -mthumb-interwork -fPIC -Os -ffunction-sections -fdata-sections -Wall -DARM7"
mkdir -p build
arm-none-eabi-gcc $FLAGS -I$INC -c source/dstt_sd.c -o build/dstt_sd.o
arm-none-eabi-gcc $FLAGS -I$INC -x assembler-with-cpp -c source/dldi_header.s -o build/dldi_header.o
arm-none-eabi-gcc -nostartfiles -nostdlib -T dldi.ld $FLAGS build/dldi_header.o build/dstt_sd.o -o build/dstt_sd.elf -Wl,-Map,build/dstt_sd.map
arm-none-eabi-objcopy -O binary build/dstt_sd.elf build/dstt_sd.dldi
cp build/dstt_sd.dldi ../retail/nitrofiles/dstt_sd.dldi
echo "built dstt_sd.dldi ($(stat -c %s build/dstt_sd.dldi) bytes)"
