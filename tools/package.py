#!/usr/bin/env python3
"""Create a source-only archive, including docs, examples, tests and CI."""
from pathlib import Path
import argparse
import hashlib
import re
import zipfile


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    match = re.search(r'#define ADB_VERSION "([^"]+)"', (root / "include/atlasdb.h").read_text())
    if match is None:
        raise SystemExit("No se encuentra ADB_VERSION")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=root / "build" / f"atlasdb-{match[1]}.zip")
    args = parser.parse_args()
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    files = [root / name for name in ["README.md", "LICENSE", "Makefile", ".gitignore"]]
    for folder in ["include", "src", "tests", "examples", "docs", "tools", ".github"]:
        files.extend(path for path in (root / folder).rglob("*")
                     if path.is_file() and not path.is_symlink() and "__pycache__" not in path.parts)
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for path in sorted(files):
            if path.resolve() == output or path.suffix == ".pyc":
                continue
            item = zipfile.ZipInfo("atlasdb/" + path.relative_to(root).as_posix(), (2026, 10, 24, 0, 0, 0))
            item.compress_type = zipfile.ZIP_DEFLATED
            item.external_attr = 0o100644 << 16
            archive.writestr(item, path.read_bytes())
    with zipfile.ZipFile(output) as archive:
        if archive.testzip() is not None:
            raise SystemExit("El archivo ZIP no supera la verificación CRC")
        count = len(archive.namelist())
    digest = hashlib.sha256(output.read_bytes()).hexdigest()
    output.with_suffix(output.suffix + ".sha256").write_text(f"{digest}  {output.name}\n", encoding="ascii")
    print(f"{output} ({count} archivos, {output.stat().st_size} bytes)\nSHA256 {digest}")


if __name__ == "__main__":
    main()
