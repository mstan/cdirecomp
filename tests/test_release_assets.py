"""Packages must reject stale or missing launcher assets."""
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
from package_runtime_release import collect_assets

class LauncherAssets(unittest.TestCase):
    def test_exact_manifest_and_stale_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            assets=root/"assets"
            (assets/"img").mkdir(parents=True)
            image=assets/"img/title.png"
            image.write_bytes(b"fixture")
            manifest=root/"manifest.txt"
            manifest.write_text("img/title.png\n")
            self.assertEqual(set(collect_assets(assets,manifest)),{"assets/img/title.png"})
            stale=assets/"img/other-console.tga"
            stale.write_bytes(b"stale fixture")
            with self.assertRaises(SystemExit):collect_assets(assets,manifest)
            stale.unlink()
            image.unlink()
            with self.assertRaises(SystemExit):collect_assets(assets,manifest)

    def test_manifest_is_required(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(SystemExit):collect_assets(Path(directory),None)

if __name__=="__main__":unittest.main()
