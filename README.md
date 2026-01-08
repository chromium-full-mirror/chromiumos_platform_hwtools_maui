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

## Getting started

Create a directory to checkout the Maui source:
```shell
mkdir maui_source
cd maui_source
```

Get the source code:
```shell
git clone https://chromium.googlesource.com/chromiumos/platform/hwtools/maui
cd maui
```

This repository uses pre-commit hooks to enforce code style. Please make sure you have them installed by running:
```shell
pip install pre-commit --break-system-packages
pre-commit install
```

To upload your changes to gerrit you can use:
```shell
git push origin HEAD:refs/for/main
```

## Build firmware using Docker

While being in `maui` directory (root of this repository), execute:

```shell
docker build -t maui-builder -f dockerfiles/Dockerfile.fw_builder .
docker run -it -v `pwd`:/repo maui-builder:latest
```

This will generate a txt firmware file in
`firmware/build_docker/zephyr/zephyr.txt`
This file can be used to flash Maui using BSL.

## Unit Testing

The project uses the Zephyr Twister framework for unit testing application code. Tests are executed inside the `maui-builder` Docker container to ensure a consistent environment.

### Running Tests

First, ensure the builder image is built (if not already):
```shell
docker build -t maui-builder -f dockerfiles/Dockerfile.fw_builder .
```

To run **all** unit tests:
```shell
docker run --rm -v $(pwd):/repo --entrypoint /firmware_test.sh maui-builder
```

To run a **specific** test suite (e.g., `utils.basic`):
```shell
docker run --rm -v $(pwd):/repo --entrypoint /firmware_test.sh maui-builder -s utils.basic -v
```

### Writing New Tests

1.  Create a new directory under `firmware/tests/unit/<component_name>_test`.
2.  Add a `testcase.yaml` defining the test scenarios. Ensure `platform_allow` and `integration_platforms` are set to `native_sim`.
3.  Add a `prj.conf` (usually `CONFIG_ZTEST=y` is sufficient).
4.  Add a `CMakeLists.txt` to include the test source and the application source files you are testing.
5.  Add your test code in `<component_name>_test/src/main.c` using the Ztest API (`ZTEST`, `zassert_equal`, etc.).

## Host Tools & Firmware Update

### Building the Utils Container
The project includes a utility container for host-side tools like `fw-updater`.
The container is automatically built when running the wrapper scripts, but can be built manually:

```shell
docker build -t maui-utils -f dockerfiles/Dockerfile.utils .
```

### Updating Firmware
To update the firmware using the `fw-updater` tool (wrapped in the utility container):

```shell
./tools/scripts/fw-updater <firmware_image> [options]
```

The script automatically handles mounting the firmware file into the container, so you can pass the path to the file on your host machine directly.

Example:
```shell
./tools/scripts/fw-updater firmware/build_docker/zephyr/zephyr.txt -u /dev/ttyUSB0
```
