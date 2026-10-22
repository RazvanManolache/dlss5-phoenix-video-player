"""Focused failure checks for helper staging, without executing downloaded tools."""
from pathlib import Path
import subprocess
import shutil
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'tools/Prepare-MediaHelpers.ps1'
POWERSHELL = shutil.which('pwsh') or shutil.which('powershell')


class MediaHelperStagingTests(unittest.TestCase):
    def run_staging(self, destination, archive):
        self.assertIsNotNone(POWERSHELL, 'PowerShell is required, as in the native build')
        return subprocess.run([POWERSHELL, '-NoProfile', '-File', str(SCRIPT),
            '-DestinationDirectory', str(destination), '-SourceArchive', str(archive)],
            cwd=ROOT, capture_output=True, text=True, timeout=30)

    def test_different_existing_helper_is_preserved_without_installing_others(self):
        with tempfile.TemporaryDirectory(prefix='media-helper-test-', dir=ROOT / 'build') as work:
            folder = Path(work)
            destination = folder / 'player'
            destination.mkdir()
            existing = destination / 'yt-dlp.exe'
            existing.write_bytes(b'preserve an unrelated executable')
            result = self.run_staging(destination, folder / 'unused.zip')
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('preserved', result.stderr)
            self.assertEqual(existing.read_bytes(), b'preserve an unrelated executable')
            self.assertEqual([path.name for path in destination.iterdir()], ['yt-dlp.exe'])

    def test_bad_source_archive_publishes_no_tools(self):
        with tempfile.TemporaryDirectory(prefix='media-helper-test-', dir=ROOT / 'build') as work:
            folder = Path(work)
            archive = folder / 'bad.zip'
            archive.write_bytes(b'not the pinned package')
            destination = folder / 'player'
            result = self.run_staging(destination, archive)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('checksum differs', result.stderr)
            self.assertEqual(list(destination.iterdir()), [])


if __name__ == '__main__':
    unittest.main()
