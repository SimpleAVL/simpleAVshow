#!/usr/bin/env python3
"""Fetch a checksum-pinned shared SDK with native MSVC .lib import libraries."""
import argparse
import hashlib
import json
from pathlib import Path
import urllib.request
import zipfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    pin = json.loads(Path("packaging/ffmpeg-windows.json").read_text())
    args.destination.mkdir(parents=True, exist_ok=True)
    archive = args.destination / "ffmpeg-sdk.zip"
    if not archive.exists():
        print("Downloading " + pin["url"], flush=True)
        request = urllib.request.Request(pin["url"], headers={"User-Agent": "SimpleAVShow-build"})
        with urllib.request.urlopen(request, timeout=120) as response, archive.open("wb") as output:
            while chunk := response.read(1024 * 1024):
                output.write(chunk)
    digest = hashlib.sha256()
    with archive.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    if digest.hexdigest() != pin["sha256"]:
        archive.unlink()
        raise RuntimeError("FFmpeg SDK checksum mismatch")
    with zipfile.ZipFile(archive) as bundle:
        for member in bundle.infolist():
            target = (args.destination / member.filename).resolve()
            if not target.is_relative_to(args.destination.resolve()):
                raise RuntimeError("Unsafe path in FFmpeg archive")
        bundle.extractall(args.destination)
    sdk = args.destination / pin["directory"]
    for name in ("avformat", "avcodec", "avutil", "swresample", "swscale"):
        if not (sdk / "lib" / (name + ".lib")).is_file():
            raise RuntimeError("Missing MSVC import library: " + name)
    print("Verified prebuilt FFmpeg SDK: " + str(sdk.resolve()))
    print("No FFmpeg compilation is needed.")


if __name__ == "__main__":
    main()
