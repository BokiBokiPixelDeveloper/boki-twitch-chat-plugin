"""Create/verify the shared release manifest from the actual release payloads."""
import argparse
import hashlib
import json
from pathlib import Path
import re


def generate(directory, version, repository, platforms):
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
        raise ValueError("Invalid GitHub repository")
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?", version):
        raise ValueError("Invalid version")
    base = f"https://github.com/{repository}/releases/download/v{version}/"

    def artifact(name):
        path = directory / name
        data = path.read_bytes()
        if not data:
            raise ValueError(f"Empty artifact: {name}")
        digest = hashlib.sha256(data).hexdigest()
        if (directory / (name + ".sha256")).read_text().split()[0].lower() != digest:
            raise ValueError(f"Checksum mismatch: {name}")
        return {"url": base + name, "sha256": digest, "size": len(data)}

    entries = {}
    for platform in platforms:
        dll, exe = ("so", "bin") if platform == "linux-x86_64" else ("dll", "exe")
        entry = artifact(f"bokis-twitch-chat-plugin-{version}-{platform}.{dll}")
        entry["helper"] = artifact(f"bokis-twitch-chat-updater-{version}-{platform}.{exe}")
        if platform == "windows-x86_64":
            entry["obs"] = {"minVersion": "32.2.2", "maxMajorVersion": 32}
        entries[platform] = entry
    return {"schema": 1, "version": version, "channel": "stable", "platforms": entries,
            "obs": {"minVersion": "32.0.0", "maxMajorVersion": 32}}


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--platform", action="append", choices=["linux-x86_64", "windows-x86_64"])
    parser.add_argument("--verify", action="store_true")
    args = parser.parse_args()
    version = (Path(__file__).resolve().parents[1] / "VERSION").read_text().strip()
    data = generate(args.directory, version, args.repository,
                    args.platform or ["linux-x86_64", "windows-x86_64"])
    output = args.directory / "update-manifest.json"
    if args.verify:
        if json.loads(output.read_text()) != data:
            raise SystemExit("Manifest does not match the release artifacts")
    else:
        output.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
