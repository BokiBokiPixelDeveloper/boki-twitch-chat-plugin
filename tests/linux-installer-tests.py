"""Offline installer/package integration tests; every write uses a temporary home."""

import fcntl
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile


SOURCE = Path(sys.argv.pop(1)).resolve()
BUILD = Path(sys.argv.pop(1)).resolve()
INSTALLER = SOURCE / "scripts/linux-installer.sh"
PAYLOAD = (
    "bin/64bit/bokis-twitch-chat-updater",
    "bin/64bit/bokis-twitch-chat-plugin.so",
    "data/licenses/NotoColorEmoji-OFL.txt",
    "uninstall.sh",
)
# Real ELF header; runtime commands are stubbed for distribution failure cases.
ELF = bytes.fromhex("7f454c4602010100000000000000000003003e00")


def write(path, data, mode=0o644):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data.encode() if isinstance(data, str) else data)
    path.chmod(mode)


def checksums(directory):
    return "".join(
        f"{hashlib.sha256((directory / name).read_bytes()).hexdigest()}  {name}\n"
        for name in sorted(PAYLOAD)
    )


class InstallerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="bokis installer ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.package = self.root / "release with spaces"
        self.home = self.root / "home"
        self.home.mkdir()
        self.config = self.home / "custom config"
        self.cache = self.home / "custom cache"
        self.target = self.config / "obs-studio/plugins/bokis-twitch-chat-plugin"
        self.pending = self.cache / "bokis-twitch-chat-plugin/pending"
        self.commands = self.root / "commands"
        self.commands.mkdir()
        self.env = {
            "PATH": f"{self.commands}:/usr/bin:/bin",
            "HOME": str(self.home),
            "XDG_CONFIG_HOME": str(self.config),
            "XDG_CACHE_HOME": str(self.cache),
            "LC_ALL": "C",
        }
        write(self.commands / "obs", ELF, 0o755)
        self.stub("timeout", 'printf "OBS Studio - %s\\n" "$TEST_OBS_VERSION"')
        self.stub("ldd", 'printf "%s\\n" "$TEST_LDD"')
        self.stub("pgrep", 'exit "$TEST_PGREP"')
        self.stub("uname", 'if [[ $1 == -s ]]; then echo Linux; else echo "$TEST_ARCH"; fi')
        self.env.update(TEST_OBS_VERSION="32.2.2", TEST_LDD="libQt6Core.so.6 => /usr/lib/libQt6Core.so.6",
                        TEST_PGREP="1", TEST_ARCH="x86_64")
        for name in PAYLOAD:
            if name.endswith(".txt"):
                content = "Test license\n"
            elif name == "uninstall.sh":
                content = INSTALLER.read_bytes()
            else:
                content = ELF + b"first release"
            write(self.package / "plugin" / name, content)
        write(self.package / "install.sh", INSTALLER.read_bytes())
        self.seal()

    def stub(self, name, body):
        write(self.commands / name, "#!/bin/bash\nset -eu\n" + body + "\n", 0o755)

    def seal(self):
        write(self.package / "SHA256SUMS", checksums(self.package / "plugin"))

    def run_installer(self, *args, success=True, script=None, message=None):
        result = subprocess.run(
            ["/bin/bash", str(script or self.package / "install.sh"), *args],
            cwd=self.home, env=self.env, text=True, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, timeout=15,
        )
        self.assertEqual(result.returncode == 0, success, result.stdout)
        if message:
            self.assertIn(message, result.stdout)
        return result

    def snapshot(self):
        return {name: (self.target / name).read_bytes() for name in PAYLOAD}

    def test_install_reinstall_uninstall_preserves_settings_and_locks(self):
        settings = self.config / "obs-studio/basic/scenes/scene.json"
        write(settings, '{"source_id":"bokis_twitch_chat_plugin"}')
        self.run_installer()
        self.assertEqual(self.snapshot(), {n: (self.package / "plugin" / n).read_bytes() for n in PAYLOAD})
        self.assertEqual((self.target / PAYLOAD[0]).stat().st_mode & 0o777, 0o755)
        self.assertEqual((self.target / PAYLOAD[2]).stat().st_mode & 0o777, 0o644)
        locks = [self.pending / "update.lock", self.target / f"{PAYLOAD[1]}.use.lock"]
        inodes = [p.stat().st_ino for p in locks]
        write(self.target / "custom-resource.txt", "preserve me")
        # The updater can replace binaries independently of the archive.
        write(self.target / PAYLOAD[0], ELF + b"updated")
        self.run_installer()
        self.assertEqual((self.target / PAYLOAD[0]).read_bytes(), ELF + b"first release")
        installed_script = self.target / "uninstall.sh"
        shutil.rmtree(self.package)
        self.run_installer(script=installed_script)
        self.assertTrue(all(not (self.target / name).exists() for name in PAYLOAD))
        self.assertEqual(settings.read_text(), '{"source_id":"bokis_twitch_chat_plugin"}')
        self.assertEqual((self.target / "custom-resource.txt").read_text(), "preserve me")
        self.assertEqual(inodes, [p.stat().st_ino for p in locks])

    def test_check_does_not_create_installation_or_cache(self):
        self.run_installer("--check", message="No files were installed")
        self.assertFalse(self.config.exists())
        self.assertFalse(self.cache.exists())

    def test_tampered_package_leaves_installed_files_unchanged(self):
        self.run_installer()
        original = self.snapshot()
        write(self.package / "plugin" / PAYLOAD[0], ELF + b"tampered")
        self.run_installer(success=False, message="checksum verification failed")
        self.assertEqual(self.snapshot(), original)

    def test_missing_manifest_or_payload_is_rejected(self):
        (self.package / "SHA256SUMS").unlink()
        self.run_installer(success=False, message="SHA256SUMS is missing")
        self.seal()
        (self.package / "plugin" / PAYLOAD[0]).unlink()
        self.run_installer(success=False, message="Package file is missing")
        self.assertFalse(self.target.exists())

    def test_manifest_cannot_supply_extra_or_traversing_paths(self):
        with (self.package / "SHA256SUMS").open("a") as manifest:
            manifest.write("0" * 64 + "  ../../outside\n")
        self.run_installer(success=False, message="checksum verification failed")
        self.assertFalse(self.target.exists())

    def test_package_and_destination_symlinks_are_rejected(self):
        external = self.root / "external"
        write(external, "untouched")
        binary = self.package / "plugin" / PAYLOAD[0]
        binary.unlink()
        binary.symlink_to(external)
        self.run_installer(success=False, message="symbolic link")
        binary.unlink()
        write(binary, ELF)
        self.seal()
        self.target.parent.mkdir(parents=True)
        self.target.symlink_to(self.root / "elsewhere")
        self.run_installer(success=False, message="symbolic link")
        self.assertEqual(external.read_text(), "untouched")

    def test_incompatible_runtime_is_rejected_before_writes(self):
        for variable, value, message in (
            ("TEST_ARCH", "aarch64", "Linux x86_64"),
            ("TEST_OBS_VERSION", "31.1.0", "supports OBS 32.x"),
            ("TEST_OBS_VERSION", "33.0.0", "supports OBS 32.x"),
            ("TEST_LDD", "libQt6WebSockets.so.6 => not found", "runtime libraries"),
            ("TEST_LDD", "libQt6Core.so.6: version Qt_6.11 not found", "runtime libraries"),
            ("SNAP", "/snap/obs/1", "native OBS only"),
        ):
            with self.subTest(variable=variable, value=value):
                original = self.env.copy()
                self.env[variable] = value
                self.run_installer(success=False, message=message)
                self.env = original
                self.assertFalse(self.target.exists())

    def test_native_obs_wrapper_and_non_elf_payload_rejected(self):
        write(self.commands / "obs", "#!/bin/sh\nflatpak run com.obsproject.Studio\n", 0o755)
        self.run_installer(success=False, message="ELF binary")
        write(self.commands / "obs", ELF, 0o755)
        write(self.package / "plugin" / PAYLOAD[0], "not a binary")
        self.seal()
        self.run_installer(success=False, message="ELF binary")

    def test_running_obs_and_process_check_failure_refuse_install(self):
        for code in ("0", "2"):
            self.env["TEST_PGREP"] = code
            self.run_installer(success=False)
            self.assertFalse(self.target.exists())

    def test_pending_update_or_journal_blocks_install_and_uninstall(self):
        self.run_installer()
        original = self.snapshot()
        for name in ("pending.json", "transaction.json"):
            write(self.pending / name, "{}")
            self.run_installer(success=False, message="pending or needs recovery")
            self.run_installer("--uninstall", success=False, message="pending or needs recovery")
            self.assertEqual(self.snapshot(), original)
            (self.pending / name).unlink()

    def test_actual_updater_and_plugin_locks_block_changes(self):
        self.run_installer()
        original = self.snapshot()
        for lock in (self.pending / "update.lock", self.target / f"{PAYLOAD[1]}.use.lock"):
            with lock.open("r+") as held:
                fcntl.flock(held, fcntl.LOCK_EX | fcntl.LOCK_NB)
                self.run_installer(success=False)
                self.run_installer("--uninstall", success=False)
                self.assertEqual(self.snapshot(), original)

    def test_relative_xdg_paths_use_home_defaults(self):
        self.env["XDG_CONFIG_HOME"] = "relative-config"
        self.env["XDG_CACHE_HOME"] = "relative-cache"
        self.run_installer()
        self.assertTrue((self.home / ".config/obs-studio/plugins/bokis-twitch-chat-plugin" / PAYLOAD[0]).is_file())
        self.assertTrue((self.home / ".cache/bokis-twitch-chat-plugin/pending/update.lock").is_file())
        self.assertFalse((self.home / "relative-config").exists())

    def test_failed_replacement_restores_both_binaries(self):
        self.run_installer()
        original = self.snapshot()
        for name in PAYLOAD[:2]:
            write(self.package / "plugin" / name, ELF + b"second release")
        self.seal()
        self.stub("mv", "\n".join([
            'for arg in "$@"; do',
            '  if [[ $arg == */.installer-transaction/new/*plugin.so ]]; then exit 1; fi',
            'done',
            'exec /usr/bin/mv "$@"',
        ]))
        self.run_installer(success=False)
        self.assertEqual(self.snapshot(), original)
        self.assertFalse((self.target / ".installer-transaction").exists())

    def test_failed_first_install_removes_partial_binaries(self):
        self.stub("mv", 'for arg in "$@"; do [[ $arg != */new/*plugin.so ]] || exit 1; done\nexec /usr/bin/mv "$@"')
        self.run_installer(success=False)
        self.assertTrue(all(not (self.target / name).exists() for name in PAYLOAD))

    def test_failed_uninstall_rolls_back(self):
        self.run_installer()
        original = self.snapshot()
        self.stub("rm", 'for arg in "$@"; do [[ $arg != */bin/64bit/*plugin.so ]] || exit 1; done\nexec /usr/bin/rm "$@"')
        self.run_installer("--uninstall", success=False)
        self.assertEqual(self.snapshot(), original)

    def test_staged_tampering_is_rejected_before_replacement(self):
        self.run_installer()
        original = self.snapshot()
        self.stub("install", '/usr/bin/install "$@"\nfor last in "$@"; do :; done\nprintf tampered >> "$last"')
        self.run_installer(success=False, message="checksum verification failed")
        self.assertEqual(self.snapshot(), original)

    def test_unfinished_installer_transaction_blocks_retry(self):
        self.run_installer()
        original = self.snapshot()
        write(self.target / ".installer-transaction/old/keep", "backup")
        self.run_installer(success=False, message="interrupted installation")
        self.run_installer("--uninstall", success=False, message="interrupted installation")
        self.assertEqual(self.snapshot(), original)

    def test_failed_rollback_retains_all_originals_for_manual_recovery(self):
        self.run_installer()
        original = self.snapshot()
        for name in PAYLOAD[:2]:
            write(self.package / "plugin" / name, ELF + b"second release")
        self.seal()
        self.stub("mv", '\n'.join([
            'if [[ $3 == */new/data/* ||',
            '      ( $3 == */.installer-transaction/restore && $4 == */bokis-twitch-chat-updater ) ]]; then',
            '  exit 1',
            'fi',
            'exec /usr/bin/mv "$@"',
        ]))
        self.run_installer(success=False, message="Rollback failed")
        transaction = self.target / ".installer-transaction"
        self.assertEqual({n: (transaction / "old" / n).read_bytes() for n in PAYLOAD}, original)
        self.assertEqual((self.target / PAYLOAD[1]).read_bytes(), original[PAYLOAD[1]])
        self.run_installer(success=False, message="interrupted installation")

    def test_termination_during_replacement_rolls_back(self):
        self.run_installer()
        original = self.snapshot()
        self.stub("mv", 'if [[ $3 == */new/*plugin.so ]]; then kill -TERM "$PPID"; exit 1; fi\nexec /usr/bin/mv "$@"')
        self.run_installer(success=False)
        self.assertEqual(self.snapshot(), original)
        self.assertFalse((self.target / ".installer-transaction").exists())

    def test_non_regular_lock_is_rejected_without_blocking(self):
        self.pending.mkdir(parents=True)
        os.mkfifo(self.pending / "update.lock")
        self.run_installer(success=False, message="lock is not a regular file")

    def test_help_invalid_arguments_and_absent_uninstall(self):
        self.run_installer("--help", message="without sudo")
        self.run_installer("--unknown", success=False, message="Unknown option")
        self.run_installer("--help", "--uninstall", success=False)
        shutil.rmtree(self.package / "plugin")
        self.run_installer("--uninstall", message="No user installation found")
        self.assertFalse(self.config.exists())

    def test_real_release_archive_contains_verified_installer_and_binaries(self):
        output = self.root / "packaged release"
        result = subprocess.run(
            ["bash", str(SOURCE / "scripts/package.sh"), str(BUILD), str(output)],
            capture_output=True, text=True, timeout=60,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for manifest in output.glob("*.sha256"):
            result = subprocess.run(["sha256sum", "--check", manifest.name], cwd=output, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        archive = next(output.glob("*.zip"))
        extracted = self.root / "extracted release"
        with zipfile.ZipFile(archive) as zipped:
            zipped.extractall(extracted)
            prefix = archive.stem + "/"
            for name in ("install.sh", "plugin/uninstall.sh", "plugin/" + PAYLOAD[0]):
                self.assertEqual(zipped.getinfo(prefix + name).external_attr >> 16 & 0o777, 0o755)
        self.package = extracted / archive.stem
        self.assertTrue((self.package / "INSTALL.md").is_file())
        self.run_installer()
        self.assertEqual((self.target / PAYLOAD[1]).read_bytes(), (BUILD / "bokis-twitch-chat-plugin.so").read_bytes())
        self.run_installer("--uninstall")


if __name__ == "__main__":
    # CI uses a root Arch container. Exercise the real root guard, then run all
    # install tests as an unprivileged UID with disposable, owned directories.
    if os.geteuid() == 0:
        result = subprocess.run(["bash", str(INSTALLER), "--uninstall"], capture_output=True, text=True)
        if result.returncode == 0 or "without sudo" not in result.stderr:
            raise AssertionError("Installer must refuse root")
        os.setgroups([])
        os.setgid(65534)
        os.setuid(65534)
    unittest.main()
