#!/usr/bin/env python3
"""generate_appcast.py 的单元测试：sparkle:version 必须等于构建号。

Sparkle 通过 `sparkle:version` 与本机 CFBundleVersion 比较，二者必须同源（构建号），
否则同版本重发不会被发现、跨版本也可能被判成更旧。shortVersionString 仍放语义版本。
"""
from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "release" / "generate_appcast.py"
NS = {"sparkle": "http://www.andymatuschak.org/xml-namespaces/sparkle"}


class AppcastTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.out = Path(self.tmp.name) / "appcast.xml"

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def generate(self, *args: str):
        return subprocess.run(
            [sys.executable, str(SCRIPT), "--output", str(self.out), *args],
            capture_output=True, text=True, check=False,
        )

    def enclosure(self) -> ET.Element:
        root = ET.parse(self.out).getroot()
        return root.find("./channel/item/enclosure")

    def test_sparkle_version_is_build(self) -> None:
        result = self.generate(
            "--version", "1.3.0", "--build", "202611210105",
            "--dmg-url", "https://dl.xiaotu.online/devcard/devcard-1.3.0.dmg",
            "--sig", "test-signature",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        enc = self.enclosure()
        self.assertEqual(enc.get("{%s}version" % NS["sparkle"]), "202611210105")
        self.assertEqual(enc.get("{%s}shortVersionString" % NS["sparkle"]), "1.3.0")

    def test_guid_is_build(self) -> None:
        result = self.generate(
            "--version", "1.3.0", "--build", "202611210105",
            "--dmg-url", "https://example.invalid/devcard.dmg", "--sig", "x",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        guid = ET.parse(self.out).getroot().find("./channel/item/guid")
        self.assertEqual(guid.text, "202611210105")

    def test_build_is_required(self) -> None:
        result = self.generate(
            "--version", "1.3.0",
            "--dmg-url", "https://example.invalid/devcard.dmg", "--sig", "x",
        )
        self.assertNotEqual(result.returncode, 0)

    def test_non_numeric_build_rejected(self) -> None:
        result = self.generate(
            "--version", "1.3.0", "--build", "2026-11-21",
            "--dmg-url", "https://example.invalid/devcard.dmg", "--sig", "x",
        )
        self.assertEqual(result.returncode, 2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
