#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class ControllerAidlContractTest(unittest.TestCase):
    def test_interface_imports_response_parcelable(self):
        source = (
            ROOT / "aidl/com/ayn/controller/IOdinController.aidl"
        ).read_text(encoding="utf-8")
        self.assertIn(
            "import com.ayn.controller.ControllerProfileResponse;", source
        )


class FanOwnershipContractTest(unittest.TestCase):
    def test_set_mode_has_no_client_owner_token(self):
        source = (
            ROOT / "aidl/com/ayn/fan/IOdinFan.aidl"
        ).read_text(encoding="utf-8")
        self.assertIn("FanResponse setMode(int mode);", source)
        self.assertNotIn("android.os.IBinder", source)
        self.assertNotIn("owner", source)

    def test_daemon_owns_mode_without_client_death_link(self):
        source = (ROOT / "src/odinfand.cpp").read_text(encoding="utf-8")
        self.assertNotIn("AIBinder_linkToDeath", source)
        self.assertNotIn("AIBinder_unlinkToDeath", source)
        self.assertNotIn("OwnerDied", source)

    def test_safe_default_precedes_binder_registration(self):
        source = (ROOT / "src/odinfand.cpp").read_text(encoding="utf-8")
        startup = source.index("InitializeSafeDefault()")
        registration = source.index("AServiceManager_addService")
        self.assertLess(startup, registration)
        between = source[startup:registration]
        self.assertIn("startup.result != ayn::fan::FanResult::kOk", between)
        self.assertIn("return EXIT_FAILURE", between)


if __name__ == "__main__":
    unittest.main()
