# NBTEVO iDrive Carlife+

English · [简体中文](README.zh-CN.md)

CarLife+ phone projection for BMW NBT Evo iDrive 5 and 6. The project includes a QNX head-unit application, an iDrive menu plugin, and a desktop simulator.

| Directory | Contents |
| --- | --- |
| [`nbt/`](nbt/) | QNX projection, wireless discovery, the RHMI menu, and build and deployment scripts |
| [`desktop/`](desktop/) | Desktop USB and Wi-Fi simulator and an offline projection demo |
| [`vendor/hu_lab/`](vendor/hu_lab/) | Third-party CarLife protocol and USB modules |
| `dist/qnx-armv7le/` | Local build output for QNX ARMv7LE, including installation files and SHA-256 checksums |

See [NOTICE](NOTICE) for third-party components and license information.

The deployment guide covers the iDrive multimedia menu, installation, startup and reconnection, persistent PF rules, and rollback. The QNX guide includes build instructions and on-vehicle test results. Both guides are in Chinese. [Deployment guide](nbt/INSTALL.md) · [QNX build and test guide](nbt/README.md)

## Offline tests

Run `sh nbt/test-host.sh` from the repository root to test protocol negotiation, display recovery, touch input, PCM audio, Wi-Fi discovery, and firewall rule handling. GitHub Actions also runs the offline demo, checks deployment script syntax, and generates and validates the RHMI menu resources.

Install Python 3 and FFmpeg with the `libx264` encoder, then run:

```sh
python3 demo.py
```

The demo uses in-memory CarLife packets to simulate version negotiation and video startup. It decodes an H.264 test pattern, sends a touch down and up at the center of the frame, and prints the path to the decoded PNG. Pass `--output PATH` to use a directory that does not yet exist.

To replay an existing recording, run `ffplay -f h264 recording.h264`. To verify that the whole file decodes, run `ffmpeg -v error -xerror -f h264 -i recording.h264 -f null -`.

On November 21, 2026, testing on macOS verified the offline handshake, first frame, and touch round trip. A 1280 × 720 H.264 recording from a vivo phone decoded successfully. The C++ session tests covered 852 × 480 and 848 × 480 modes and negotiation at 60 and 30 fps.

Protocol references. [CarLife-Vehicle-Lib](https://github.com/674809/carlife/tree/f9522db/CarLife-Vehicle-Lib) · [Carlife_on_box protocol implementation](https://github.com/SadykovIV/Carlife_on_box/blob/d9397d9/hu_lab/protocol.py)

## Desktop USB simulator

Install libusb 1.0 first. On macOS, run `brew install libusb`. From the repository root:

```sh
python3.12 -m venv .venv
.venv/bin/python -m pip install -r desktop/requirements.txt
.venv/bin/python simulator.py
```

The defaults match the tested vivo X Fold5 and Mac USB port `2:1`. Once the window opens, reconnect the phone over USB, keep Bluetooth connected, and open the phone’s built-in Smart Car app (智能车载). Click or drag on the projected image to send single-touch input. Use `--device` and `--vendor` to select another port or phone. To list USB port identifiers and vendor IDs, run:

```sh
.venv/bin/python -c 'from vendor.hu_lab.usb import LibUSB; u=LibUSB(); print([(d["physical_key"], hex(d["identity"]["vid"])) for d in u.discover()]); u.close()'
```

The simulator connects through Android Open Accessory (AOA), decodes H.264 continuously, and waits for the phone to be reconnected after a disconnect. Video is processed in memory by default. The desktop simulator supports video and single-touch input. Audio output is unavailable.

## Wireless projection on macOS

Put the Mac and phone on the same reachable Wi-Fi network. Select wireless CarLife+ in the vivo Smart Car app and find the phone’s IPv4 address in its network details.

```sh
.venv/bin/python simulator.py --wifi PHONE_IPV4
```

If the phone requires its own hotspot, join that hotspot from the Mac and use the hotspot gateway address. The simulator connects to the phone’s six CarLife TCP services and reports the first port it cannot reach.

Live 1280 × 720 wireless projection was verified on a vivo X Fold5. Touch return passed with a locally simulated phone. Touch response on the physical phone still needs verification.
