#!/usr/bin/env python3
"""Opt-in Linux GUI smoke test using only a temporary OBS profile and local fixture."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--plugin", required=True, type=Path)
parser.add_argument("--output", required=True, type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
fixture = root / "local-fixtures/streamelements/ScrapbookChatWidget8.zip"
before = hashlib.sha256(fixture.read_bytes()).digest()
output = args.output.resolve()
output.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="boki-obs-smoke-") as temporary:
    work = Path(temporary)
    config = work / "config/obs-studio"
    (config / "basic/scenes").mkdir(parents=True)
    (config / "basic/profiles/Test").mkdir(parents=True)
    (config / "global.ini").write_text(
        "[General]\nFirstRun=true\nEnableAutoUpdates=false\n"
        "[Basic]\nProfile=Test\nProfileDir=Test\nSceneCollection=Test\nSceneCollectionFile=Test\n")
    (config / "basic/profiles/Test/basic.ini").write_text(
        "[General]\nName=Test\n[Video]\nBaseCX=640\nBaseCY=480\nOutputCX=640\nOutputCY=480\nFPSType=0\nFPSCommon=30\n"
        "[Audio]\nSampleRate=48000\nChannelSetup=Stereo\n"
        f"[SimpleOutput]\nFilePath={output}\n[AdvOut]\nRecFilePath={output}\n")
    (config / "basic/scenes/Test.json").write_text(json.dumps({
        "name": "Test", "sources": [], "modules": {"scripts-tool": [{
            "path": str(root / "tests/obs-widget-smoke.lua"), "settings": {"fixture": str(fixture)}}]}}))
    plugin = config / "plugins/bokis-twitch-chat-plugin"
    (plugin / "bin/64bit").mkdir(parents=True)
    shutil.copy2(args.plugin, plugin / "bin/64bit/bokis-twitch-chat-plugin.so")
    shutil.copytree(root / "data", plugin / "data")
    env = os.environ.copy()
    env.update(XDG_CONFIG_HOME=str(work / "config"), XDG_CACHE_HOME=str(work / "cache"), QT_QPA_PLATFORM="xcb")
    log_path = output / "obs-widget-smoke.log"
    with log_path.open("w") as log:
        process = subprocess.Popen([
            "obs", "--multi", "--disable-updater", "--disable-missing-files-check", "--profile", "Test", "--collection", "Test",
            "--no-first-run", "--no-default-browser-check", "--password-store=basic"], env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 40
            while process.poll() is None and time.monotonic() < deadline:
                if "SMOKE complete" in log_path.read_text(errors="replace"):
                    break
                time.sleep(0.2)
            if process.poll() is not None:
                raise RuntimeError(f"OBS exited before completion: {process.returncode}")
            process.send_signal(signal.SIGINT)
            process.wait(timeout=15)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
    text = log_path.read_text(errors="replace")
    assert process.returncode == 0, f"OBS exit: {process.returncode}"
    assert "SMOKE complete" in text, "Smoke script did not complete"
    statuses = [line for line in text.splitlines() if "SMOKE <span" in line]
    assert len(statuses) == 6 and all("Web Widget ready" in line for line in statuses), statuses
    assert "[WebWidget][Runtime] Initial package document served" in text
    assert "[WebWidget][Runtime] Document initialized; runtime Ready" in text
    assert "[WebWidget][Runtime] Web Widget failed" not in text
    assert "Saved screenshot to" in text, "OBS did not capture a source screenshot"
    assert hashlib.sha256(fixture.read_bytes()).digest() == before, "Fixture archive changed"
    print(f"OBS CEF smoke passed; six Ready checkpoints, reload, mode switching, two instances, deletion, clean exit. Log: {log_path}")
