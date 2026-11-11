#!/usr/bin/env python3
"""Package a prebuilt x64 GUI and an explicitly pinned Microsoft OpenSSH ZIP."""
import argparse
import hashlib
import json
import pathlib
import shutil
import tempfile
import urllib.request
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manager-exe", type=pathlib.Path, required=True)
    parser.add_argument("--engine-zip", type=pathlib.Path, help="Use an offline, hash-verified engine package")
    parser.add_argument("--output", type=pathlib.Path, default=ROOT / "dist")
    args = parser.parse_args()
    lock = json.loads((ROOT / "engine.lock.json").read_text())
    if not args.manager_exe.is_file():
        parser.error("Build OpenSSHManager.exe first")
    args.output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="openssh-manager-") as temporary:
        temp = pathlib.Path(temporary)
        archive = args.engine_zip
        if archive is None:
            archive = temp / "engine.zip"
            print(f"Downloading pinned {lock['channel']} engine: {lock['tag']}")
            urllib.request.urlretrieve(lock["url"], archive)
        actual = hashlib.sha256(archive.read_bytes()).hexdigest()
        if actual != lock["sha256"]:
            raise SystemExit(f"Engine SHA-256 mismatch: {actual}")
        package = temp / "OpenSSHManager"
        package.mkdir()
        engine = package / "engine"
        engine.mkdir()
        with zipfile.ZipFile(archive) as source:
            for entry in source.infolist():
                path = pathlib.PurePosixPath(entry.filename)
                if not path.parts or path.parts[0] != "OpenSSH-Win64" or ".." in path.parts or "\\" in entry.filename:
                    raise SystemExit(f"Unexpected engine archive entry: {entry.filename}")
                if entry.is_dir():
                    continue
                dest = engine.joinpath(*path.parts[1:])
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_bytes(source.read(entry))
        for required in ["sshd.exe", "sshd-session.exe", "sshd-auth.exe", "ssh-keygen.exe", "sftp-server.exe", "LICENSE.txt", "NOTICE.txt"]:
            if not (engine / required).is_file():
                raise SystemExit(f"Engine missing required file: {required}")
        shutil.copy2(args.manager_exe, package / "OpenSSHManager.exe")
        for name in ["Install.cmd", "Install.ps1", "Uninstall.ps1", "EnginePath.ps1", "EngineArchive.ps1", "Localization.ps1", "Test-Windows.ps1"]:
            shutil.copy2(ROOT / "scripts" / name, package / name)
        for name in ["README.md", "LICENSE", "engine.lock.json"]:
            shutil.copy2(ROOT / name, package / name)
        shutil.copy2(ROOT / "resources/app.svg", package / "OpenSSHManager.svg")
        hashes = {p.relative_to(package).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in sorted(package.rglob("*")) if p.is_file()}
        (package / "package-hashes.json").write_text(json.dumps(hashes, indent=2) + "\n", encoding="utf-8")
        output = args.output / "OpenSSHManager-0.1.23-win64.zip"
        with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as bundle:
            for file in sorted(package.rglob("*")):
                if file.is_file():
                    bundle.write(file, file.relative_to(temp))
        checksum = hashlib.sha256(output.read_bytes()).hexdigest()
        output.with_suffix(".zip.sha256").write_text(f"{checksum}  {output.name}\n", encoding="ascii")
        print(output)
        print(f"SHA-256: {checksum}")


if __name__ == "__main__":
    main()
