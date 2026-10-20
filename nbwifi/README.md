# nbwifi: the WiFi side of WideMelon multiplayer

This folder holds what nds-bootstrap loads for **Local as WiFi** (see
[WIFI-MULTIPLAYER.md](../WIFI-MULTIPLAYER.md)): a DSi WiFi driver for the ARM7
and the code that carries a DS game's local wireless over it to
[WideMelon](https://github.com/Dusknight4/widemelon).

- `src/`, `include/calico/`: the DSi (Atheros AR6002) WiFi driver: SDIO, the
  AR6K firmware interface, WPA/WPA2, and a small IP layer (ARP, IPv4, UDP,
  DHCP). The driver files come from devkitPro's
  [calico](https://github.com/devkitPro/calico), reworked to run without
  threads from nds-bootstrap's ARM7 code; `nbwifi.c` (connection handling and
  the IP layer) follows calico's `wifi.twl.32.c`.
- `wmhle/`: the ARM7 half of the DS SDK's wireless library (WM),
  reimplemented: it answers the game's local wireless requests and speaks the
  WideMelon bridge protocol over UDP (port 7071). The same files are in
  WideMelon (`src/net/wmhle`).
- `blob/`: everything above linked into one binary at a fixed place in RAM
  (`nbwifi_blob.bin`), plus the glue: joining the console's saved WiFi
  network, finding WideMelon on the network, taking the game's wireless
  requests (`pxi_find.h` finds the game's ARM7 request table by its code), and
  turning the radio off while the game isn't using its wireless.

## Building

In devkitPro's MSYS2 shell, with devkitARM installed:

```
bash nbwifi/blob/build.sh
```

This builds `libnbwifi.a`, then `nbwifi_blob.bin`, and copies it to
`retail/nitrofiles/` (and its header to `retail/common/include/nbw_blob.h`).
Then build nds-bootstrap as usual. A built `nbwifi_blob.bin` is committed, so
nds-bootstrap also builds without this step.

## Licenses

Files with an `SPDX-License-Identifier: ZPL-2.1` header are from calico,
Copyright fincs, devkitPro, under the Zope Public License 2.1
([COPYING.calico](COPYING.calico)). The rest of this folder is under the
GNU General Public License v3 or later, like nds-bootstrap.
