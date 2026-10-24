"""Cross-platform tests for the C# frontend; CI also runs the Windows executable."""
import argparse
import json
from pathlib import Path
import socket
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--dotnet')
    parser.add_argument('--app', required=True)
    args = parser.parse_args()
    command = ([args.dotnet] if args.dotnet else []) + [str(Path(args.app).resolve())]
    checks = 0

    def check(condition, message):
        nonlocal checks
        checks += 1
        if not condition:
            raise AssertionError(message)

    def run(*arguments, success=True, input=None):
        result = subprocess.run(command + list(arguments), input=input, text=True,
                                encoding='utf-8', capture_output=True, timeout=30)
        check((result.returncode == 0) == success, (arguments, result.returncode, result.stdout, result.stderr))
        return result

    def rows(result):
        return [json.loads(line) for line in result.stdout.splitlines() if line.strip()]

    with tempfile.TemporaryDirectory(prefix='atlasdb-managed-') as directory:
        directory = Path(directory)
        database = directory / 'datos-ñ-日本語'
        sql = directory / 'script.sql'
        sql.write_text("-- ;\nCREATE TABLE t (id INTEGER PRIMARY KEY, texto TEXT);\n"
                       "INSERT INTO t VALUES (1,'O''Brien; ñ 日本語');\nSELECT * FROM t;", encoding='utf-8')
        result = rows(run('--db', str(database), '--file', str(sql), '--json'))
        check(result[-1]['rows'] == [[1, "O'Brien; ñ 日本語"]], 'Scripts y Unicode')
        result = rows(run('--db', str(database), '--execute', 'SELECT COUNT(*) FROM t;', '--json'))
        check(result[-1]['rows'] == [[1]], 'Persistencia entre procesos')
        run('--execute', 'SELECT inexistente FROM tabla;', '--json', success=False)
        run('--server', '0', success=False)
        run('--file', 'no-existe.sql', success=False)
        run('--opcion-desconocida', success=False)
        run('--execute', 'SHOW TABLES;', '--file', str(sql), success=False)
        result = rows(run('--json', input="CREATE TABLE stdin (v TEXT);\nINSERT INTO stdin VALUES ('a;b');\nSELECT * FROM stdin;\n"))
        check(result[-1]['rows'] == [['a;b']], 'Entrada redirigida')
        run('--db', str(database), '--execute', 'CHECKPOINT;', '--json')
        with socket.socket() as available:
            available.bind(('127.0.0.1', 0))
            port = available.getsockname()[1]
        server = subprocess.Popen(command + ['--db', str(database), '--server', str(port), '--max-clients', '4'],
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            end = time.monotonic() + 30
            while True:
                if server.poll() is not None:
                    raise AssertionError(server.communicate())
                try:
                    connection = socket.create_connection(('127.0.0.1', port), timeout=1)
                    break
                except OSError:
                    if time.monotonic() > end:
                        raise TimeoutError('El servidor no inicio')
                    time.sleep(0.05)
            with connection:
                stream = connection.makefile('rwb', buffering=0)
                def query(sql):
                    stream.write((sql + '\n').encode('utf-8'))
                    return json.loads(stream.readline())
                check(query('SELECT * FROM t;')['rows'] == [[1, "O'Brien; ñ 日本語"]], 'TCP inicial')
                check(query('BEGIN;')['ok'], 'BEGIN TCP')
                check(query("INSERT INTO t VALUES (2,'confirmado');")['ok'], 'INSERT TCP')
                check(query('COMMIT;')['ok'], 'COMMIT TCP')
                check(not query('SELECT no_existe FROM t;')['ok'], 'Error SQL TCP')
                check(query('SELECT COUNT(*) FROM t;')['rows'] == [[2]], 'Sesion util tras error')
                result = rows(run('--client', f'127.0.0.1:{port}', '--execute', 'SELECT COUNT(*) FROM t;'))
                check(result[-1]['rows'] == [[2]], 'Cliente C#')
                query('BEGIN;')
                query("INSERT INTO t VALUES (3,'no confirmado');")
                # The next connection must not see this pending row.
                result = rows(run('--client', f'127.0.0.1:{port}', '--execute', 'SELECT COUNT(*) FROM t;'))
                check(result[-1]['rows'] == [[2]], 'Aislamiento TCP')
                stream.close()
            # Another open must fail while the server holds the directory lock.
            run('--db', str(database), '--execute', 'SHOW TABLES;', success=False)
        finally:
            server.kill()  # Deliberate abrupt shutdown, exercising WAL recovery.
            server.communicate(timeout=10)
        result = rows(run('--db', str(database), '--execute', 'SELECT COUNT(*) FROM t;', '--json'))
        check(result[-1]['rows'] == [[2]], 'Commit durable tras cierre abrupto; fila pendiente ausente')
    print(f'C# CLI/TCP: {checks} comprobaciones correctas')


if __name__ == '__main__':
    main()
