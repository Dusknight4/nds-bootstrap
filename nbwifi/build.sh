#!/bin/bash
# Builds libnbwifi.a, the DSi WiFi driver, for the DS ARM7. Run in devkitPro's MSYS2 shell
# (devkitARM). blob/build.sh runs this first, then builds the blob nds-bootstrap loads.
set -e
cd "$(dirname "$0")"
export PATH="${DEVKITARM:-/opt/devkitpro/devkitARM}/bin:$PATH"
mkdir -p obj lib
CFLAGS="-O2 -Wall -Wno-unused-function -march=armv4t -mtune=arm7tdmi -mthumb -mthumb-interwork -ffunction-sections -fdata-sections -DARM7 -Iinclude -Isrc"
for f in src/*.c; do arm-none-eabi-gcc $CFLAGS -c $f -o obj/$(basename ${f%.c}).o; done
for f in src/*.s; do arm-none-eabi-gcc -march=armv4t -mthumb-interwork -x assembler-with-cpp -Iinclude -c $f -o obj/$(basename ${f%.s})_s.o; done
rm -f lib/libnbwifi.a
arm-none-eabi-ar rcs lib/libnbwifi.a obj/*.o
arm-none-eabi-size -t lib/libnbwifi.a | tail -1
echo built libnbwifi.a
