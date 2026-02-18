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

**Power / Data (Placeholder):**
Current firmware does not yet support remote power/data switching. The commands exist but will report as unimplemented.
```bash
./tools/scripts/maui-ctl --serial <SERIAL> power off
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

**Flash from a local file:**
```bash
./tools/scripts/maui-flash --serial <SERIAL> mcu --file /path/to/zephyr.txt
```

**Flash the 'stable' version:**
```bash
./tools/scripts/maui-flash --serial <SERIAL> mcu --fw_channel stable
```

## Development

### Project Structure

*   `tools/maui_libs/`: Python package source.
    *   `transport.py`: Low-level serial communication and device scanning.
    *   `device.py`: High-level `MauiDevice` abstraction.
    *   `pdc.py`: Logic for PDC firmware updates.
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

A unified script is provided to handle the entire release flow, including building, tagging, and GCS upload.

```bash
# Release as a specific version (e.g. v0.0.1)
./tools/release.sh --tag v0.0.1

# Release as 'stable' (updates the stable channel marker in GCS)
./tools/release.sh --tag stable
```

The script automatically:
1.  Generates a dynamic version string based on `tools/VERSION` and git hash. This version is embedded in the Docker image and can be verified via `./maui-ctl --version`.
2.  Builds the `maui-utils` Docker image.
3.  Tags and saves the image.
4.  Uploads the tarball to GCS.
5.  If tagging as `stable`, it migrates the `stable.dockertag` marker in GCS to the current version.

#### Manual Release Steps (Reference)

To release a new version of the host tools Docker image manually:

### GCS Bucket Structure

Firmware and Docker image fetching relies on a public GCS bucket `gs://maui-firmware`.

**Firmware:**
`{component}-fw/{version}/{file}`
*   Tags are resolved via marker files (e.g., `stable.pdcfw`) in version directories.

**Docker Images:**
`docker/{version}/maui-utils.tar`
*   Tags are resolved via marker files (e.g., `stable.dockertag`) in version directories.
