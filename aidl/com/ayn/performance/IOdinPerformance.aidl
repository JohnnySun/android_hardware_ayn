// SPDX-License-Identifier: Apache-2.0
package com.ayn.performance;

interface IOdinPerformance {
    PerformanceResponse getStatus();
    PerformanceResponse setMode(int mode);
}
