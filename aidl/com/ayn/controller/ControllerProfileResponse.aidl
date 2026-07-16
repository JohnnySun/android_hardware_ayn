// SPDX-License-Identifier: Apache-2.0
package com.ayn.controller;

parcelable ControllerProfileResponse {
    const int PROFILE_STANDARD = 0;
    const int PROFILE_FLIPPED_FACE = 1;

    const int RESULT_OK = 0;
    const int RESULT_UNSUPPORTED_DEVICE = 1;
    const int RESULT_INVALID_PROFILE = 2;
    const int RESULT_BUSY = 3;
    const int RESULT_STORE_READ_FAILED = 4;
    const int RESULT_STORE_WRITE_FAILED = 5;
    const int RESULT_NOT_INITIALIZED = 6;

    int result = RESULT_NOT_INITIALIZED;
    int requestedProfile = PROFILE_STANDARD;
    int activeProfile = PROFILE_STANDARD;
}
