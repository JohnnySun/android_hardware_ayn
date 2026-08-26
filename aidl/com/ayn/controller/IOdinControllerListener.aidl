// SPDX-License-Identifier: Apache-2.0
package com.ayn.controller;

/**
 * Told when the controller daemon sees something the framework never will.
 *
 * oneway throughout: the daemon raises these from the thread reading the UART,
 * and a listener that stalls must not be able to stall the pad.
 */
oneway interface IOdinControllerListener {
    /**
     * The overlay chord was pressed. Raised once per press, never repeated
     * while it is held, and the buttons that made it are swallowed so whatever
     * is running never sees them.
     */
    void onOverlayChord();
}
