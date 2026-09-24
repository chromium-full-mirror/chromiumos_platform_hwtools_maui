# Maui Host Software Tools

This directory contains the host-side software for managing and interacting with the Maui debug device.

## Overview

The software suite consists of:

*   **`maui-libs`**: A Python library providing the core logic for device discovery, serial communication, and firmware updates.
*   **`maui-ctl`**: A CLI tool for device control (status, power, data).
*   **`maui-flash`**: A unified CLI tool for updating firmware on the Maui device (MCU, PDC).
*   **`maui-utils`**: A Docker container that packages these tools and their dependencies for easy distribution and execution.

## Getting Started

### Prerequisites

*   Docker installed on your host machine.
*   A connected Maui device.

### Build the Docker Image (Local Development)

If you are developing or want to use a locally built image, you can build it yourself. The wrapper scripts support a `--utils_channel local` flag to use this image instead of fetching one from Google Cloud Storage.

```bash
# From the root of the repo
docker build -t maui-utils -f dockerfiles/Dockerfile.utils .
```

## Common CLI Arguments

All Maui host tools support the following common arguments:

*   `--serial <SERIAL>`: Specify the serial number of the Maui device to target (required when multiple devices are connected).
*   `--port <PORT>`: Specify the explicit serial device port (e.g. `/dev/ttyACM0` or `/dev/ttyUSB0`) to bypass automatic discovery.
*   `--usb`: Enable native USB DFU flashing mode for `maui_proto_5117` hardware.
*   `--verbose`: Enable detailed debug logging for troubleshooting transport and flashing.
*   `--internal-version`: Report the internal version string of `maui-libs` and exit.

## Usage

Wrapper scripts are provided in `tools/scripts/` to execute the tools inside the Docker container seamlessly. These scripts automatically handle downloading the necessary Docker image from GCS if not using local mode.

### Docker Image Selection

*   **Default**: Fetches the `stable` version from GCS.
*   `--utils_channel <name>`: Use a specific channel (e.g., `alpha`, `prev`) or explicit version (e.g., `v1.0.0`). The script checks if the image for this version is already available locally before downloading.
*   `--utils_channel local`: Use the locally built `maui-utils` image.

### `maui-ctl` - Device Control

Used for querying device status and controlling power/data lines.

**Version:**
```bash
./tools/scripts/maui-ctl --version
```
Reports the version string of the tools *inside* the container.

**Status:**
```bash
./tools/scripts/maui-ctl --serial <SERIAL> status
```
*Example Output:*
```
INFO: Device Version: Maui version 0.1.0-proto
```

**Power Control:**
Controls the load switch to the DUT.
```bash
# Turn DUT power ON
./tools/scripts/maui-ctl --serial <SERIAL> power on

# Turn DUT power OFF
./tools/scripts/maui-ctl --serial <SERIAL> power off

# Cycle DUT power (Off then On)
./tools/scripts/maui-ctl --serial <SERIAL> power cycle
```

**Data Control:**
Controls the USB data muxes to the DUT.
```bash
# Enable USB data (Connect)
./tools/scripts/maui-ctl --serial <SERIAL> data on

# Disable USB data (Disconnect)
./tools/scripts/maui-ctl --serial <SERIAL> data off

# Cycle USB data (Disconnect then Connect)
./tools/scripts/maui-ctl --serial <SERIAL> data cycle
```

### `maui-flash` - Firmware Updates

Used for updating the firmware of Maui components.

#### Update All Components (Recommended)

Updates both MCU and PDC firmware to the version specified (default: `stable`).

```bash
./tools/scripts/maui-flash --serial <SERIAL> all
./tools/scripts/maui-flash --serial <SERIAL> all --fw_channel alpha
```

#### Power Delivery Controller (PDC)

**Flash from a local file:**
```bash
./tools/scripts/maui-flash --serial <SERIAL> pdc --file /path/to/pdc.bin
```

**Flash the 'stable' version (fetched from GCS):**
```bash
./tools/scripts/maui-flash --serial <SERIAL> pdc --fw_channel stable
```

#### Microcontroller (MCU)

Depending on your board hardware:
* **`maui_proto_5117`**: Supports native USB flashing via DFU (`--usb`).
* **`maui_proto_3507`**: Uses UART BSL flashing (`--port`).

**Flash from a local file:**
```bash
# Native USB DFU (maui_proto_5117)
./tools/scripts/maui-flash --usb mcu --file /path/to/maui_proto_5117.txt

# UART BSL (maui_proto_3507 / explicit port)
./tools/scripts/maui-flash --port /dev/ttyUSB0 mcu --file /path/to/maui_proto_3507.txt
```

**Flash the 'stable' version:**
```bash
# Native USB (maui_proto_5117)
./tools/scripts/maui-flash --usb mcu --fw_channel stable

# UART (maui_proto_3507)
./tools/scripts/maui-flash mcu --fw_channel stable
```

#### H2H Bridge SPI Flash (IT82002)

Programs the Prolific PL27A1 SPI flash through the MCU over the "Maui RPC"
interface (whole-chip erase, program, CRC-32 verify on the MCU):
```bash
./tools/scripts/maui-flash --serial <SERIAL> h2h --file /path/to/pl27a1.bin
./tools/scripts/maui-flash --serial <SERIAL> h2h --info
```

Dump the current image over the same RPC interface (default 8704 B, the PL27A1
configuration size). The H2H bridge is held in reset and drops off USB while
reading:
```bash
./tools/scripts/maui-flash --serial <SERIAL> h2h --read h2h_dump.bin
./tools/scripts/maui-flash --serial <SERIAL> h2h --read full.bin --size 524288
```

## Transport & Device Discovery Architecture

The `maui_libs.transport` module manages low-level serial communication and handles hardware differences across board phases:

*   **Supported Device Types**:
    *   **Proto Board (`maui_proto_3507`)**: FTDI Dual USB-to-UART bridge (VID `0x0403`, PID `0x6002` / `0x6001`).
    *   **Final Board (`maui_proto_5117`)**: Native USB CDC ACM (VID `0x18D1`, PID `0x5085`).
*   **Shell Synchronization (`_synchronize_shell`)**: Ensures the serial interface is clean and ready at the prompt (`maui$`). Uses a bounded retry loop (5 retries) and non-blocking reads to avoid hangs on chatty asynchronous serial output.
*   **Port Wait Helper (`wait_for_port`)**: Monitors CDC ACM port re-enumeration after firmware updates or reboots, ensuring tools wait for the MCU USB stack to come back online before sending subsequent commands.

## Development

### Project Structure

*   `tools/maui_libs/`: Python package source.
    *   `transport.py`: Low-level serial communication, prompt parsing, and device scanning.
    *   `device.py`: High-level `MauiDevice` abstraction.
    *   `pdc.py`: Logic for PDC firmware updates.
    *   `rpc.py`: Client for the IT82002 "Maui RPC" channel (COBS + Protobuf, `firmware/proto/maui_rpc.proto`).
    *   `h2h.py`: H2H bridge SPI flash reading and programming over RPC.
    *   `proto/`: Generated Protobuf modules; regenerate with `tools/scripts/gen_proto.sh`.
    *   `cli.py`: Common CLI argument parsing and logging setup.
*   `tools/src/`: CLI entry points (`maui_ctl.py`, `maui_flash.py`).
*   `tools/tests/`: Unit tests.
*   `tools/scripts/`: Host-side wrapper scripts (`maui-ctl`, `maui-flash`, `maui_image_manager.py`).

### Running Tests

You can run the unit tests inside the Docker container:

```bash
docker run --rm -v $(pwd):/repo -w /repo maui-utils \
    python3 -m unittest discover tools/tests
```

### Release Process

#### Automated Release (Recommended)

Unified scripts are provided in `tools/scripts/release/` to handle the entire release flow, including building, tagging, and GCS upload.

**Host Tools:**
```bash
# Release as 'stable' (updates the stable channel marker in GCS)
./tools/scripts/release/tools_release.sh --tag stable
```

**MCU Firmware:**
```bash
# Build and release as 'stable'
./tools/scripts/release/mcu_release.sh --tag stable
```

**PDC Firmware:**
```bash
# Release a local binary to the 'stable' channel
./tools/scripts/release/pdc_release.sh --file path/to/pdc_v1.2.bin --version v1.2 --tag stable
```

The scripts automatically:
1.  Generate dynamic version strings.
2.  Build or package the relevant components.
3.  Upload artifacts to GCS.
4.  Manage channel tag migration (stable, alpha, etc.) by creating empty marker objects in GCS.

#### GCS Bucket Structure

Firmware and Docker image fetching relies on a public GCS bucket `gs://maui-firmware`.

**Firmware:**
`{component}-fw/{version}/{file}`
*   Tags are resolved via marker files (e.g., `stable.pdcfw`) in version directories.

**Docker Images:**
`docker/{version}/maui-utils.tar`
*   Tags are resolved via marker files (e.g., `stable.dockertag`) in version directories.
