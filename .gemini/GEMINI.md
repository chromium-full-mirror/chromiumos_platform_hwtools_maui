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
    - Linux-based tools
    - Integration with standard Google debugging tools (adb, fastboot, servod),
    - Provide maintenance tools: all system components firmware updates, managing in fleet
    - General host interface for additional features: remote DUT disconnection, etc.

## Directory Structure

*   `firmware/`: Main MCU firmware source.
    *   `src/`: Application source code and internal drivers.
    *   `boards/`: Board definitions (devicetree, Kconfig defconfig).
    *   `dts/`: Custom Device Tree bindings.
*   `dockerfiles/`: Docker definitions for build and test environments.
*   `tools/`: Host-side tools and scripts.

## Build Instructions

The firmware is built using the `maui-builder` Docker container.

To build the firmware:
```bash
docker run --rm -v $(pwd):/repo maui-builder:latest
```

This will generate the build artifacts in `firmware/build_docker/`.

## Development Guidelines

### GPIO Handling
The project uses a custom internal driver for GPIO management to simplify access and configuration.

1.  **Definition**: All GPIOs must be defined in the `gpio.dtsi` file under the `google,maui-gpios` compatible node.
2.  **Naming**: Use descriptive node names. The driver automatically generates an enum value for each GPIO based on its node name (converted to uppercase and prefixed with `GPIO_`).
    *   Example: Node `usb3_mux_sel` becomes `GPIO_USB3_MUX_SEL`.
3.  **Access**: Do **not** use standard Zephyr `gpio_dt_spec` or `device_get_binding` for these GPIOs in application code.
    *   Include `firmware/src/gpio.h`.
    *   Use `gpio_set(GPIO_ENUM_NAME, val)` to set output values.
    *   Use `gpio_get(GPIO_ENUM_NAME)` to read input values.

### Application Logic & Drivers
*   **Logic Modules**: For application logic (e.g., controlling muxes, handling state machines based on ADC inputs), prefer creating simple modules initialized via `SYS_INIT`. Avoid creating full Zephyr Device Drivers (using `DEVICE_DT_INST_DEFINE`) unless the module strictly represents a hardware peripheral requiring that model.
*   **Configuration**: Use `Kconfig` for tunable parameters (e.g., timing delays, voltage thresholds) rather than Device Tree properties, especially for logic modules that don't have a direct hardware representation in the DT.

## Flashing the Firmware

TBD

## Serial Console

TBD

## Testing Strategy

The project utilizes the Zephyr Twister framework for unit testing application logic.

### Unit Tests
- **Location:** `firmware/tests/unit/`
- **Framework:** Zephyr Ztest + Twister
- **Target Platform:** `native_sim` (simulates execution on the host machine).

### Creating Unit Tests
When creating new unit tests, follow this structure:

1.  **Directory:** `firmware/tests/unit/<component>/`
2.  **Configuration (`testcase.yaml`):**
    ```yaml
    tests:
      <component>.basic:
        tags: test_framework
        platform_allow: native_sim
        integration_platforms:
          - native_sim
    ```
3.  **Project Config (`prj.conf`):**
    ```
    CONFIG_ZTEST=y
    ```
4.  **Build (`CMakeLists.txt`):**
    Link the specific source files from the main firmware that need to be tested.
    ```cmake
    cmake_minimum_required(VERSION 3.20.0)
    find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
    project(my_test)

    target_sources(app PRIVATE src/main.c)
    target_sources(app PRIVATE ../../../src/target_file.c) # Add file under test
    target_include_directories(app PRIVATE ../../../src)
    ```
5.  **Source (`src/main.c`):** Use `ZTEST` macros.

### Executing Tests
Tests are run using the `maui-builder` Docker container. The `firmware_test.sh` script inside the container handles the environment setup.

To run all tests:
```bash
docker run --rm -v $(pwd):/repo --entrypoint /firmware_test.sh maui-builder
```

To run specific tests or pass arguments:
```bash
docker run --rm -v $(pwd):/repo --entrypoint /firmware_test.sh maui-builder -s <test_suite> -v
```

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
