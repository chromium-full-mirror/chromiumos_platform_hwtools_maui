<!--
Copyright 2025 The ChromiumOS Authors
Use of this source code is governed by a BSD-style license that can be
found in the LICENSE file.
-->

# Team shared context for the Maui Project

This document provides context for the Gemini AI assistant to help it understand the project structure, build process, and conventions.

## Project Overview

Maui is device to streamline Chromebook debugging by combining ADB, CCD, and power delivery functionalities. This repo would be used for:
MCU Firmware:
    - based on Zephyr RTOS
    - control of on-board ICs: PDC, signal muxing, etc.
    - host interface
Host Software:
    - Linux-based tools (CLI)
    - Distributed via Docker container
    - Handles FW updates and device control (Integration with standard Google debugging tools)

## Directory Structure

*   `firmware/`: Main MCU firmware source.
    *   `src/`: Application source code and internal drivers.
    *   `boards/`: Board definitions (devicetree, Kconfig defconfig).
    *   `dts/`: Custom Device Tree bindings.
*   `dockerfiles/`: Docker definitions.
    *   `Dockerfile.fw_builder`: For building firmware.
    *   `Dockerfile.utils`: For host tools container (`maui-utils`).
*   `tools/`: Host-side tools and scripts.
    *   `maui_libs/`: Core Python libraries (Device abstraction, Transport, PDC update logic).
    *   `src/`: Python CLI implementations (`maui_ctl.py`, `maui_flash.py`).
    *   `scripts/`: User-facing Bash wrapper scripts (`maui-ctl`, `maui-flash`, `maui_image_manager.py`).
    *   `tests/`: Unit tests for host tools.

## Host Software Architecture

The host software is designed to be "Docker-first" to minimize dependencies on user machines.

*   **Wrapper Scripts**: Users interact with `tools/scripts/maui-ctl` and `maui-flash`.
    *   These scripts automatically fetch the `maui-utils` Docker image from a public GCS bucket (`gs://maui-firmware`) or use a local build.
    *   They invoke the tool inside the container, mapping serial ports (`/dev`) and file arguments appropriately.
*   **Image Management**: `maui_image_manager.py` (Python standard lib only) handles resolving tags (e.g., `stable`) to versions via GCS XML parsing and downloading the image.
*   **maui-libs**: The core logic is in Python (`maui_libs`).
    *   Uses `pyserial` for communication.
    *   `MauiSerialTransport` supports raw command mode for binary protocols.
*   **maui-flash**: Unified updater.
    *   **PDC**: Updates via serial passthrough commands using `maui_libs.pdc` (ported from legacy scripts).
    *   **MCU**: Orchestrates the Rust-based `fw-updater` tool (installed inside the container) to flash via BSL.

## Build Instructions

### Firmware
The firmware is built using the `maui-builder` Docker container.

To build the firmware:
```bash
docker run --rm -v $(pwd):/repo maui-builder:latest
```
Artifacts: `firmware/build_docker/zephyr/zephyr.txt`.

### Host Tools Container
To build the `maui-utils` container locally (for development):
```bash
docker build -t maui-utils -f dockerfiles/Dockerfile.utils .
```

To release a new version to GCS (manages `stable.dockertag` automatically):
```bash
./tools/release.sh --tag stable
```

To check version:
```bash
./tools/scripts/maui-ctl --version
```

## Flashing the Firmware

Use the `maui-flash` tool.

*   **Update All (Recommended)**: `./tools/scripts/maui-flash all` (Fetches stable from cloud. `--serial <SN>` is optional, required only if multiple devices are connected).
*   **MCU (Local)**: `./tools/scripts/maui-flash mcu --file path/to/zephyr.txt` (`--serial <SN>` is optional).
*   **PDC (Local)**: `./tools/scripts/maui-flash pdc --file path/to/pdc.bin` (`--serial <SN>` is optional).

## Serial Console

The Maui device exposes a serial console (via FTDI or native USB CDC).
*   **Baudrate**: 115200
*   **Prompt**: `maui$`
*   **Usage**: The console is used by `maui-ctl` for status/control (`--serial <SN>` is optional) and `maui-flash` for PDC updates.

## Development Guidelines

### GPIO Handling (Firmware)
*   Use `firmware/src/gpio.h` and `gpio_set`/`gpio_get`.
*   Define GPIOs in `gpio.dtsi` under `google,maui-gpios`.

### Application Logic (Firmware)
*   Prefer `SYS_INIT` modules over full Zephyr Drivers for logic.
*   Use `Kconfig` for tunable parameters.

### Host Scripts
*   **Wrappers**: Use Bash for top-level wrappers.
*   **Helpers**: Use Python (standard library only) for complex logic like GCS fetching (`maui_image_manager.py`) to ensure portability.
*   **Core Logic**: Use Python (`maui_libs`) with dependencies (`pyserial`, `pyusb`) inside the Docker container.

## Testing Strategy

### Firmware Unit Tests
*   **Framework**: Zephyr Twister (Ztest).
*   **Run**: `docker run --rm -v $(pwd):/repo --entrypoint /firmware_test.sh maui-builder`

### Host Tools Unit Tests
*   **Framework**: Python `unittest`.
*   **Run**: `docker run --rm -v $(pwd):/repo -w /repo maui-utils python3 -m unittest discover tools/tests`

## GCS Bucket Structure (Public)

*   **Bucket**: `gs://maui-firmware`
*   **Firmware**: `{component}-fw/{version}/{file}` (Tags via marker files like `stable.pdcfw`).
*   **Docker**: `docker/{version}/maui-utils.tar` (Tags via marker files like `stable.dockertag`).

## Git Conventions

### Commit Messages

Commit messages should follow this format:

```
<component>: <summary>

<description>

BUG=b:<bug_number>
TEST=<test_steps>
```
If there is no bug number, use `BUG=none`.

### Pre-commit Hooks

This repository uses pre-commit hooks to enforce code style. Please make sure you have them installed by running:
```bash
pip install pre-commit --break-system-packages
pre-commit install
```
