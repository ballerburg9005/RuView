# RuView CSI for the original ESP32 Cheap Yellow Display (CYD)

**Target:** ESP32-2432S028 / ESP32-2432S028R (2.8-inch CYD,
original ESP32-WROOM-32, 4 MB flash, no PSRAM), with the SPI ILI9341
240x320 display wired exactly as in the user's board pinout.

This is a **minimal raw-CSI firmware with an onboard status screen**, not
a port of the full ESP32-S3/C6 RuView edge firmware. It preserves the current
upstream firmware and requires no extra hardware beyond USB power and an
existing **2.4 GHz** Wi-Fi access point. The display is driven over
SPI2/HSPI (not 8-bit parallel); touch is not yet implemented.
CYD revisions using an ST7789 instead of an ILI9341 require a separate
display driver/initialization and are not qualified in this version.

A single board captures CSI from packets transmitted by its associated Wi-Fi
access point and forwards raw I/Q samples to a Linux host using RuView's
**ADR-018 (UDP magic `0xC5110001`)** format. Each board has its own
node ID. This can be used for RF presence/motion *experimentation*, but
not for dependable medical, fall, heartbeat or breathing monitoring.

## Topology

    Linux PC (wired or Wi-Fi)        existing 2.4 GHz Wi-Fi router/AP
        |                                            |
        |  UDP probes to port 5051 ----------------->| Wi-Fi unicast
        |                                            +------> CYD #1
        |                                            +------> CYD #2
        |                                            +------> CYD #3
        ^                                                      |
        +-------------------- UDP 5005 raw CSI frames ---------+

The router does **not** need CSI support or custom firmware; only the CYD
receivers collect CSI. Each CYD joins an existing 2.4 GHz network as a
normal station. The host must be on the same reachable LAN. An AP may
transmit packets at legacy 802.11b rates for some traffic (no CSI), so
the included UDP traffic helper is useful to provide a reproducible
unicast stream. It does not interact with cloud services.

## Build on Linux (Espressif ESP-IDF 5.4.x)

1. Install Espressif ESP-IDF 5.4.x for target `esp32`; activate its
   environment (`. /path/to/esp-idf/export.sh`). Use the official
   [ESP-IDF Linux setup](https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32/get-started/linux-macos-setup.html).
   Example with an existing installation:

   ```sh
   . ~/esp/esp-idf/export.sh
   cd RuView/firmware/esp32-cyd-csi
   idf.py set-target esp32
   idf.py menuconfig
   ```

2. In **RuView CYD (original ESP32) CSI Node**, enter:
   - Your *2.4 GHz* Wi-Fi SSID and password.
   - Linux receiver's private LAN IPv4 (e.g. `192.168.178.20`).
   - Collector UDP port, default `5005`.
   - Unique node ID: `1` for first CYD, `2` for second, etc.
   - Probe port default `5051`; max CSI forwarding rate default 30/s.

   Do **not** commit generated `sdkconfig`: it contains your Wi-Fi password.
   There is no provisioning API in this prototype. Re-run `menuconfig`
   before building for the next board.

3. Build and flash over the CYD's USB-to-UART port:

   ```sh
   idf.py build
   ls /dev/serial/by-id/
   idf.py -p /dev/ttyUSB0 flash monitor
   ```

   Substitute the actual port for your board. On Arch Linux, the
   serial-port access group is often `uucp`; use your distro's correct
   permission setup. Do not flash ESP32-S3 or ESP32-C6 binaries to CYD.

4. The screen starts after initialization and refreshes roughly every **2 seconds**.
   The serial console also prints its DHCP IPv4 address and two-second statistics:

   ```text
   Node IP: 192.168.178.81 (probe UDP 5051; collector UDP 5005)
   2s: CSI callback=60 queued=58 UDP sent=58 probes=60 ...
   ```

   Numbers above are **illustrative**, not recorded hardware results.

## Quick screen-only test without Wi-Fi credentials

Once the branch's **CYD original ESP32 CSI firmware** GitHub Actions job
passes, download its `cyd-esp32-s028-test-firmware` artifact (three .bin
files) from the successful workflow run. It is compiled **without your
private Wi-Fi credentials**, so it is useful for checking the SPI display
before configuring the network. The board should show `WIFI: DISCONNECTED`
and `IP: NONE` rather than simulate connectivity.

After extracting the artifact, flash using esptool (requires serial
port access; this overwrites the board's current application):

```sh
python -m esptool --chip esp32 --port /dev/ttyUSB0 write_flash \
  --flash_mode dio --flash_freq 40m --flash_size 4MB \
  0x1000 bootloader.bin \
  0x8000 partition-table.bin \
  0x10000 ruview_cyd_csi.bin
```

Replace the serial device path. These files are produced for the **original
ESP32 2.8-inch SPI CYD**, not an ESP32-S3 nor a parallel-screen 2.2-inch
CYD. Rebuild via `idf.py menuconfig` (above) for Wi-Fi CSI testing.

## Verify CSI before running the full RuView aggregator

Run this on the **Linux PC**. The collector binds UDP 5005:

```sh
cd RuView/firmware/esp32-cyd-csi
python tools/monitor.py --port 5005 --csv /tmp/cyd-csi.csv
```

In another terminal, after seeing the CYD IP via the serial console:

```sh
python tools/traffic.py 192.168.178.81 --hz 30 --seconds 60
```

When you have multiple boards, you can pass all their IPs:

```sh
python tools/traffic.py 192.168.178.81 192.168.178.82 192.168.178.83 --hz 30 --seconds 60
```

Monitor output reports each node's packet rate, Wi-Fi signal strength,
subcarriers and mean raw CSI magnitude. If this works, stop the diagnostic
monitor and run a RuView UDP aggregator configured for **ADR-018 raw CSI on
UDP port 5005**. Only one program can bind the same port at once.

**Integration status:** the wire packet format matches RuView's documented
CSI frame header/payload, but dashboard discovery and higher-level inference
have **not** been validated with this implementation. In particular, this
firmware sends **raw CSI only**, no RuView vital-sign, mesh-health,
feature-state or cross-node time-sync frames.

## Troubleshooting

| Symptom | What to check |
|---|---|
| No Wi-Fi connection | SSID is 2.4 GHz, WPA password, no client isolation/captive portal |
| `probes=0` | Wrong CYD IP, Linux probe sender, firewall, AP guest/client isolation |
| Probes arrive, `CSI callback=0` | Router's transmitted 802.11 PHY rate; try nearby and move or change AP |
| Callbacks, but `queued=0` | AP BSSID filter mismatch, rate gate, invalid CSI length |
| Queued, but receiver empty | Set correct Linux IP; inbound UDP 5005 firewall; host binding |
| Receiver shows 0 Hz for a node | Stream absent; verify USB power and Wi-Fi association |
| Screen is black | Check that the board is ESP32-2432S028, active-high backlight on GPIO21, SPI ILI9341. Some later revisions have ST7789; check serial for "CYD TFT unavailable" |
| Different CSI lengths | Expected: 11g / 11n / STBC and channel width change subcarrier count |

Keep each CYD stationary with USB power. Rooms can have different multipath
and access-point geometry. Independent node IDs do **not** mean that this
prototype can triangulate a person: multi-node fusion requires validation,
time synchronization and suitable calibration.

## Onboard SPI status screen (CYD 2.8-inch)

Display pin mapping from the original 2.8-inch ESP32-2432S028 board:

| LCD signal | ESP32 GPIO |
|---|---:|
| SPI MOSI | 13 |
| SPI SCLK | 14 |
| SPI CS | 15 |
| LCD DC | 2 |
| Backlight (active high) | 21 |
| Reset | Board reset / EN (no dedicated GPIO) |

Uses the ILI9341 controller at a conservative **20 MHz** SPI clock.
The 320x240 landscape screen renders a small diagnostics table:

- Wi-Fi associated / disconnected and DHCP IP
- Connected AP channel and RSSI
- Real CSI callback, accepted, and UDP sent packets **per second**
- Traffic probes per second and cumulative dropped packets
- Explicit **RAW WIFI CSI ONLY** and **NOT FOR MEDICAL USE** warnings

No invented breathing or heart-rate data is shown. The screen uses a 10 KiB
DMA-compatible strip, not a full-screen framebuffer or LVGL. It updates from
the normal FreeRTOS status task, never directly inside a CSI callback.
Display failure should leave CSI streaming running. Touch and SD card are
untouched and unconfigured. This has passed source compilation but **has not
been flashed or visually verified on a real CYD**.

Known caveat: a portion of ESP32-2432S028 clones use ST7789 displays instead
of ILI9341. Those may show a blank/white screen with this firmware; use a
matching ST7789 panel implementation after confirming the display controller.

## Remaining functionality

The firmware can read raw CSI and send RuView-formatted UDP. It **does not**
provide RuView's native onboarding/OTA workflow, synchronized multi-node
localization, touchscreen setup, on-device vital sign estimation, validated
presence detection, remote alerting or medical-grade health monitoring.
An ordinary Wi-Fi router is a transmission source, **not** a guaranteed
continuous 30 Hz CSI generator. The `tools/traffic.py` host script improves
test packet yield but the actual rate depends on Wi-Fi PHY choices, signal
quality, interference and router configuration.

## Packet format

20-byte little-endian header plus signed 8-bit I/Q bytes as received from
ESP-IDF. The serialized fields are:

| Offset | Field |
|---|---|
| 0–3 | 32-bit magic `0xC5110001` |
| 4 | Node ID |
| 5 | Antennas = 1 |
| 6–7 | Number of subcarriers (uint16) |
| 8–11 | Frequency in MHz (uint32) |
| 12–15 | Sequence number (uint32) |
| 16 | RSSI (int8) |
| 17 | Noise floor (int8) |
| 18 | PPDU type (0 = legacy/HT) |
| 19 | Flags bit 0 = 40 MHz, bit 2 = STBC, bit 5 = first 4 CSI bytes sanitized |
| 20+ | I/Q array, 2 bytes per subcarrier |

Source reference:
[RuView original ESP32-S3 packet serialization](../esp32-csi-node/main/csi_collector.c)
and [Espressif ESP-IDF Wi-Fi CSI guide](https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32/api-guides/wifi.html).

## Test boundary and medical limitation

- Protocol decoder unit tests: `python -m unittest discover -s tools -p 'test_*.py'`
- ESP32 compile: run `idf.py build` or see GitHub Actions build lane.
- **Hardware flash, physical packet rates, operational stability and
  accuracy are not yet verified**; the intended board must be tested.
- No heartbeat-rate, respiration-rate, fall-detection or person-localization
  claims. This is not a safety monitor for vulnerable people.
