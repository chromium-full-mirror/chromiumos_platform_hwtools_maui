## Maui

Maui is device to streamline DUT debugging by combining ADB, CCD, and power delivery functionalities. This repo would be used for:
MCU Firmware:
    - based on Zephyr RTOS
    - control of on-board ICs: PDC, signal muxing, etc.
    - host interface
Host Software:
    - Linux-based tools
    - Integration with standard Google debugging tools (adb, fastboot, servod),
    - Provide maintenance tools: all system components firmware updates, managing in fleet
    - General host interface for additional features: remote DUT disconnection, etc.

## Contributions
This repository uses pre-commit hooks to enforce code style. Please make sure you have them installed by running:
```bash
pip install pre-commit --break-system-packages
pre-commit install
```
