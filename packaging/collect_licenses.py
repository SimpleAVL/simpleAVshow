#!/usr/bin/env python3
"""Include installed dependency license texts and package/version metadata."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess


def copy_notices(root, destination):
    root = root.resolve()
    for source in root.rglob("*"):
        if source.is_file() and any(word in source.name.upper()
                                    for word in ("LICENSE", "COPYING", "COPYRIGHT")):
            target = destination / source.relative_to(root)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("platform", choices=["macos", "linux", "windows"])
    parser.add_argument("destination", type=Path)
    parser.add_argument("--vcpkg", type=Path)
    parser.add_argument("--qt", type=Path)
    args = parser.parse_args()
    args.destination.mkdir(parents=True, exist_ok=True)
    shutil.copy2("packaging/README.txt", args.destination / "THIRD-PARTY.txt")
    shutil.copytree("packaging/licenses/qt", args.destination / "qt-license-texts", dirs_exist_ok=True)
    if args.platform == "macos":
        roots = ["qtbase", "ffmpeg", "rtaudio", "rtmidi"]
        metadata = json.loads(subprocess.check_output(
            ["brew", "info", "--json=v2", "--installed"], text=True))
        installed_formulae = {formula["full_name"]: formula for formula in metadata["formulae"]}
        pending, wanted = list(roots), set()
        while pending:
            name = pending.pop()
            if name in wanted:
                continue
            wanted.add(name)
            formula = installed_formulae.get(name)
            if formula is None:
                raise RuntimeError(f"Installed runtime dependency not found: {name}")
            for installed in formula["installed"]:
                pending.extend(dep["full_name"] for dep in installed.get("runtime_dependencies", []))
        # Use each installed bottle's recorded dependencies; today's formula may
        # have changed since that bottle was built (e.g. OpenSSL or xz support).
        metadata["formulae"] = [installed_formulae[name] for name in sorted(wanted)]
        metadata["casks"] = []
        (args.destination / "homebrew.json").write_text(json.dumps(metadata, indent=2) + "\n")
        cellar = Path(subprocess.check_output(["brew", "--cellar"], text=True).strip())
        for formula in metadata["formulae"]:
            for installed in formula["installed"]:
                copy_notices(cellar / formula["name"] / installed["version"],
                             args.destination / formula["name"])
    elif args.platform == "linux":
        # Distribution copyright files include dependency-specific license texts.
        for source in Path("/usr/share/doc").glob("*/copyright"):
            target = args.destination / source.parent.name / "copyright"
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
        metadata = subprocess.check_output(
            ["dpkg-query", "-W", "-f=${Package}\t${Version}\n"], text=True)
        (args.destination / "packages.txt").write_text(metadata)
        copy_notices(Path("/tmp/rtaudio"), args.destination / "rtaudio-6.0.1")
    else:
        if not args.vcpkg or not args.qt:
            parser.error("Windows requires --vcpkg and --qt")
        copy_notices(args.vcpkg / "share", args.destination / "vcpkg")
        copy_notices(args.qt, args.destination / "qt")
        shutil.copy2("vcpkg.json", args.destination / "vcpkg.json")


if __name__ == "__main__":
    main()
