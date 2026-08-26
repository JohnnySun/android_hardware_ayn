// SPDX-License-Identifier: Apache-2.0
package com.ayn.controller;

import com.ayn.controller.ControllerProfileResponse;
import com.ayn.controller.IOdinControllerListener;

interface IOdinController {
    ControllerProfileResponse getProfile();
    ControllerProfileResponse setProfile(int profile);

    /**
     * Listen for events the daemon sees before the framework does.
     *
     * Registering twice with the same binder is idempotent. A listener whose
     * process dies is dropped without the caller having to say so.
     */
    void registerListener(IOdinControllerListener listener);
    void unregisterListener(IOdinControllerListener listener);
}
