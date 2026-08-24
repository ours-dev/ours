import hashlib
import sys
import tempfile
import unittest
from pathlib import Path
from urllib.parse import urljoin

sys.path.insert(0, str(Path(__file__).parent))

import activate


class ActivateTests(unittest.TestCase):
    def write_manifest(self, directory, content):
        manifest = Path(directory) / "prebuilt.yml"
        manifest.write_text(content, encoding="utf-8")
        return manifest

    def test_downloads_file_url_to_prebuilt(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.bin"
            source.write_bytes(b"artifact")
            manifest = self.write_manifest(
                root,
                "artifacts:\n"
                f"  - name: nested/tool.bin\n    url: {source.as_uri()}\n",
            )

            downloaded = activate.download_artifacts(manifest, root / "prebuilt")

            self.assertEqual(downloaded, [(root / "prebuilt/nested/tool.bin").resolve()])
            self.assertEqual((root / "prebuilt/nested/tool.bin").read_bytes(), b"artifact")

    def test_skips_existing_matching_checksum(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.bin"
            source.write_bytes(b"artifact")
            checksum = hashlib.sha256(b"artifact").hexdigest()
            destination = root / "prebuilt/tool.bin"
            destination.parent.mkdir()
            destination.write_bytes(b"artifact")
            manifest = self.write_manifest(
                root,
                "artifacts:\n"
                f"  - name: tool.bin\n    url: {source.as_uri()}\n    sha256: {checksum}\n",
            )

            self.assertEqual(activate.download_artifacts(manifest, root / "prebuilt"), [])

    def test_removes_partial_file_on_checksum_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.bin"
            source.write_bytes(b"artifact")
            manifest = self.write_manifest(
                root,
                "artifacts:\n"
                f"  - name: tool.bin\n    url: {source.as_uri()}\n    sha256: {'0' * 64}\n",
            )

            with self.assertRaises(activate.ActivationError):
                activate.download_artifacts(manifest, root / "prebuilt")

            self.assertFalse((root / "prebuilt/tool.bin").exists())
            self.assertFalse((root / "prebuilt/tool.bin.part").exists())

    def test_rejects_path_traversal(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.bin"
            source.write_bytes(b"artifact")
            manifest = self.write_manifest(
                root,
                "artifacts:\n"
                f"  - name: ../outside.bin\n    url: {source.as_uri()}\n",
            )

            with self.assertRaises(activate.ActivationError):
                activate.download_artifacts(manifest, root / "prebuilt")


if __name__ == "__main__":
    unittest.main()
