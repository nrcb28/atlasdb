# Desarrollo y verificación

El proyecto utiliza GCC o Clang, C17, `make`, hilos POSIX y las interfaces de archivos y sockets de Linux. No necesita paquetes de terceros. Python 3 se usa para comparar consultas con SQLite y comprobar las interfaces desde otro proceso.

## Comandos

| Comando | Qué comprueba |
|---|---|
| `make` | Consola, servidor y biblioteca estática |
| `make test-unit` | API, parser, AVL, modelo, codec, consultas y utilidades |
| `make test-integration` | CLI, JSON, TCP, aislamiento, persistencia y cierre abrupto |
| `make test-oom` | Fallos de asignación y liberación completa con wrappers del enlazador GNU |
| `make test-faults` | Fallos parciales de escritura, sincronización, truncado y checkpoint |
| `make differential` | Expresiones y patrones aleatorios comparados con SQLite |
| `make check` | Todos los grupos anteriores |
| `make sanitize` | Pruebas principales compiladas con ASan y UBSan |
| `make examples` | Un recorrido SQL completo |
| `make build/embed` | Ejemplo de integración de la API C |
| `make benchmark` | Mediciones locales en memoria |
| `make package` | ZIP de fuentes y suma SHA256, sin bases de datos ni binarios |

El benchmark admite otro volumen: `./build/benchmark 20000`. Sus cifras incluyen parsing y creación de resultados; no miden escrituras persistentes. Las consultas fuera de transacciones comparten el mutex de la base, también cuando proceden de varios hilos.

`make test-integration` necesita abrir sockets de loopback. Un sandbox que bloquee sockets debe permitirlos o ejecutar primero `make test-unit`. Algunos entornos con `ptrace` impiden LeakSanitizer; en ellos puede usarse `ASAN_OPTIONS=detect_leaks=0 make sanitize`. ASan y UBSan siguen activos, y `test-oom` cuenta las asignaciones propias que quedan pendientes.

Para tratar advertencias como errores:

```sh
make BUILD=build/strict CFLAGS='-O2 -g -std=c17 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror' check
```

La configuración de GitHub Actions incluye GCC, Clang y sanitizadores. El archivo se entrega como parte del código; la ejecución remota depende del repositorio donde se use.

## Invariantes para ampliar el motor

- El AST posee sus cadenas, valores y nodos. Nunca toma prestada memoria del SQL original.
- Las filas conservan identificadores positivos y crecientes, aunque se borren filas intermedias.
- Una sentencia que modifica datos trabaja sobre una copia privada; en caso de error se descarta por completo.
- Los índices se reconstruyen tras una mutación y sus claves tienen memoria propia.
- La publicación del estado ocurre después de persistir el registro de commit.
- Un error de commit no cierra una transacción explícita: permite inspección, rollback o un nuevo intento cuando proceda.
- Las estructuras públicas de resultados deben mantenerse sincronizadas con la declaración `ctypes` de la suite diferencial.

Una función SQL nueva necesita validación de nombres y tipos, evaluación de nulos, límites de recursos y una decisión explícita sobre errores de cálculo. Un cambio del formato persistente necesita una versión de formato nueva o una migración documentada.
