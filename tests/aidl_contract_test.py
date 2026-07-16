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


if __name__ == "__main__":
    unittest.main()
