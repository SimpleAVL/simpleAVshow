#!/usr/bin/env python3
"""Bundle non-system dylibs after macdeployqt, checking every Mach-O dependency."""
import argparse
from pathlib import Path
import shutil
import subprocess


def run(*args):
    return subprocess.check_output(args, text=True).strip()


def dependencies(path):
    return [line.strip().split(" (", 1)[0]
            for line in run("otool", "-L", str(path)).splitlines()[1:]]


def rpaths(path):
    lines = run("otool", "-l", str(path)).splitlines()
    return [lines[i + 2].strip().split(" (", 1)[0].removeprefix("path ")
            for i, line in enumerate(lines) if line.strip() == "cmd LC_RPATH"]


def system(path):
    return path.startswith(("/usr/lib/", "/System/Library/"))


def macho(path):
    return path.is_file() and not path.is_symlink() and "Mach-O" in run("file", "-b", str(path))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("app", type=Path)
    args = parser.parse_args()
    app = args.app.resolve()
    executable_dir = app / "Contents/MacOS"
    frameworks = app / "Contents/Frameworks"
    frameworks.mkdir(parents=True, exist_ok=True)
    licenses = app / "Contents/Resources/licenses"
    licenses.mkdir(parents=True, exist_ok=True)
    queue = [p for p in app.rglob("*") if macho(p)]
    seen = set()
    copied = {}
    formulae = set()
    executable_rpaths = rpaths(executable_dir / "simpleavshow")

    def resolve(dep, binary):
        expanded = dep.replace("@loader_path", str(binary.parent)).replace(
            "@executable_path", str(executable_dir))
        if expanded.startswith("@rpath/"):
            suffix = expanded.removeprefix("@rpath/")
            for root in rpaths(binary) + executable_rpaths + [str(frameworks)]:
                root = root.replace("@loader_path", str(binary.parent)).replace(
                    "@executable_path", str(executable_dir))
                candidate = Path(root) / suffix
                if candidate.exists():
                    return candidate.resolve()
            raise RuntimeError(f"Cannot resolve {dep} in {binary}")
        candidate = Path(expanded)
        if not candidate.exists():
            raise RuntimeError(f"Missing {dep} in {binary}")
        return candidate.resolve()

    while queue:
        binary = queue.pop()
        if binary in seen:
            continue
        seen.add(binary)
        binary.chmod(binary.stat().st_mode | 0o200)
        # Remove signatures before rewriting load commands; re-sign everything later.
        subprocess.run(["codesign", "--remove-signature", str(binary)],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for dep in dependencies(binary):
            if system(dep):
                continue
            source = resolve(dep, binary)
            if source == binary:  # dylib's own install name
                continue
            if source.is_relative_to(app):
                target = source
            else:
                if ".framework" in str(source):
                    raise RuntimeError(f"macdeployqt left an external framework: {source}")
                target = frameworks / source.name
                if target in copied and copied[target] != source:
                    raise RuntimeError(f"Dylib name collision: {source} and {copied[target]}")
                if target not in copied:
                    shutil.copy2(source, target)
                    copied[target] = source
                    queue.append(target)
                    if "/Cellar/" in str(source):
                        relative = str(source).split("/Cellar/", 1)[1].split("/")
                        formula, version = relative[:2]
                        formulae.add(formula)
                        root = Path(str(source).split("/Cellar/", 1)[0]) / "Cellar" / formula / version
                        destination = licenses / formula
                        destination.mkdir(exist_ok=True)
                        for notice in root.rglob("*"):
                            if notice.is_file() and any(word in notice.name.upper()
                                                       for word in ("LICENSE", "COPYING", "COPYRIGHT")):
                                output = destination / notice.relative_to(root)
                                output.parent.mkdir(parents=True, exist_ok=True)
                                shutil.copy2(notice, output)
            relative = target.relative_to(frameworks)
            new_dep = "@executable_path/../Frameworks/" + str(relative)
            if dep != new_dep:
                subprocess.check_call(["install_name_tool", "-change", dep, new_dep, str(binary)])
        if binary.parent == frameworks and binary.suffix == ".dylib":
            subprocess.check_call(["install_name_tool", "-id",
                                   "@rpath/" + binary.name, str(binary)])
        for root in rpaths(binary):
            if root.startswith(("/opt/homebrew/", "/usr/local/", "/Users/")):
                subprocess.check_call(["install_name_tool", "-delete_rpath", root, str(binary)])

    for binary in seen:
        for dep in dependencies(binary):
            if not system(dep) and not resolve(dep, binary).is_relative_to(app):
                raise RuntimeError(f"External runtime dependency: {binary}: {dep}")
    if formulae:
        metadata = run("brew", "info", "--json=v2", *sorted(formulae))
        (licenses / "homebrew.json").write_text(metadata + "\n")
    # Sign inside out. No Developer ID identity or notarization is implied.
    for binary in sorted(seen, key=lambda p: len(p.parts), reverse=True):
        subprocess.check_call(["codesign", "--force", "--sign", "-", str(binary)])
    subprocess.check_call(["codesign", "--force", "--deep", "--sign", "-", str(app)])
    subprocess.check_call(["codesign", "--verify", "--deep", "--strict", str(app)])
    print(f"Verified {len(seen)} Mach-O files; bundled {len(copied)} additional dylibs")


if __name__ == "__main__":
    main()
