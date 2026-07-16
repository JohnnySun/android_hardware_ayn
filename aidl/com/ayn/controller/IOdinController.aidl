// SPDX-License-Identifier: Apache-2.0
package com.ayn.controller;

import com.ayn.controller.ControllerProfileResponse;

interface IOdinController {
    ControllerProfileResponse getProfile();
    ControllerProfileResponse setProfile(int profile);
}
