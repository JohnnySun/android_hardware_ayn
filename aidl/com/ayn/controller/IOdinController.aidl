// SPDX-License-Identifier: Apache-2.0
package com.ayn.controller;

interface IOdinController {
    ControllerProfileResponse getProfile();
    ControllerProfileResponse setProfile(int profile);
}
