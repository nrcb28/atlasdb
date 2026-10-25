#!/usr/bin/env python3
"""Black-box CLI, differential SQL, WAL recovery, and TCP session tests."""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import random
import signal
import socket
import sqlite3
import subprocess
import tempfile
import time


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


class Suite:
    def __init__(self, binary: Path):
        self.binary = str(binary.resolve())
        self.checks = 0

    def check(self, condition: bool, message: str) -> None:
        require(condition, message)
        self.checks += 1

    def cli(self, sql: str, db: str = ":memory:", success: bool = True) -> list[dict]:
        proc = subprocess.run(
            [self.binary, "--db", db, "--json", "--execute", sql],
            text=True, capture_output=True, timeout=30,
        )
        self.check((proc.returncode == 0) == success,
                   f"exit={proc.returncode}, stderr={proc.stderr}, stdout={proc.stdout}, SQL={sql[:200]}")
        objects = [json.loads(line) for line in proc.stdout.splitlines() if line.strip()]
        if success:
            self.check(bool(objects) and all(item.get("ok") for item in objects),
                       f"missing successful JSON response: {proc.stdout!r}")
        return objects

    def scripts_and_json(self) -> None:
        script = """
-- A semicolon inside a string must not split a statement.
CREATE TABLE notes (id INTEGER PRIMARY KEY, message TEXT);
/* A comment contains ; and ' and \" */
INSERT INTO notes VALUES (1,'a;b'),(2,'O''Brien'),(3,'línea\nsegunda');
SELECT id,message FROM notes ORDER BY id;
"""
        with tempfile.TemporaryDirectory(prefix="atlasdb-cli-") as directory:
            source = Path(directory, "script.sql")
            source.write_text(script, encoding="utf-8")
            proc = subprocess.run([self.binary, "--json", "--file", str(source)],
                                  text=True, capture_output=True, timeout=15)
            self.check(proc.returncode == 0, proc.stderr + proc.stdout)
            objects = [json.loads(line) for line in proc.stdout.splitlines() if line.strip()]
            self.check(objects[-1]["rows"] == [[1, "a;b"], [2, "O'Brien"], [3, "línea\nsegunda"]],
                       f"script framing/JSON failed: {objects!r}")
        result = self.cli("CREATE TABLE values_table (x INTEGER); INSERT INTO values_table VALUES (42); SELECT x AS answer FROM values_table")[-1]
        self.check(result["columns"] == ["answer"] and result["rows"] == [[42]], "aliases/result JSON")
        self.cli("CREATE TABLE x (id INT PRIMARY KEY); INSERT INTO x VALUES (1),(1)", success=False)

    def differential_sql(self) -> None:
        rng = random.Random(20261024)
        sqlite = sqlite3.connect(":memory:")
        statements = ["CREATE TABLE ledger (id INTEGER PRIMARY KEY, amount INTEGER, label TEXT)"]
        for ident in range(1, 81):
            statements.append(f"INSERT INTO ledger VALUES ({ident},{rng.randrange(-100,101)},'group{ident % 7}')")
        for _ in range(100):
            ident = rng.randrange(1, 81)
            delta = rng.randrange(-9, 10)
            statements.append(f"UPDATE ledger SET amount=amount+({delta}) WHERE id={ident}")
        statements.extend([
            "DELETE FROM ledger WHERE amount < -90",
            "SELECT id,amount,label FROM ledger WHERE amount>=-70 AND amount<=85 ORDER BY id DESC LIMIT 30 OFFSET 3",
            "SELECT COUNT(*),SUM(amount),MIN(amount),MAX(amount),AVG(amount) FROM ledger",
        ])
        for statement in statements[:-2]:
            sqlite.execute(statement)
        expected_rows = [list(row) for row in sqlite.execute(statements[-2])]
        expected_aggregate = [list(row) for row in sqlite.execute(statements[-1])]
        actual = self.cli(";\n".join(statements) + ";")
        self.check(actual[-2]["rows"] == expected_rows, "deterministic differential rows disagree with SQLite")
        for actual_value, expected_value in zip(actual[-1]["rows"][0], expected_aggregate[0]):
            self.check(abs(actual_value - expected_value) < 1e-8, "differential aggregate disagrees with SQLite")
        sqlite.close()

    def wal_recovery(self) -> None:
        with tempfile.TemporaryDirectory(prefix="atlasdb-wal-") as directory:
            self.cli("CREATE TABLE saved (id INTEGER PRIMARY KEY, text TEXT); INSERT INTO saved VALUES (1,'durable')", directory)
            wal = Path(directory, "atlas.wal")
            original_size = wal.stat().st_size
            with wal.open("ab") as file:
                file.write(b"ATLAS")
            rows = self.cli("SELECT * FROM saved", directory)[-1]["rows"]
            self.check(rows == [[1, "durable"]], "valid commits lost after torn WAL header")
            self.check(wal.stat().st_size == original_size, "torn WAL header was not truncated")
            self.cli("CHECKPOINT", directory)
            self.check(wal.stat().st_size == 0, "checkpoint did not compact WAL")
            self.check(Path(directory, "atlas.snapshot").is_file(), "checkpoint missing snapshot")
            self.cli("INSERT INTO saved VALUES (2,'after snapshot')", directory)
            valid = wal.read_bytes()
            self.check(len(valid) > 32, "WAL frame is missing payload")
            # The first payload byte is present in a complete frame: corruption must fail closed.
            damaged = bytearray(valid)
            damaged[32] ^= 0x80
            wal.write_bytes(damaged)
            self.cli("SELECT * FROM saved", directory, success=False)
            wal.write_bytes(valid)
            self.check(self.cli("SELECT COUNT(*) FROM saved", directory)[-1]["rows"] == [[2]], "restored frame failed recovery")

    def start_server(self, directory: str = ":memory:") -> tuple[subprocess.Popen, int]:
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        proc = subprocess.Popen([self.binary, "--db", directory, "--server", str(port), "--host", "127.0.0.1"],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            if proc.poll() is not None:
                stdout, stderr = proc.communicate()
                raise AssertionError(f"server exited: {stdout}\n{stderr}")
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                    return proc, port
            except OSError:
                time.sleep(0.03)
        proc.kill()
        proc.communicate()
        raise AssertionError("server did not listen within 8 seconds")

    def remote_bind_guard(self) -> None:
        proc = subprocess.run(
            [self.binary, "--server", "7373", "--host", "0.0.0.0"],
            text=True, capture_output=True, timeout=10,
        )
        self.check(proc.returncode != 0 and "loopback" in proc.stderr,
                   f"remote bind was not rejected: exit={proc.returncode}, stderr={proc.stderr}")

    def network(self) -> None:
        self.remote_bind_guard()
        with tempfile.TemporaryDirectory(prefix="atlasdb-server-") as directory:
            proc, port = self.start_server(directory)
            first = second = None
            try:
                first = socket.create_connection(("127.0.0.1", port), timeout=4)
                second = socket.create_connection(("127.0.0.1", port), timeout=4)
                a = first.makefile("rwb", buffering=0)
                b = second.makefile("rwb", buffering=0)

                def send(stream, sql: str, ok: bool = True) -> dict:
                    stream.write(sql.encode("utf-8") + b"\n")
                    line = stream.readline(4 * 1024 * 1024)
                    self.check(bool(line), f"server closed before replying to {sql!r}")
                    value = json.loads(line)
                    self.check(value.get("ok") == ok, f"unexpected TCP response to {sql!r}: {value}")
                    return value

                send(a, "CREATE TABLE counters (id INT PRIMARY KEY, value INTEGER)")
                send(a, "INSERT INTO counters VALUES (1,10)")
                send(a, "BEGIN")
                send(b, "BEGIN")
                send(a, "UPDATE counters SET value=20 WHERE id=1")
                self.check(send(b, "SELECT value FROM counters")["rows"] == [[10]], "uncommitted write visible")
                send(a, "COMMIT")
                self.check(send(b, "SELECT value FROM counters")["rows"] == [[10]], "snapshot changed after another commit")
                send(b, "UPDATE counters SET value=30 WHERE id=1")
                conflict = send(b, "COMMIT", ok=False)
                self.check(conflict.get("code") == 8, "expected transaction conflict code")
                send(b, "ROLLBACK")
                self.check(send(b, "SELECT value FROM counters")["rows"] == [[20]], "committed value missing")
                send(a, "nonsense", ok=False)
                send(a, "SELECT value FROM counters")
                send(a, "CREATE TABLE parallel_counter (id INT PRIMARY KEY, value INTEGER)")
                send(a, "INSERT INTO parallel_counter VALUES (1,0)")

                def increment_many(worker: int) -> int:
                    with socket.create_connection(("127.0.0.1", port), timeout=8) as connection:
                        with connection.makefile("rwb", buffering=0) as stream:
                            for _ in range(20):
                                stream.write(b"UPDATE parallel_counter SET value=value+1 WHERE id=1\n")
                                response = json.loads(stream.readline(65536))
                                require(response.get("ok") and response.get("affected") == 1,
                                        f"concurrent worker {worker}: {response}")
                    return 20

                with ThreadPoolExecutor(max_workers=4) as pool:
                    counts = list(pool.map(increment_many, range(4)))
                self.check(sum(counts) == 80, "concurrent clients did not finish")
                self.check(send(a, "SELECT value FROM parallel_counter")["rows"] == [[80]],
                           "simultaneous updates lost increments")
                # One acknowledged durable commit must survive an abrupt process exit.
                proc.kill()
                proc.communicate(timeout=5)
                first.close(); second.close(); a.close(); b.close()
                first = second = None
                self.check(self.cli("SELECT value FROM counters", directory)[-1]["rows"] == [[20]],
                           "acknowledged commit lost after SIGKILL")
                self.check(self.cli("SELECT value FROM parallel_counter", directory)[-1]["rows"] == [[80]],
                           "concurrent acknowledged commits lost after SIGKILL")
            finally:
                if first is not None:
                    first.close()
                if second is not None:
                    second.close()
                if proc.poll() is None:
                    proc.send_signal(signal.SIGTERM)
                    proc.communicate(timeout=8)
        proc, _ = self.start_server()
        proc.send_signal(signal.SIGTERM)
        proc.communicate(timeout=8)
        self.check(proc.returncode == 0, "graceful server shutdown failed")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", nargs="?", type=Path, default=Path("build/atlasdb"))
    args = parser.parse_args()
    suite = Suite(args.binary)
    suite.scripts_and_json()
    suite.differential_sql()
    suite.wal_recovery()
    suite.network()
    print(f"AtlasDB integration: {suite.checks} checks passed")


if __name__ == "__main__":
    main()
