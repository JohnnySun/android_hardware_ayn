// SPDX-License-Identifier: Apache-2.0
package com.ayn.fan;

parcelable FanResponse {
    const int MODE_UNKNOWN = -1;
    const int MODE_OFF = 0;
    const int MODE_QUIET = 1;
    const int MODE_SPORT = 2;

    const int RESULT_OK = 0;
    const int RESULT_UNSUPPORTED_DEVICE = 1;
    const int RESULT_UNEXPECTED_PATHS = 2;
    const int RESULT_INVALID_MODE = 3;
    const int RESULT_INVALID_OWNER = 4;
    const int RESULT_IO_ERROR = 5;
    const int RESULT_TACH_TIMEOUT = 6;
    const int RESULT_DISABLE_UNCONFIRMED = 7;
    const int RESULT_NOT_OWNER = 8;

    int result = RESULT_IO_ERROR;
    int mode = MODE_UNKNOWN;
    int state = -1;
    int duty = -1;
    int tach = -1;
}
