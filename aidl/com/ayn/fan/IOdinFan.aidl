// SPDX-License-Identifier: Apache-2.0
package com.ayn.fan;

import android.os.IBinder;
import com.ayn.fan.FanResponse;

@RequiresNoPermission
interface IOdinFan {
    FanResponse getStatus();
    FanResponse setMode(int mode, in IBinder owner);
}
