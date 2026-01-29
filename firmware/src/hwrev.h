/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef HWREV_H_
#define HWREV_H_

/**
 * @brief Returns the hardware revision id from strap-pins
 * Value is read at boot and cached.
 *
 * @return int Revision id
 * -1 if the revision id was not yet read or some error happened
 */
int hwrev_read();

#endif /* HWREV_H_ */
