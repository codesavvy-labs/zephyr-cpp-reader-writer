import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


APP_DIR = Path(__file__).resolve().parents[1]
READER_NAMES = {"control", "diagnostics", "telemetry"}


class NativeSimTest(unittest.TestCase):
    def run_command(self, command, timeout):
        try:
            return subprocess.run(
                command,
                cwd=APP_DIR,
                env={**os.environ, "ZEPHYR_TOOLCHAIN_VARIANT": "host"},
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                timeout=timeout,
                check=False,
            )
        except subprocess.TimeoutExpired as error:
            self.fail(f"{command} exceeded {timeout}s\n{error.stdout!r}")

    def run_demo(self, *, disable_sync):
        with tempfile.TemporaryDirectory(prefix="zephyr-reader-writer-") as temp:
            temp_dir = Path(temp)
            build_dir = temp_dir / "build"
            config = temp_dir / "test.conf"
            config.write_text(
                "CONFIG_APP_TEST_DURATION_MS=500\n"
                f"CONFIG_APP_DISABLE_SYNC={'y' if disable_sync else 'n'}\n",
                encoding="utf-8",
            )
            build = self.run_command(
                [
                    "west", "build", "-b", "native_sim/native/64",
                    "-d", str(build_dir), str(APP_DIR), "--",
                    f"-DEXTRA_CONF_FILE={config}",
                ],
                timeout=120,
            )
            self.assertEqual(build.returncode, 0, build.stdout)
            run = self.run_command([str(build_dir / "zephyr" / "zephyr.exe")], timeout=10)

        summaries = [
            line.removeprefix("SUMMARY ")
            for line in run.stdout.splitlines()
            if line.startswith("SUMMARY ")
        ]
        self.assertEqual(len(summaries), 1, run.stdout)
        return run, json.loads(summaries[0])

    def assert_worker_progress(self, summary):
        readers = summary["readers"]
        self.assertEqual(set(readers), READER_NAMES)
        for name, counts in readers.items():
            self.assertGreater(counts["reads"], 0, name)
            self.assertLessEqual(counts["inconsistent_reads"], counts["reads"], name)
        self.assertEqual(summary["reads"], sum(r["reads"] for r in readers.values()))
        self.assertEqual(
            summary["inconsistent_reads"],
            sum(r["inconsistent_reads"] for r in readers.values()),
        )
        self.assertGreater(summary["writes"], 0)
        self.assertGreaterEqual(summary["max_concurrent_readers"], 2)
        self.assertLessEqual(summary["max_concurrent_readers"], len(READER_NAMES))

    def test_synchronized_readers_overlap_without_writer(self):
        run, summary = self.run_demo(disable_sync=False)
        self.assertEqual(run.returncode, 0, run.stdout)
        self.assertIs(summary["sync_enabled"], True)
        self.assert_worker_progress(summary)
        self.assertEqual(summary["inconsistent_reads"], 0)
        self.assertEqual(summary["exclusion_violations"], 0)
        self.assertGreater(summary["max_writer_acquisition_us"], 0)

    def test_unsynchronized_readers_detect_partial_writes(self):
        run, summary = self.run_demo(disable_sync=True)
        self.assertEqual(run.returncode, 1, run.stdout)
        self.assertIs(summary["sync_enabled"], False)
        self.assert_worker_progress(summary)
        self.assertGreater(summary["inconsistent_reads"], 0)
        self.assertGreater(summary["exclusion_violations"], 0)
        self.assertEqual(summary["max_writer_acquisition_us"], 0)


if __name__ == "__main__":
    unittest.main()
