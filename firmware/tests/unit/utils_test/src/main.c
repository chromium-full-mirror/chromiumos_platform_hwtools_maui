/* Copyright 2025 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "utils.h"

#include <zephyr/ztest.h>

ZTEST(utils_suite, test_add)
{
	zassert_equal(utils_add(2, 3), 5, "2 + 3 should equal 5");
	zassert_equal(utils_add(-1, 1), 0, "-1 + 1 should equal 0");
}

ZTEST_SUITE(utils_suite, NULL, NULL, NULL, NULL, NULL);
