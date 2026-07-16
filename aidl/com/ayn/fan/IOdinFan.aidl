// SPDX-License-Identifier: Apache-2.0
package com.ayn.fan;

import com.ayn.fan.FanResponse;

interface IOdinFan {
    FanResponse getStatus();
    FanResponse setMode(int mode);
}
