// SPDX-License-Identifier: Apache-2.0
package com.ayn.charge;

import com.ayn.charge.ChargeResponse;

interface IOdinCharge {
    ChargeResponse getStatus();
    ChargeResponse setMode(int mode);
}
