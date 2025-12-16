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

TBD

## Build Instructions

TBD

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
