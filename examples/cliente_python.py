"""Cliente TCP local: una conexión para la transacción completa.
Iniciar primero: ./build/atlasdb --db ./datos-demo --server 7373
Ejecutar: python3 examples/cliente_python.py
La tabla debe estar ausente antes de ejecutar este ejemplo.
"""
import json
import socket


def query(stream, sql):
    stream.write((sql + "\n").encode("utf-8"))
    line = stream.readline(8 * 1024 * 1024)
    if not line or not line.endswith(b"\n"):
        raise ConnectionError("Respuesta incompleta o superior al limite del ejemplo")
    result = json.loads(line)
    if not result.get("ok"):
        raise RuntimeError(result)
    return result


def main():
    with socket.create_connection(("127.0.0.1", 7373), timeout=5) as connection:
        with connection.makefile("rwb", buffering=0) as stream:
            query(stream, "CREATE TABLE lecturas_demo (id INTEGER PRIMARY KEY, sensor TEXT NOT NULL, valor REAL)")
            query(stream, "BEGIN")
            try:
                query(stream, "INSERT INTO lecturas_demo VALUES (1,'laboratorio',22.5),(2,'almacen',18.2)")
                query(stream, "COMMIT")
            except Exception:
                # Si se pierde la conexión, no se puede asegurar el resultado del commit.
                try:
                    query(stream, "ROLLBACK")
                except Exception:
                    pass
                raise
            print(json.dumps(query(stream, "SELECT COUNT(*) AS muestras, AVG(valor) AS media FROM lecturas_demo"), ensure_ascii=False))


if __name__ == "__main__":
    main()
