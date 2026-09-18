"""The shared schema must describe actual payloads and retain Linux compatibility."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    "manifest", Path(__file__).resolve().parents[1] / "scripts/generate-manifest.py")
manifest = importlib.util.module_from_spec(spec)
spec.loader.exec_module(manifest)


class ManifestTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        for platform, suffixes in (("linux-x86_64", ("so", "bin")), ("windows-x86_64", ("dll", "exe"))):
            for component, suffix in zip(("plugin", "updater"), suffixes):
                name = f"bokis-twitch-chat-{component}-1.2.3-{platform}.{suffix}"
                data = name.encode()
                (self.root / name).write_bytes(data)
                (self.root / (name + ".sha256")).write_text(f"{hashlib.sha256(data).hexdigest()}  {name}\n")

    def test_both_platforms(self):
        data = manifest.generate(self.root, "1.2.3", "owner/repo", ["linux-x86_64", "windows-x86_64"])
        self.assertEqual(data["schema"], 1)
        self.assertEqual(data["version"], "1.2.3")
        for platform, entry in data["platforms"].items():
            for artifact in (entry, entry["helper"]):
                name = artifact["url"].rsplit("/", 1)[1]
                self.assertEqual(artifact["size"], (self.root / name).stat().st_size)
                self.assertIn("/v1.2.3/", artifact["url"])
        self.assertEqual(data["platforms"]["windows-x86_64"]["obs"]["minVersion"], "32.2.2")
        self.assertEqual(json.loads(json.dumps(data)), data)

    def test_linux_only_retains_schema(self):
        data = manifest.generate(self.root, "1.2.3", "owner/repo", ["linux-x86_64"])
        self.assertEqual(list(data["platforms"]), ["linux-x86_64"])
        self.assertTrue(data["platforms"]["linux-x86_64"]["helper"]["url"].endswith(".bin"))

    def test_corrupt_or_missing_asset(self):
        name = self.root / "bokis-twitch-chat-plugin-1.2.3-windows-x86_64.dll"
        name.write_bytes(b"corrupt")
        with self.assertRaisesRegex(ValueError, "Checksum mismatch"):
            manifest.generate(self.root, "1.2.3", "owner/repo", ["windows-x86_64"])
        name.unlink()
        with self.assertRaises(FileNotFoundError):
            manifest.generate(self.root, "1.2.3", "owner/repo", ["windows-x86_64"])

    def test_invalid_release_identity(self):
        with self.assertRaises(ValueError):
            manifest.generate(self.root, "../bad", "owner/repo", ["linux-x86_64"])
        with self.assertRaises(ValueError):
            manifest.generate(self.root, "1.2.3", "https://other/owner/repo", ["linux-x86_64"])


if __name__ == "__main__":
    unittest.main()
