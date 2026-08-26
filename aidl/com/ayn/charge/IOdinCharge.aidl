// SPDX-License-Identifier: Apache-2.0
package com.ayn.charge;

import com.ayn.charge.ChargeResponse;

interface IOdinCharge {
    ChargeResponse getStatus();
    ChargeResponse setMode(int mode);
    // Refused whole when the pair is outside what the policy accepts, so a
    // rejected threshold changes nothing. The accepted values come back in the
    // response's stopPercent and resumePercent.
    ChargeResponse setThresholds(int stopPercent, int resumePercent);
}
