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