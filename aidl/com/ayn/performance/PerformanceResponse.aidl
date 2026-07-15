// SPDX-License-Identifier: Apache-2.0
package com.ayn.performance;

parcelable PerformanceResponse {
    const int MODE_SYSTEM_MANAGED = 0;
    const int MODE_STOCK_NORMAL = 1;
    const int MODE_PERFORMANCE = 2;
    const int MODE_HIGH = 3;

    const int RESULT_OK = 0;
    const int RESULT_UNSUPPORTED_DEVICE = 1;
    const int RESULT_UNEXPECTED_PATHS = 2;
    const int RESULT_NOT_INITIALIZED = 3;
    const int RESULT_ALREADY_INITIALIZED = 4;
    const int RESULT_INVALID_MODE = 5;
    const int RESULT_READ_FAILED = 6;
    const int RESULT_WRITE_FAILED = 7;
    const int RESULT_READBACK_FAILED = 8;
    const int RESULT_ROLLBACK_FAILED = 9;
    const int RESULT_MODE_UNAVAILABLE = 10;

    int result;
    int requestedMode;
    int activeMode;
    boolean initialized;
}
