# AtlasDB

Una base de datos relacional escrita en **C17**, con biblioteca embebible, consola SQL y servidor TCP. El proyecto reúne análisis léxico y sintáctico, evaluación de expresiones, árboles AVL, transacciones con aislamiento por instantánea y recuperación de almacenamiento persistente.

El objetivo es estudiar y ampliar un sistema completo. Su diseño favorece la claridad y la integridad de los datos: cada escritura trabaja sobre una copia del estado y los commits persistentes guardan una imagen completa en el WAL. Este enfoque tiene un coste alto en memoria y disco; AtlasDB no pretende reemplazar una base de datos de producción.

## Windows con C#

La [edición Windows x64](windows/README.md) incluye consola y servidor en C# (.NET 10), una biblioteca administrada y el motor original como DLL. El workflow **Windows C#** genera un paquete autocontenido. Conserva SQL y formato de datos; documenta diferencias de interfaz y durabilidad.

## Documentación ampliada

- [Casos de uso e integración avanzada](docs/usos-avanzados.md).
- [Informe de verificación local](docs/verificacion.md).
- [Preparación para GitHub](docs/publicacion-github.md).
- [Inventario reproducible](examples/inventario.sql) y [cliente Python TCP](examples/cliente_python.py).

## Compilar y probar

Requiere Linux, GCC o Clang con soporte C17, `make` y POSIX threads. Las pruebas también necesitan Python 3 con su biblioteca estándar. Se utilizan `flock`, sockets e interfaces de archivos de Linux.

```sh
make
make test
make check
make sanitize
```

Los artefactos principales son `build/atlasdb` y `build/libatlasdb.a`. No se descargan dependencias.

`make check` añade pruebas con fallos de memoria y disco y más de 20.000 comparaciones con SQLite. `make benchmark` ejecuta mediciones locales; `make package` crea un ZIP de las fuentes y su suma SHA256. Los [comandos de desarrollo](docs/desarrollo.md) explican cada grupo de pruebas.

## Empezar

```sh
# Consola con almacenamiento persistente en ./mis-datos
./build/atlasdb --db ./mis-datos

# Ejecutar un ejemplo completo en memoria
./build/atlasdb --file examples/demo.sql

# Resultado legible por herramientas: un objeto JSON por sentencia
./build/atlasdb --db ./mis-datos --json --execute 'SHOW TABLES;'
```

Una sesión de ejemplo:

```sql
CREATE TABLE productos (
    id INTEGER PRIMARY KEY,
    nombre TEXT NOT NULL,
    precio REAL NOT NULL,
    stock INTEGER DEFAULT 0,
    activo BOOLEAN DEFAULT TRUE
);

INSERT INTO productos (id,nombre,precio,stock)
VALUES (1,'Teclado',79.90,12), (2,'Ratón',35.50,24);

CREATE INDEX productos_nombre ON productos(nombre);

SELECT nombre,precio*stock AS valor
FROM productos
WHERE activo=TRUE AND stock>0
ORDER BY nombre;

BEGIN;
UPDATE productos SET stock=stock-1 WHERE id=1;
COMMIT;

SELECT COUNT(*) AS productos,SUM(precio*stock) AS inventario
FROM productos;
CHECKPOINT;
```

La consola admite `.help`, `.tables`, `.schema nombre`, `.checkpoint` y `.quit`. Una sentencia puede ocupar varias líneas; el punto y coma la termina. Los ficheros SQL admiten comentarios y puntos y coma dentro de cadenas.

## Qué incluye

- Tipos `INTEGER` de 64 bits, `REAL`, `TEXT`, `BOOLEAN` y valores `NULL`.
- Esquemas con `PRIMARY KEY`, `UNIQUE`, `NOT NULL` y `DEFAULT`.
- `INSERT` de varias filas, `SELECT`, `UPDATE`, `DELETE` y operaciones de esquema.
- Expresiones aritméticas, comparaciones, lógica booleana, `LIKE` y pruebas de nulos.
- Funciones escalares `ABS`, `LENGTH`, `LOWER` y `UPPER`.
- Proyecciones con alias, ordenación, límites y agregados globales `COUNT`, `SUM`, `AVG`, `MIN`, `MAX`.
- Índices ordenados AVL y selección mediante igualdad sobre columnas indexadas.
- Transacciones explícitas, rollback, detección de conflictos entre sesiones y escrituras atómicas.
- WAL con CRC32, sincronización de commits, checkpoints y recuperación de colas incompletas.
- Servidor TCP con una sesión por conexión, respuestas JSON y límite de clientes.
- Pruebas de API, integración, comparación con SQLite y recuperación después de un cierre abrupto.

No hay `JOIN`, `GROUP BY`, subconsultas, claves foráneas, usuarios, TLS ni replicación. El servidor está pensado para desarrollo local y escucha en `127.0.0.1` por defecto. Las interfaces remotas se rechazan por defecto; `--insecure-remote` solo debe usarse detrás de una red protegida, porque no proporciona autenticación ni TLS.

## Servidor y cliente

```sh
./build/atlasdb --db ./datos-servidor --server 7373
./build/atlasdb --client 127.0.0.1:7373 --execute 'SHOW TABLES;'
```

El protocolo acepta una sentencia SQL por línea y devuelve un objeto JSON por línea. Para mantener una transacción, usa la misma conexión TCP durante `BEGIN`, las consultas y `COMMIT` o `ROLLBACK`. El cliente de línea de órdenes realiza una consulta por conexión.

```json
{"ok":true,"columns":["id"],"rows":[[1]],"affected":0,"message":null}
```

Los errores incluyen `ok:false`, `code`, `error` y `offset`. La forma exacta del campo informativo `message` depende de la operación. Véase [protocolo](docs/protocolo.md).

## Integrar la biblioteca

```c
#include "atlasdb.h"
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    ADBError error = {0};
    ADBResult result = {0};
    ADBDatabase *db = adb_open(":memory:", &error);
    if (!db) {
        fprintf(stderr, "%s\n", error.message);
        return 1;
    }
    ADBSession *session = adb_session_create(db, &error);
    if (!session) {
        fprintf(stderr, "%s\n", error.message);
        adb_close(db);
        return 1;
    }
    if (!adb_execute(session, "SHOW TABLES", &result, &error)) {
        fprintf(stderr, "%s\n", error.message);
        adb_session_destroy(session);
        adb_close(db);
        return 1;
    }
    char *json = adb_result_json(&result);
    if (json) { puts(json); free(json); }
    adb_result_free(&result);
    adb_session_destroy(session);
    adb_close(db);
    return 0;
}
```

```sh
cc -std=c17 -Iinclude programa.c build/libatlasdb.a -pthread -lm -o programa
```

Cada llamada a `adb_execute` acepta una sola sentencia. Inicializa `ADBResult` a cero y libera el resultado antes de reutilizarlo. La base de datos debe vivir más que sus sesiones; una sesión pertenece a un solo hilo. Distintas sesiones pueden usarse desde distintos hilos. El proceso que integra la biblioteca debe conservar el locale numérico `C`; el SQL y los textos deben ser UTF-8 válido.

El ejemplo [examples/embed.c](examples/embed.c) incluye creación de tablas, una transacción y consultas. Se compila con `make build/embed`.

## Orientarse en el código

| Ruta | Responsabilidad |
|---|---|
| `include/atlasdb.h` | API pública y tipos de resultado |
| `src/parser.c` | Lexer, parser y árbol de sintaxis |
| `src/engine.c` | Validación y ejecución de SQL |
| `src/index.c`, `src/model.c` | Índices AVL, tablas y copias del estado |
| `src/storage.c` | Serialización, WAL y checkpoints |
| `src/database.c` | Sesiones, transacciones y exclusión mutua |
| `src/main.c`, `src/server.c` | Consola, scripts y transporte TCP |
| `tests/` | Pruebas de API e integración |
| `examples/` | Scripts para explorar el motor |

La [referencia SQL](docs/sql.md), el [diseño interno](docs/arquitectura.md), el [formato de almacenamiento](docs/almacenamiento.md) y la [guía de desarrollo](docs/desarrollo.md) detallan el comportamiento y sus límites.

Licencia MIT.
