#!/usr/bin/env python3
# Copyright 2025 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import base64
import os
import re
import sys
from urllib import request


UNBLOCKED_TERMS_FILE = "unblocked_terms.txt"


def _read_terms_from_array(lines):
    keywords = set()
    for line in lines:
        line = line.split("#", 1)[0]
        if not line:
            continue
        keywords.add(line)
    return keywords


def _read_terms_file(terms_file: str):
    """Read list of words from file, skipping comments and blank lines."""
    with open(terms_file, "r", encoding="utf-8") as fh:
        keywords = _read_terms_from_array(fh.readlines())

    return keywords


def _read_terms_from_gitiles(url: str):
    """Read list of words from gitiles."""
    response = request.urlopen(url)
    if response.getcode() != 200:
        print("Unable to get bad words list")
        sys.exit(1)
    encoded = response.read()
    decoded = base64.b64decode(encoded).decode("utf-8")
    lines = decoded.split("\n")
    return _read_terms_from_array(lines)


_cache = {}
_default_terms = set()


def _read_terms_from_cache(file):
    if not file:
        raise NameError("requesting terms for checking no file?")

    d = os.path.dirname(file)
    while True:
        terms_file = os.path.join(d, UNBLOCKED_TERMS_FILE)
        if os.path.isfile(terms_file):
            if d not in _cache:
                _cache[d] = _read_terms_file(terms_file)
            return _cache[d]
        d = os.path.dirname(d)
        if os.path.isdir(os.path.join(d, ".git")):
            break

    return _default_terms


def _check_keywords_in_file(file_to_check):
    """Checks there are no blocked keywords in a file being changed."""

    def _check_line(line):
        # Store information about each span matching blocking regex.
        # to match unblocked regex with blocked reg ex match.
        # [{'span':re.span,    - overlap of matching regex in line
        #   'group':re.group,  - matching term
        #   'blocked':bool,    - whether matching is blocked
        #   'keyword':regex,   - block regex
        #  }, ...]
        blocked_span = []
        # Store information about each span matching unblocking regex.
        # [re.span, ...]
        unblocked_span = []

        # Ignore lines that end with nocheck, typically in a comment.
        # This enables devs to bypass this check line by line.
        if line.endswith(" nocheck") or line.endswith(" nocheck */"):
            return False

        for word in keywords:
            for match in re.finditer(word, line, flags=re.I):
                blocked_span.append(
                    {
                        "span": match.span(),
                        "group": match.group(0),
                        "blocked": True,
                        "keyword": word,
                    }
                )

        # Unblock terms that are superset of blocked terms:
        #   blocked := "this.?word"
        #   unblocked := "\.this.?word"
        # "this line is blocked because of this1word"
        # "this line is unblocked because of thenew.this1word"
        #
        for b in blocked_span:
            for ub in unblocked_span:
                if ub[0] <= b["span"][0] and ub[1] >= b["span"][1]:
                    b["blocked"] = False
            if b["blocked"]:
                return f'Matched "{b["group"]}" with regex of "{b["keyword"]}"'
        return False

    keywords = _read_terms_from_cache(file_to_check)

    matches = []
    if file_to_check:
        try:
            with open(file_to_check, "r", encoding="utf-8") as fh:
                for line in fh.readlines():
                    result = _check_line(line.strip())
                    if result:
                        matches.append((file_to_check, result))
        except UnicodeDecodeError:
            pass

    if matches:
        for error_file, error in matches:
            print("File: %s Error: %s" % (error_file, error))
        raise sys.exit(1)


def main():
    global _default_terms
    _default_terms = _read_terms_from_gitiles(
        (
            "https://chromium.googlesource.com/chromiumos/"
            "repohooks/+/refs/heads/main/blocked_terms.txt?format=TEXT"
        )
    )
    for filename in sys.argv:
        if os.path.isfile(filename):
            _check_keywords_in_file(filename)


if __name__ == "__main__":
    main()
