#!/usr/bin/env python3
# Copyright 2025 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import datetime
import re
import subprocess
import sys


def _get_new_files():
    result = subprocess.run(
        ["git", "diff", "--raw", "--name-status", "HEAD"],
        capture_output=True,
        check=True,
    )
    lines = result.stdout.split(b"\n")
    new_files = []
    for line in lines:
        if line:
            items = line.decode("utf-8").split("\t")
            if items[0] == "A":
                new_files.append(items[1])
    return new_files


def _check_license(files):
    """Verifies the Chromium OS license/copyright header.

    Should be following the spec:
    http://dev.chromium.org/developers/coding-style#TOC-File-headers
    """

    LICENSE_HEADER = (
        # Line 1 - copyright.
        r".*Copyright(?P<copyright> \(c\))? "
        r"(?P<year>20[0-9]{2})(?:-20[0-9]{2})? "
        r"The Chromium(?P<chromium_space_os> )?OS Authors(?P<period>\.)?"
        r"(?P<rights_reserved> All rights reserved\.)?\n"
        # Line 2 - License.
        r".*Use of this source code is governed by a BSD-style license that "
        r"can be\n"
        # Line 3 - License continuation.
        r".*found in the LICENSE file\.\n"
    )
    license_re = re.compile(LICENSE_HEADER, re.MULTILINE)

    bad_files = []
    bad_copyright_files = []
    bad_year_files = []
    bad_chromiumos_files = []
    bad_rights_reserved_files = []
    bad_period_files = []

    new_files = _get_new_files()

    current_year = datetime.datetime.now().year
    for filename in files:
        contents = None
        try:
            with open(filename, "r", encoding="utf-8") as fh:
                contents = fh.read()
        except UnicodeDecodeError:
            pass
        if not contents:
            # Ignore empty files.
            continue

        license_match = license_re.search(contents)
        if not license_match:
            bad_files.append(filename)
        else:
            new_file = filename in new_files
            year = int(license_match.group("year"))
            if license_match.group("copyright"):
                bad_copyright_files.append(filename)
            if new_file and year != current_year:
                bad_year_files.append(filename)
            if license_match.group("chromium_space_os"):
                bad_chromiumos_files.append(filename)
            if license_match.group("rights_reserved"):
                bad_rights_reserved_files.append(filename)
            if license_match.group("period"):
                bad_period_files.append(filename)

    if bad_files:
        msg = "%s:\n%s\n%s" % (
            "License must match",
            license_re.pattern,
            "Found a bad header in these files:",
        )
        print(msg, bad_files)
    if bad_copyright_files:
        msg = "Do not use (c) in copyright headers:"
        print(msg, bad_copyright_files)
    if bad_year_files:
        msg = "Use current year (%s) in copyright headers in new files:" % (
            current_year
        )
        print(msg, bad_year_files)
    if bad_chromiumos_files:
        msg = "Use ChromiumOS instead of Chromium OS:"
        print(msg, bad_chromiumos_files)
    if bad_rights_reserved_files:
        msg = 'Do not include "All rights reserved.":'
        print(msg, bad_rights_reserved_files)
    if bad_period_files:
        msg = 'Do not include period after "ChromiumOS Authors":'
        print(msg, bad_period_files)

    if (
        bad_files
        or bad_copyright_files
        or bad_year_files
        or bad_rights_reserved_files
        or bad_period_files
    ):
        sys.exit(1)


if __name__ == "__main__":
    _check_license(sys.argv[1:])
