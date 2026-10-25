# rover-ground-esp32p4

Ground-station radio for the winject rover link on a **Guition JC-ESP32P4-M3-DEV** (ESP32-P4 rev v1.3, 32 MB PSRAM, 16 MB flash, ESP32-C6 on SDIO). It replaces the GS's RTL8812AU + winject-radio-realtek: the P4 runs only the **winject SDIO proxy** ([winject-radio-esp32-sdio-proxy](https://github.com/winject-wireless/winject-radio-esp32-sdio-proxy)), which drives the C6 ([winject-radio-esp32c6](https://github.com/winject-wireless/winject-radio-esp32c6)) and exposes it on Ethernet as an ordinary winject radio. winject-l3 and the video receiver stay on the GS host (Orange Pi 5, [rover-ground](https://github.com/FlyingComputer2/rover-ground)).

```
 air ─▶ C6 (winject-radio-esp32c6) ──SDIO──▶ P4 (winject_proxy) ──Ethernet UDP 22 / 9000──▶ opi5: winject-l3 ─▶ vstreamer receiver
```

Everything upstream of the radio is UDP, as with the Realtek radio. On the GS, `sudo RADIO=p4 scripts/start.sh` in rover-ground runs winject-manager with `winject-manager/config.p4radio.cfg`, which points at the P4's address (`GROUND_IP` on the console, hostname `rover-ground-radio`; it comes from DHCP, so reserve it). Bench result against the rover's P4 + C6 on channel 1: ~31 fps, 0.6% UDP loss before FEC and none after, the same as the RTL8812AU.

The proxy comes in with CMake FetchContent at a pinned commit (top-level `CMakeLists.txt`); it fetches `sdio_msg` from winject-radio-esp32c6 itself. To build against a local checkout:

```bash
idf.py -DFETCHCONTENT_SOURCE_DIR_WINJECT_SDIO_PROXY=~/development/winject-radio-esp32-sdio-proxy build
```

## Board notes

- **Silicon revision.** The module's P4 is rev v1.3. `sdkconfig.defaults` sets `CONFIG_ESP32P4_SELECTS_REV_LESS_V3` and `CONFIG_ESP32P4_REV_MIN_100`; without them the bootloader dies right after `entry 0x…` with `Illegal instruction`.
- **USB.** Flash and console on the **CH340C** USB-C port (UART0, GPIO37/38, `/dev/ttyUSB*`), 921600 baud. The native USB-Serial/JTAG port (`303A:1001`) is not used.
- **Ethernet.** IP101, PHY address 1, reset GPIO51; the RMII pins are IDF 5.5's P4 EMAC defaults, so no pin config.
- **C6 link.** SDIO slot 1, CLK 18, CMD 19, D0–D3 14–17, C6 `CHIP_PU` on GPIO54: the proxy's defaults.
- **C6 boot strap.** On this board the C6's `IO9` reaches only JP1 pin 24 (pins 20/22/24/26 are the C6's `U0RXD`/`U0TXD`/`IO9`/`CHIP_PU`). The proxy drives it from **GPIO45** (LDO_VO4 at 3.3 V), so jumper **JP1 pin 24 → GPIO45**. Without it `/c6/download` does nothing, but normal boot still works.
- **Manual download mode** if auto-reset fails: hold SW1 (BOOT, GPIO35) → tap SW2 (RST) → release SW1. Bad firmware cannot brick the P4; do not run `espefuse`.

## Build and flash

Native ESP-IDF v5.5 (`. ~/esp/esp-idf-v5.5.5/export.sh`):

```bash
idf.py set-target esp32p4 build && idf.py -p /dev/ttyUSB0 flash monitor
```

Afterwards over HTTP:

```bash
curl --data-binary @build/rover-ground-esp32p4.bin http://<P4 IP>/update/p4   # kept once reached (e.g. curl http://<P4 IP>/)
curl --data-binary @winject-esp32c6.bin http://<P4 IP>/update/c6             # needs winject firmware already on the C6
```

Console: `GROUND_IP ip=…`, `GROUND_READY`, `GROUND_PROXY_STATS …` every 10 s.

## First C6 flash (stock ESP-Hosted → winject)

`/update/c6` goes over the winject SDIO link, so a C6 still running Guition's ESP-Hosted slave has to be flashed over its UART once. [`tools/c6-uart-bridge`](tools/c6-uart-bridge) is a P4 image that holds the C6 in its ROM downloader and bridges the CH340 port (UART0) to the C6's UART0 at 115200.

Jumpers on JP1: **GPIO32 → pin 20 (C6 U0RXD)**, **GPIO33 → pin 22 (C6 U0TXD)**, **GPIO45 → pin 24 (C6 IO9)**.

```bash
. ~/esp/esp-idf-v5.5.5/export.sh
# 1. P4 = bridge (it resets the C6 into download mode at boot)
(cd tools/c6-uart-bridge && idf.py set-target esp32p4 build && idf.py -p /dev/ttyUSB0 flash)
# 2. C6 through the bridge: fixed 115200, no resets (the P4 owns the C6's reset)
esptool.py --chip esp32c6 -p /dev/ttyUSB0 -b 115200 --before no_reset --after no_reset flash_id
(cd ~/development/winject-radio-esp32c6/c6/build && \
 esptool.py --chip esp32c6 -p /dev/ttyUSB0 -b 115200 --before no_reset --after no_reset \
   write_flash --flash_mode dio --flash_freq 80m --flash_size 4MB \
   0x0 bootloader/bootloader.bin 0x8000 partition_table/partition-table.bin \
   0x15000 ota_data_initial.bin 0x20000 winject-esp32c6.bin)
# 3. P4 = ground radio; the proxy resets the C6 into the new firmware
idf.py -p /dev/ttyUSB0 flash monitor
```

The C6 on this module is an ESP32-C6FH4 (4 MB flash), which winject-radio-esp32c6's 4 MB partition table fits. Save the stock image first if you want it back: `esptool.py … read_flash 0 0x400000 c6-stock.bin` at step 2.
