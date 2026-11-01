#!/usr/bin/env python3
"""Exercise the real contest compile entry with isolated build-tool fixtures.

These checks verify forwarded flags and failure propagation, not a GPU build.
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]


@unittest.skipUnless(sys.platform == "linux", "requires Linux and Bash")
class CompileEntryTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="seekdb-compile-entry-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.home = self.root / "home"
        self.home.mkdir()
        self.workspace = self.root / "workspace with spaces"
        self.workspace.mkdir()
        self.task = self.root / "task"
        self.script = self.root / "compile.sh"
        # Write LF so a Windows checkout can be tested under WSL too.
        self.script.write_bytes((ROOT / ".github/script/seekdb/compile.sh").read_bytes().replace(b"\r\n", b"\n"))
        self.record = self.workspace / "args.json"
        (self.workspace / "build.sh").write_text(
            '#!/usr/bin/env bash\n'
            'if [[ "$1" == init ]]; then exit 0; fi\n'
            f'exec "{sys.executable}" "{self.root / "configure.py"}" "$@"\n')
        (self.root / "configure.py").write_text(
            'import json, os, pathlib, sys\n'
            'p=pathlib.Path.cwd()\n'
            '(p/"args.json").write_text(json.dumps(sys.argv[1:]))\n'
            'if os.environ.get("CONFIG_FAIL"): sys.exit(7)\n'
            'b=p/("build_"+sys.argv[1]); b.mkdir(exist_ok=True)\n'
            'flags=dict(a[2:].split("=",1) for a in sys.argv[2:] if a.startswith("-D"))\n'
            'if os.environ.get("IGNORE_GPU"): flags["OB_BUILD_HIPVS"]="OFF"\n'
            '(b/"CMakeCache.txt").write_text("".join(k+":UNINITIALIZED="+v+"\\n" for k,v in flags.items()))\n')
        self.make = self.root / "fixture-make"
        self.make.write_text(
            f'#!{sys.executable}\n'
            'import os, pathlib, sys\n'
            'p=pathlib.Path.cwd(); (p/"make-ran").touch()\n'
            'if os.environ.get("MAKE_FAIL"): sys.exit(9)\n'
            'if os.environ.get("MISSING_ARTIFACTS"): sys.exit(0)\n'
            'w=p/"src/observer/seekdb_hipvs_hnsw_worker"; w.parent.mkdir(parents=True)\n'
            'w.write_text("fixture"); w.chmod(0o755)\n'
            'b=p/"lib/libseekdb_hipvs_hnsw.so"; b.parent.mkdir(); b.write_text("fixture")\n')
        self.make.chmod(0o755)

    def run_entry(self, **overrides):
        env = dict(os.environ)
        for name in ("SEEKDB_BUILD_HIPVS", "FORWARDING_HOST", "SEEKDB_CACHE_NFS_ROOT",
                     "CONFIG_FAIL", "MAKE_FAIL", "IGNORE_GPU", "MISSING_ARTIFACTS"):
            env.pop(name, None)
        env.update(HOME=str(self.home), GITHUB_WORKSPACE=str(self.workspace),
                   SEEKDB_TASK_DIR=str(self.task), MAKE=str(self.make), MAKE_ARGS="-j1",
                   PACKAGE_TYPE="release")
        env.update(overrides)
        return subprocess.run(["bash", str(self.script)], env=env, text=True,
                              capture_output=True, timeout=30)

    def test_default_enables_gpu_and_preserves_paths(self):
        result = self.run_entry(HIPVS_ROOT="/custom hipvs", ROCM_ROOT="/custom rocm")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        args = json.loads(self.record.read_text())
        for argument in ("-DOB_BUILD_CUVS=ON", "-DOB_BUILD_HIPVS=ON",
                         "-DHIPVS_ROOT=/custom hipvs", "-DROCM_ROOT=/custom rocm"):
            self.assertIn(argument, args)
        self.assertIn("artifacts verified", result.stdout)

    def test_explicit_cpu_clears_gpu_flags_without_requiring_artifacts(self):
        result = self.run_entry(SEEKDB_BUILD_HIPVS="OFF", MISSING_ARTIFACTS="1")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        args = json.loads(self.record.read_text())
        self.assertIn("-DOB_BUILD_HIPVS=OFF", args)
        self.assertIn("-DOB_BUILD_CUVS=OFF", args)
        self.assertFalse(any(a.startswith("-DHIPVS_ROOT=") for a in args))

    def test_invalid_mode_fails_before_configuration(self):
        result = self.run_entry(SEEKDB_BUILD_HIPVS="auto")
        self.assertEqual(result.returncode, 2)
        self.assertFalse(self.record.exists())

    def test_missing_build_entry_cannot_report_success(self):
        (self.workspace / "build.sh").unlink()
        result = self.run_entry()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.record.exists())

    def test_ignored_gpu_flag_stops_before_make(self):
        result = self.run_entry(IGNORE_GPU="1")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("did not retain OB_BUILD_HIPVS=ON", result.stderr)
        self.assertFalse((self.workspace / "build_release/make-ran").exists())

    def test_missing_gpu_artifacts_cannot_report_success(self):
        result = self.run_entry(MISSING_ARTIFACTS="1")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("GPU build is incomplete", result.stderr)

    def test_configure_failure_is_preserved(self):
        result = self.run_entry(CONFIG_FAIL="1")
        self.assertEqual(result.returncode, 7)
        self.assertFalse((self.workspace / "build_release/make-ran").exists())

    def test_build_failure_is_preserved(self):
        result = self.run_entry(MAKE_FAIL="1")
        self.assertEqual(result.returncode, 9)


if __name__ == "__main__":
    unittest.main()
