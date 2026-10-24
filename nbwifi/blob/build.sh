#!/bin/bash
# Builds nbwifi_blob.bin: the WiFi driver (libnbwifi), the local wireless glue (blob_*.c) and
# wmhle (the ARM7 half of the DS wireless library, ../wmhle), linked at NBW_BLOB_LOCATION, and
# copies it into nds-bootstrap's nitrofiles with its shared header. Run in devkitPro's MSYS2 shell
# (devkitARM), before building nds-bootstrap itself. (A built nbwifi_blob.bin is committed in
# retail/nitrofiles, so nds-bootstrap also builds without this step.)
set -e
cd "$(dirname "$0")"
export PATH="${DEVKITARM:-/opt/devkitpro/devkitARM}/bin:$PATH"
bash ../build.sh > /dev/null
mkdir -p out
CFLAGS="-O2 -Wall -mcpu=arm7tdmi -mthumb -mthumb-interwork -ffunction-sections -fdata-sections -I../src -I../wmhle -DWMHLE_MAX_CHILD=7 -DWMHLE_MAX_PARENTS=4"
arm-none-eabi-gcc $CFLAGS -c blob.c -o out/blob.o
for f in blob_wm.c blob_radio.c blob_bridge.c ../wmhle/wmhle.c ../wmhle/wmhle_air.c ../wmhle/wmpeers.c; do
    arm-none-eabi-gcc $CFLAGS -c $f -o out/$(basename ${f%.c}).o
done
arm-none-eabi-gcc -mcpu=arm7tdmi -c frame_stack.s -o out/frame_stack.o
arm-none-eabi-gcc -mcpu=arm7tdmi -mthumb -mthumb-interwork -nostartfiles -T blob.ld -Wl,--gc-sections \
    -Wl,-Map,out/blob.map out/blob.o out/frame_stack.o out/blob_wm.o out/blob_radio.o out/blob_bridge.o out/wmhle.o out/wmhle_air.o out/wmpeers.o -L../lib -lnbwifi -lc -lgcc -o out/blob.elf
arm-none-eabi-objcopy -O binary out/blob.elf out/nbwifi_blob.bin
arm-none-eabi-size out/blob.elf
echo "built nbwifi_blob.bin ($(stat -c %s out/nbwifi_blob.bin) bytes)"
cp out/nbwifi_blob.bin ../../retail/nitrofiles/
cp blob.h ../../retail/common/include/nbw_blob.h
echo "copied into nds-bootstrap (retail/nitrofiles, retail/common/include/nbw_blob.h)"
