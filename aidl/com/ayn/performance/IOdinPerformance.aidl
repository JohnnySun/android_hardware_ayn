// SPDX-License-Identifier: Apache-2.0
package com.ayn.performance;

import com.ayn.performance.PerformanceResponse;

interface IOdinPerformance {
    PerformanceResponse getStatus();
    PerformanceResponse setMode(int mode);
}
