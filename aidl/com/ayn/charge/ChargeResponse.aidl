// SPDX-License-Identifier: Apache-2.0
package com.ayn.charge;

parcelable ChargeResponse {
    const int MODE_UNKNOWN = -1;
    // Charge normally; the daemon holds no restriction.
    const int MODE_OFF = 0;
    // Hold the battery between the resume and stop thresholds.
    const int MODE_LIMIT = 1;
    // Keep charging off while the adapter is attached, whatever the capacity.
    // This sets the charge current to zero rather than routing adapter power
    // around the pack, so a load heavier than the adapter still draws from the
    // battery, and the low-battery floor still releases the restriction.
    const int MODE_BYPASS = 2;

    const int RESULT_OK = 0;
    const int RESULT_UNSUPPORTED_DEVICE = 1;
    const int RESULT_UNEXPECTED_PATHS = 2;
    const int RESULT_INVALID_MODE = 3;
    const int RESULT_IO_ERROR = 4;
    const int RESULT_CAPACITY_UNAVAILABLE = 5;

    int result = RESULT_IO_ERROR;
    int mode = MODE_UNKNOWN;
    // -1 when the capacity could not be read.
    int capacity = -1;
    // 1 while charging is being held off, 0 otherwise, -1 when unknown.
    int restricted = -1;
    int stopPercent = -1;
    int resumePercent = -1;
}
