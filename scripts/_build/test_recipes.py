"""Project policy and license contracts across the shared dependency package."""
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import _recipes as recipes
from aria_deps import deps_build as deps

ROOT = Path(__file__).resolve().parents[2]


class RecipeTests(unittest.TestCase):
    def setUp(self):
        self.config = recipes.make_config(ROOT / "scripts/_recipes")
        deps.init_project(self.config)

    def test_real_lock_and_patches_support_every_platform_policy(self):
        lock = deps.read_resolved(ROOT / "dependencies.json")
        for platform in ("win32", "darwin", "linux"):
            for profile in ("runtime", "tests"):
                with self.subTest(platform=platform, profile=profile), \
                        patch.object(recipes.sys, "platform", platform):
                    selected = recipes.configure(recipes.RECIPES, profile)
                    locked = deps.locked_recipes(lock, selected, self.config)
                    self.assertTrue(deps.recipe_digest(locked))
                    self.assertEqual(len(locked), len({dep.name for dep in locked}))
                    self.assertEqual("doctest" in {dep.name for dep in locked}, profile == "tests")
                    seen = set()
                    for dep in locked:
                        self.assertLessEqual(set(dep.requires), seen)
                        seen.add(dep.name)

    def test_windows_default_schannel_does_not_pull_openssl(self):
        with patch.object(recipes.sys, "platform", "win32"):
            selected = {dep.name: dep for dep in recipes.configure(recipes.RECIPES)}
        self.assertNotIn("openssl", selected)
        self.assertEqual(selected["curl"].requires, ("zlib",))
        self.assertIn("-DCURL_USE_OPENSSL=OFF", selected["curl"].options)
        self.assertIn("-DCURL_USE_SCHANNEL=ON", selected["curl"].options)

    def test_macos_curl_keeps_native_certificate_trust(self):
        with patch.object(recipes.sys, "platform", "darwin"):
            selected = {dep.name: dep for dep in recipes.configure(recipes.RECIPES)}
        self.assertIn("openssl", selected)
        self.assertIn("-DUSE_APPLE_SECTRUST=ON", selected["curl"].options)

    def test_license_hooks_preserve_embedded_notices_through_the_package(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source, prefix = root / "source", root / "prefix"
            headers = source / "include/nlohmann"
            headers.mkdir(parents=True)
            (source / "LICENSE.MIT").write_text("MIT fixture", encoding="utf-8")
            notice = "// SPDX-FileCopyrightText: 2026 Example Author"
            (headers / "json.hpp").write_text(notice + "\n// SPDX-License-Identifier: MIT\n", encoding="utf-8")
            dep = next(dep for dep in recipes.RECIPES if dep.name == "json")
            copied = deps.copy_licenses(prefix, source, dep)
            self.assertIn("ATTRIBUTIONS.txt", copied)
            self.assertIn(notice, (prefix / "share/licenses/json/ATTRIBUTIONS.txt").read_text(encoding="utf-8"))
            (source / "sqlite3.h").write_text(
                "The author disclaims copyright to this source code.\n" + "*" * 73,
                encoding="utf-8")
            dep = next(dep for dep in recipes.RECIPES if dep.name == "sqlite3")
            self.assertIn("PUBLIC-DOMAIN.txt", deps.copy_licenses(prefix, source, dep))

    def test_zlib_hook_preserves_static_linkage_on_windows(self):
        with tempfile.TemporaryDirectory() as folder:
            prefix = Path(folder)
            (prefix / "lib").mkdir()
            (prefix / "lib/zs.lib").write_bytes(b"static library fixture")
            (prefix / "lib/zlib.lib").write_bytes(b"dynamic import fixture")
            dep = next(dep for dep in recipes.RECIPES if dep.name == "zlib")
            with patch.object(recipes.sys, "platform", "win32"):
                dep.post_build(prefix, prefix, dep)
            self.assertEqual((prefix / "lib/zlibstatic.lib").read_bytes(), b"static library fixture")
            self.assertTrue((prefix / "lib/zs.lib").is_file())
            self.assertFalse((prefix / "lib/zlib.lib").exists())


if __name__ == "__main__":
    unittest.main()
