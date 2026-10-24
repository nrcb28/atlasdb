#!/usr/bin/env python3
"""Local CLI framing and Unicode table rendering checks (no network access)."""

import json
import os
import pathlib
import subprocess
import sys
import unicodedata


BINARY = str(pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "build/atlasdb").resolve())


def run(sql, *, json_output=False, locale="C.UTF-8"):
    env = dict(os.environ, LC_ALL=locale)
    args = [BINARY, "--execute", sql]
    if json_output:
        args.append("--json")
    result = subprocess.run(args, capture_output=True, env=env, timeout=10)
    assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
    return result.stdout.decode("utf-8", errors="strict")


def width(text):
    return sum(0 if unicodedata.combining(c) else
               2 if unicodedata.east_asian_width(c) in ("W", "F") else 1
               for c in text)


def assert_aligned(output):
    lines = [line for line in output.splitlines() if line.startswith(("|", "+"))]
    assert lines, output
    assert len({width(line) for line in lines}) == 1, lines


for locale in ("C.UTF-8", "C"):
    output = run("CREATE TABLE t(id INT, name TEXT); "
                 "INSERT INTO t VALUES (1, 'Ratón'), (2, 'Teclado mecánico'), "
                 "(3, '日本語'), (4, '🚀' ), (5, 'é'); SELECT * FROM t;", locale=locale)
    assert "Ratón" in output and "Teclado mecánico" in output
    assert_aligned(output)

    # A four-byte character can straddle the old byte truncation boundary.
    text = "x" * 44 + "🚀" + "á" * 20
    output = run("CREATE TABLE t(name TEXT); INSERT INTO t VALUES ('" + text + "'); SELECT * FROM t;",
                 locale=locale)
    assert "..." in output
    assert_aligned(output)

header = "cabecera\ncon\ttab\x1b[31m"
quoted_header = '"' + header.replace('"', '""') + '"'
output = run(f"CREATE TABLE t({quoted_header} TEXT); INSERT INTO t VALUES ('dato'); SELECT * FROM t;")
assert r"cabecera\ncon\ttab\x1b[31m" in output
assert "\x1b" not in output
assert_aligned(output)

long_header = "á" * 43 + "🚀" + "界" * 10
output = run(f'CREATE TABLE t("{long_header}" TEXT); INSERT INTO t VALUES (\'dato\'); SELECT * FROM t;')
assert "..." in output
assert_aligned(output)

script = '''/* ignored ; */ CREATE TABLE "semi;table"(v TEXT);
-- ignored ;
INSERT INTO "semi;table" VALUES ('it''s; -- /* text */');
SELECT * FROM "semi;table" -- final statement without semicolon
'''
results = [json.loads(line) for line in run(script, json_output=True).splitlines()]
assert len(results) == 3
assert results[-1]["rows"] == [["it's; -- /* text */"]]
assert run("-- only a comment", json_output=True) == ""
assert run("", json_output=True) == ""
print("frontend: Unicode, escaped headers, alignment and script framing passed")
