#!/usr/bin/env python3
# Copyright 2026 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""
Helper script to manage Maui Docker images.
Resolves, fetches, and loads Docker images from GCS or checks for local ones.
"""

import argparse
import logging
import os
import shutil
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request
import xml.etree.ElementTree as ET


# GCS Bucket Base URL
GCS_BASE_URL = "https://storage.googleapis.com/maui-firmware"
DEFAULT_IMAGE = "maui-utils"

logging.basicConfig(level=logging.WARNING, format="%(message)s")
logger = logging.getLogger(__name__)


def resolve_version(channel):
    """Resolves a channel to a version string."""
    if channel not in ["stable", "alpha", "prev"]:
        return channel

    logger.info("Resolving channel '%s'...", channel)
    try:
        with urllib.request.urlopen(f"{GCS_BASE_URL}?prefix=docker/", timeout=10) as r:
            root = ET.fromstring(r.read())

        found_key = None
        for child in root:
            if child.tag.endswith("Contents"):
                key_elem = None
                for sub in child:
                    if sub.tag.endswith("Key"):
                        key_elem = sub
                        break

                if key_elem is not None:
                    key = key_elem.text
                    if key.endswith(f"/{channel}.dockertag"):
                        found_key = key
                        break

        if not found_key:
            raise RuntimeError(f"Channel '{channel}' not found.")

        # Key: docker/v1.0.0/stable.dockertag
        parts = found_key.split("/")
        if len(parts) >= 3:
            return parts[1]
        raise RuntimeError(f"Unexpected key format: {found_key}")
    except Exception as e:
        raise RuntimeError(f"Failed to resolve channel '{channel}': {e}") from e


def download_image(version):
    """Downloads the docker image tarball for a version to a temporary file."""
    target_dir = f"docker/{version}"
    logger.info("Locating image for %s...", version)

    image_url = ""
    try:
        with urllib.request.urlopen(
            f"{GCS_BASE_URL}?prefix={target_dir}/", timeout=10
        ) as r:
            root = ET.fromstring(r.read())

        for child in root:
            if child.tag.endswith("Contents"):
                key_elem = None
                for sub in child:
                    if sub.tag.endswith("Key"):
                        key_elem = sub
                        break

                if key_elem is not None:
                    key = key_elem.text
                    if key.endswith(".tar"):
                        image_url = f"{GCS_BASE_URL}/{key}"
                        break

        if not image_url:
            raise RuntimeError(f"No .tar image found in {target_dir}")

    except Exception as e:
        raise RuntimeError(f"Failed to locate image: {e}") from e

    # Create a temporary file
    fd, output_path = tempfile.mkstemp(prefix=f"maui_{version}_", suffix=".tar")
    os.close(fd)

    print(f"Downloading Maui Docker image ({version})...", file=sys.stderr)
    try:
        with urllib.request.urlopen(image_url, timeout=60) as r, open(
            output_path, "wb"
        ) as f:
            shutil.copyfileobj(r, f)
        return output_path
    except Exception as e:
        if os.path.exists(output_path):
            os.remove(output_path)
        raise RuntimeError(f"Download failed: {e}") from e


def ensure_image(channel):
    """Ensures the docker image is available and returns its tag."""
    if channel == "local":
        # Check if local image exists
        try:
            subprocess.check_output(
                ["docker", "images", "-q", DEFAULT_IMAGE], stderr=subprocess.DEVNULL
            )
            return DEFAULT_IMAGE
        except subprocess.CalledProcessError:
            print(
                f"Error: Local image '{DEFAULT_IMAGE}' not found. Please build it.",
                file=sys.stderr,
            )
            sys.exit(1)

    # Remote Flow
    version = resolve_version(channel)
    target_tag = f"{DEFAULT_IMAGE}:{version}"

    # Check if already loaded
    try:
        out = subprocess.check_output(
            ["docker", "images", "-q", target_tag], stderr=subprocess.DEVNULL
        )
        if out.strip():
            return target_tag
    except subprocess.CalledProcessError:
        pass

    # Download and Load
    tar_path = download_image(version)
    try:
        print("Loading Docker image...", file=sys.stderr)
        subprocess.check_call(
            ["docker", "load", "-i", tar_path],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    finally:
        # Cleanup temporary file
        if os.path.exists(tar_path):
            os.remove(tar_path)

    return target_tag


def main():
    """Main entry point."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--utils_channel", default="stable")
    parser.add_argument("--verbose", action="store_true")
    args, _ = parser.parse_known_args()  # Ignore other args

    if args.verbose:
        logger.setLevel(logging.INFO)

    try:
        image = ensure_image(args.utils_channel)
        print(image)
    except Exception as e:  # pylint: disable=broad-exception-caught
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
