#!/usr/bin/env python3
"""Deterministic SQLite differential tests through AtlasDB's public C API.

Builds a temporary shared library by default; --library reuses an existing one.
Inputs intentionally stay within AtlasDB's documented strict SQL type system.
"""
from __future__ import annotations

import argparse
import ctypes
import json
import os
from pathlib import Path
import random
import shlex
import sqlite3
import subprocess
import tempfile


class Error(ctypes.Structure):
    _fields_ = [("code", ctypes.c_int), ("offset", ctypes.c_size_t),
                ("message", ctypes.c_char * 512)]


class Result(ctypes.Structure):
    _fields_ = [("columns", ctypes.c_void_p), ("ncolumns", ctypes.c_size_t),
                ("cells", ctypes.c_void_p), ("nrows", ctypes.c_size_t),
                ("capacity", ctypes.c_size_t), ("text_bytes", ctypes.c_size_t),
                ("affected", ctypes.c_uint64),
                ("message", ctypes.c_void_p)]


class Atlas:
    def __init__(self, library: Path):
        self.lib = ctypes.CDLL(str(library.resolve()))
        self.free = ctypes.CDLL(None).free
        self.free.argtypes = [ctypes.c_void_p]
        self.free.restype = None
        signatures = {
            "adb_open": ([ctypes.c_char_p, ctypes.POINTER(Error)], ctypes.c_void_p),
            "adb_session_create": ([ctypes.c_void_p, ctypes.POINTER(Error)], ctypes.c_void_p),
            "adb_execute": ([ctypes.c_void_p, ctypes.c_char_p, ctypes.POINTER(Result), ctypes.POINTER(Error)], ctypes.c_bool),
            "adb_result_json": ([ctypes.POINTER(Result)], ctypes.c_void_p),
            "adb_result_free": ([ctypes.POINTER(Result)], None),
            "adb_session_destroy": ([ctypes.c_void_p], None),
            "adb_close": ([ctypes.c_void_p], None),
        }
        for name, (arguments, returns) in signatures.items():
            function = getattr(self.lib, name)
            function.argtypes, function.restype = arguments, returns
        error = Error()
        self.database = self.lib.adb_open(b":memory:", ctypes.byref(error))
        if not self.database:
            raise RuntimeError(error.message.decode())
        self.session = self.lib.adb_session_create(self.database, ctypes.byref(error))
        if not self.session:
            self.lib.adb_close(self.database)
            raise RuntimeError(error.message.decode())

    def query(self, sql: str) -> list[list]:
        result, error = Result(), Error()
        try:
            if not self.lib.adb_execute(self.session, sql.encode(), ctypes.byref(result), ctypes.byref(error)):
                raise AssertionError(f"AtlasDB error {error.code}: {error.message.decode()}\n{sql}")
            pointer = self.lib.adb_result_json(ctypes.byref(result))
            if not pointer:
                raise MemoryError("Serializing query result")
            try:
                response = json.loads(ctypes.string_at(pointer))
            finally:
                self.free(pointer)
            return response["rows"]
        finally:
            self.lib.adb_result_free(ctypes.byref(result))

    def close(self) -> None:
        self.lib.adb_session_destroy(self.session)
        self.lib.adb_close(self.database)


def literal(value) -> str:
    if value is None:
        return "NULL"
    if isinstance(value, bool):
        return "TRUE" if value else "FALSE"
    if isinstance(value, str):
        return "'" + value.replace("'", "''") + "'"
    return repr(value)


def numeric_expression(rng: random.Random, depth: int) -> str:
    if depth <= 0 or rng.randrange(4) == 0:
        return rng.choice(["NULL", "0", "1", "-1", "2", "-7", "10", "0.5", "-1.25"])
    if rng.randrange(5) == 0:
        return "-(" + numeric_expression(rng, depth - 1) + ")"
    left, right = numeric_expression(rng, depth - 1), numeric_expression(rng, depth - 1)
    return f"({left}) {rng.choice(['+', '-', '*'])} ({right})"


def boolean_expression(rng: random.Random, depth: int) -> str:
    if depth <= 0 or rng.randrange(4) == 0:
        if rng.randrange(3) == 0:
            return rng.choice(["NULL", "TRUE", "FALSE"])
        left, right = numeric_expression(rng, 2), numeric_expression(rng, 2)
        return f"({left}) {rng.choice(['=', '<>', '<', '<=', '>', '>='])} ({right})"
    kind = rng.randrange(4)
    if kind == 0:
        return "NOT (" + boolean_expression(rng, depth - 1) + ")"
    if kind == 1:
        return "(" + numeric_expression(rng, 2) + ") IS " + rng.choice(["NULL", "NOT NULL"])
    left, right = boolean_expression(rng, depth - 1), boolean_expression(rng, depth - 1)
    return f"({left}) {rng.choice(['AND', 'OR'])} ({right})"


def compare(actual, expected, sql: str) -> None:
    if actual != expected:
        raise AssertionError(f"Differential mismatch\n{sql}\nAtlasDB: {actual!r}\nSQLite: {expected!r}")


def run_suite(atlas: Atlas, cases: int) -> int:
    reference = sqlite3.connect(":memory:")
    reference.execute("PRAGMA case_sensitive_like=ON")
    atlas.query("CREATE TABLE anchor (id INT PRIMARY KEY)")
    atlas.query("INSERT INTO anchor VALUES (1)")
    rng = random.Random(20261024)
    checks = 0
    try:
        # '%' in the input text is a regression case: it must not turn a
        # wildcard in the pattern into a literal match.
        fixed = [("%foo", "%"), ("%ba", "%a"), ("", "%"), ("", "_"),
                 ("a'b", "%''%"), ("A", "a"), ("abc", "a%%c")]
        pairs = fixed + [
            ("".join(rng.choices("abA%_' \\\n", k=rng.randrange(12))),
             "".join(rng.choices("abA%_' \\\n", k=rng.randrange(12))))
            for _ in range(cases)
        ]
        for text, pattern in pairs:
            expression = f"{literal(text)} LIKE {literal(pattern)}"
            sql = "SELECT " + expression
            compare(atlas.query(sql + " FROM anchor"), [list(row) for row in reference.execute(sql)], sql)
            checks += 1
        for _ in range(cases):
            expression = numeric_expression(rng, 4) if rng.randrange(2) else boolean_expression(rng, 4)
            sql = "SELECT " + expression
            compare(atlas.query(sql + " FROM anchor"), [list(row) for row in reference.execute(sql)], sql)
            checks += 1
        for left in ["NULL", "TRUE", "FALSE"]:
            for right in ["NULL", "TRUE", "FALSE"]:
                for op in ["AND", "OR"]:
                    sql = f"SELECT {left} {op} {right}"
                    compare(atlas.query(sql + " FROM anchor"), [list(row) for row in reference.execute(sql)], sql)
                    checks += 1

        schema = "CREATE TABLE edges (id INTEGER PRIMARY KEY, i INTEGER, r REAL, t TEXT, b BOOLEAN)"
        atlas.query(schema)
        reference.execute(schema)
        integers = [None, -(2**63), -(2**53 + 1), -(2**53), -1, 0, 1, 2**53, 2**53 + 1, 2**63 - 1]
        reals = [None, -float(2**63), -float(2**53), -1.5, -0.0, 0.5, float(2**53), float(2**53 + 2), float(2**63)]
        texts = [None, "", "%foo", "a", "a'b", "z"]
        booleans = [None, False, True]
        for ident in range(1, 61):
            values = [ident, integers[ident % len(integers)], reals[ident % len(reals)],
                      texts[ident % len(texts)], booleans[ident % len(booleans)]]
            sql = "INSERT INTO edges VALUES (" + ",".join(literal(value) for value in values) + ")"
            atlas.query(sql)
            reference.execute(sql)
        predicates = []
        for column, values in [("i", integers + reals), ("r", integers + reals),
                               ("t", texts), ("b", booleans)]:
            for value in values:
                predicates.extend([f"{column}={literal(value)}", f"{literal(value)}={column}",
                                   f"{column}={literal(value)} AND id>0"])
            predicates.extend([f"{column} IS NULL", f"{column} IS NOT NULL"])
        queries = ["SELECT id,i,r,t,b FROM edges WHERE " + predicate + " ORDER BY id" for predicate in predicates]
        baseline = []
        for sql in queries:
            rows = atlas.query(sql)
            compare(rows, [list(row) for row in reference.execute(sql)], sql)
            baseline.append(rows)
            checks += 1
        for column in ["i", "r", "t", "b"]:
            atlas.query(f"CREATE INDEX edge_{column} ON edges({column})")
        for sql, before in zip(queries, baseline):
            compare(atlas.query(sql), before, sql + " [indexed versus scan]")
            checks += 1
    finally:
        reference.close()
    return checks


def verify_layout(directory: Path) -> None:
    """Check the Python ABI against the current public header before any call.

    A C structure growing without a corresponding ctypes change would otherwise
    permit adb_execute to overwrite the Python allocation. Check every offset as
    well as size/alignment, including the error structure used on failure paths.
    """
    root = Path(__file__).resolve().parents[1]
    structures = [("ADBError", Error), ("ADBResult", Result)]
    lines = ['#include "atlasdb.h"', '#include <stddef.h>', '#include <stdio.h>',
             'int main(void) {']
    for name, structure in structures:
        lines.append(f'printf("{name} %zu %zu", sizeof({name}), _Alignof({name}));')
        for field, _ in structure._fields_:
            lines.append(f'printf(" %zu", offsetof({name}, {field}));')
        lines.append('puts("");')
    lines.append('return 0; }')
    source, executable = directory / "layout.c", directory / "layout"
    source.write_text("\n".join(lines), encoding="utf-8")
    command = shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c17", "-Wall", "-Wextra", "-Werror", "-I" + str(root / "include"),
        str(source), "-o", str(executable),
    ]
    subprocess.run(command, check=True, timeout=30)
    probe = subprocess.run([str(executable)], check=True, capture_output=True,
                           text=True, timeout=10)
    observed = {line.split()[0]: tuple(map(int, line.split()[1:]))
                for line in probe.stdout.splitlines()}
    for name, structure in structures:
        expected = (ctypes.sizeof(structure), ctypes.alignment(structure)) + tuple(
            getattr(structure, field).offset for field, _ in structure._fields_)
        if observed.get(name) != expected:
            raise RuntimeError(f"ctypes layout mismatch for {name}: "
                               f"header={observed.get(name)}, Python={expected}; "
                               "update the test structure before calling the C API")


def build_library(directory: Path) -> Path:
    root = Path(__file__).resolve().parents[1]
    output = directory / "libatlasdb-differential.so"
    sources = ["util", "parser", "index", "model", "engine", "storage", "database"]
    command = shlex.split(os.environ.get("CC", "cc")) + [
        "-D_POSIX_C_SOURCE=200809L", "-std=c17", "-O2", "-g", "-fPIC", "-shared",
        "-I" + str(root / "include"), "-I" + str(root / "src"),
    ] + [str(root / "src" / (name + ".c")) for name in sources] + ["-pthread", "-lm", "-o", str(output)]
    subprocess.run(command, check=True, timeout=120)
    return output


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", type=Path, help="Use this shared library instead of compiling one")
    parser.add_argument("--cases", type=int, default=10000, help="Random cases in each expression/LIKE family")
    args = parser.parse_args()
    if args.cases < 0 or args.cases > 1000000:
        parser.error("--cases must be between 0 and 1000000")
    with tempfile.TemporaryDirectory(prefix="atlasdb-differential-") as directory:
        verify_layout(Path(directory))
        library = args.library or build_library(Path(directory))
        atlas = Atlas(library)
        try:
            count = run_suite(atlas, args.cases)
        finally:
            atlas.close()
    print(f"AtlasDB differential: {count} comparisons passed (SQLite and indexed versus scan)")


if __name__ == "__main__":
    main()
