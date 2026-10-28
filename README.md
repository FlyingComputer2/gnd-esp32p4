# rover-ground-esp32p4

Ground-station firmware for Guition JC-ESP32P4-M3-DEV (P4 rev 1.3, C6 on SDIO).
The P4 runs the SDIO proxy, winject-l3 and vstreamer's `stream_receiver`.
It recovers radio FEC once, then shares recovered RTP H.264 with Ethernet
multicast and [Android](https://github.com/FlyingComputer2/rover-ground-android).
Android depayloads RTP and displays H.264 through MediaCodec.

```text
C6 → SDIO proxy → L3 UpstreamQueue → stream_receiver → recovered RTP H.264
                                                       ├→ USB channel 0 → Android
                                                       └→ 239.255.42.1:5600 / UDP
```

See [video pipeline and multicast setup](docs/video-pipeline.md).

| USB ID (`u16`) | Service | State |
| --- | --- | --- |
| 0 | Video | Receive-only RTP H.264 (`rtp-h264-v1`) |
| 1 | MSP DisplayPort | TBA; not registered |
| 2 | MSP RC telemetry | TBA; not registered |

All unsigned IDs 0–65535 are supported by the transport. Management is selected
by record kind and never routed as channel-0 video. See the [wire/API specification](docs/usb-upstream-proxy.md).

## Build and flash

ESP-IDF v5.5.5:

```sh
. ~/esp/esp-idf-v5.5.5/export.sh
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

For an existing build, enable the new TinyUSB, socket-count and pthread-stack
settings in `sdkconfig.defaults`, or regenerate `sdkconfig` from those defaults.
The app retains the existing two OTA partitions and Ethernet HTTP OTA support.
Normal C6 firmware is unchanged. Connect Android in USB host/OTG mode to the
board's **High Speed USB (USB3)** port; use CH340 for flash and logs.

L3 uses the local SDIO proxy through in-process radio queues; management uses loopback UDP 22. Its video upstream
index is 0, TX bus d4, RX bus c3, domain 0xB00B, channel 1, OFDM_24M, 15 dBm.
Upstream 1 (TX e5, RX f6, no FEC) is the bandwidth tester, app_bwtest, with its UDP
console on 5100 (`test_send <offer_kbps> <duration_ms>`, `stats`, `clr`; see the
rover-remote-esp32p4 README).
These must match the rover (with buses swapped). Video has no manager-level FEC;
vstreamer's FEC terminates at the ground P4; P4 returns receiver feedback. The manager console is bound to
0.0.0.0:2400 (no authentication; Ethernet is an internal link). Do not run a second Ethernet L3 manager against this radio:
the underlying radio proxy has a single peer per plane.

L3 creates the `UpstreamQueue` for the video upstream (no queue names in its
config) and hands it over through `Config::on_upstream_queue`. The video service
consumes that L3-created object. USB channel 0 is a
local processed source with its own bounded queue, registered through
`usb_upstream_register_source()`. Other services may still register L3 transports
directly through `usb_upstream_register()`; duplicate IDs/transports fail.

## USB behavior and status

Development identity 303a:4001; bulk OUT 01 and IN 81. Android requests
RESET_SESSION through EP0, negotiates v2 and opens channel 0 with receive
credits. One record is one application datagram. Independent credits and bounded
queues isolate stalled consumers; records are scheduled round-robin. Admission
is lossy and provides no end-to-end delivery acknowledgement.

`GROUND_READY usb=v2 video=0 ...` indicates the USB interface started after L3
published its video transport. `GROUND_USB channel=...` reports local counters.
`GROUND_L3_EXIT` indicates the manager stopped; reconnect/reset after recovery.
The current boot does not automatically restart the manager. Backend failure
invalidates an open USB session; HTTP OTA stays available over Ethernet.

Host tests cover P4 FEC recovery and feedback over UDP and direct queues, ordered retry after Ethernet TX backpressure, identical RTP output on both
branches, queue isolation, and Android fragmented IDR reconstruction. Hardware USB
enumeration, live video, power delivery and sustained latency still need a
P4/phone bench run. Live hardware validation is recorded separately from host test results.

## Board notes

- **Silicon revision.** The module's P4 is rev v1.3. `sdkconfig.defaults` sets `CONFIG_ESP32P4_SELECTS_REV_LESS_V3` and `CONFIG_ESP32P4_REV_MIN_100`; without them the bootloader dies right after `entry 0x…` with `Illegal instruction`.
- **USB.** Flash and console on the **CH340C** USB-C port (UART0, GPIO37/38, `/dev/ttyUSB*`), 921600 baud. Radio packets use the dedicated **High Speed USB** connector, USB3 in schematic sheet `5_USB&485`; the Full Speed USB / USB-Serial/JTAG connector is separate.
- **Ethernet.** IP101, PHY address 1, reset GPIO51; the RMII pins are IDF 5.5's P4 EMAC defaults, so no pin config.
- **C6 wiring.** On this board the C6's `U0RXD`/`U0TXD`/`IO9`/`CHIP_PU` come out on JP1 pins 20/22/24/26 and are jumpered to the P4:

  | C6 | JP1 | P4 | Notes |
  |---|---|---|---|
  | `U0RXD` / `U0TXD` | 20 / 22 | GPIO32 / GPIO33 | UART1 of [`tools/c6-uart-bridge`](tools/c6-uart-bridge) |
  | `IO9` (boot strap) | 24 | GPIO45 | Powered by LDO_VO4 at 3.3 V; the proxy default. Without it `/c6/download` does nothing, but normal boot still works |
  | `CHIP_PU` | 26 | GPIO54 | Proxy default |
  | SDIO | — | slot 1: CLK 18, CMD 19, D0–D3 14–17 | Proxy default |
- **Manual download mode** if auto-reset fails: hold SW1 (BOOT, GPIO35) → tap SW2 (RST) → release SW1. Bad firmware cannot brick the P4; do not run `espefuse`.

## Host verification

With winject-l3 and BFC checkouts available:

```sh
python3 tests/run_host_tests.py --l3 /path/to/winject-l3 --bfc /path/to/BFC
```

The test covers matching Android/C++ golden wire bytes, split records, high u16
IDs, channel 0 routing, duplicate bindings, independent credits, reset recovery
and invalid traffic. A PyUSB bench client is in `tools/usb-radio/smoke.py`.

## First C6 flash (stock ESP-Hosted → winject)

`/update/c6` goes over the winject SDIO link, so a C6 still running Guition's ESP-Hosted slave has to be flashed over its UART once. [`tools/c6-uart-bridge`](tools/c6-uart-bridge) is a P4 image that holds the C6 in its ROM downloader and bridges the CH340 port (UART0) to the C6's UART0 at 115200.

Jumpers on JP1: **GPIO32 → pin 20 (C6 U0RXD)**, **GPIO33 → pin 22 (C6 U0TXD)**, **GPIO45 → pin 24 (C6 IO9)**.

The bridge also builds for the rover's Waveshare ESP32-P4-WIFI6-DEV-KIT. There the C6 UART is wired the other way round (C6 U0TXD → GPIO32, U0RXD → GPIO33), and its boot strap is on GPIO22. The P4 is rev v3.x:

```bash
idf.py -B build-waveshare -D SDKCONFIG=build-waveshare/sdkconfig \
  -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.waveshare" -p /dev/ttyACM0 flash
```

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
