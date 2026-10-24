# Guía avanzada de usos de AtlasDB

## 1. Elegir el modo de integración

AtlasDB es un motor relacional educativo en C17 para Linux. Ofrece tres entradas al mismo motor: API C embebida, consola/scripts y servidor TCP. Es adecuado para estudiar implementación de bases de datos, crear demostradores locales e integrar pequeños conjuntos de datos en herramientas experimentales. El tamaño práctico depende de memoria, frecuencia de escritura y número de índices; los límites máximos no son una recomendación de capacidad.

| Necesidad | Modo recomendado | Condición |
|---|---|---|
| Aprender parsing, índices y durabilidad | API y tests | Seguir una sentencia desde parser.c hasta storage.c |
| Inventario de demostración | CLI persistente | Importes enteros; validación de negocio en la aplicación |
| Lecturas de sensores de laboratorio | API C o TCP local | Retención acotada y volumen pequeño |
| Fixtures para pruebas de una aplicación C | API en memoria | Crear una base nueva por prueba |
| Herramienta SQL de escritorio experimental | Biblioteca estática | Linux, locale C y sesiones con propietario claro |
| Enseñar conflictos y recuperación | Dos conexiones TCP | Una conexión por transacción |

No se recomienda para datos críticos, servicios públicos ni cargas grandes. No dispone de autenticación, TLS, replicación, JOIN, GROUP BY, claves foráneas ni consultas preparadas.

## 2. Inventario: ejemplo verificable

```sh
make
./build/atlasdb --file examples/inventario.sql
# Persistencia: usar un directorio nuevo para la primera ejecución.
./build/atlasdb --db ./datos-inventario --file examples/inventario.sql
./build/atlasdb --db ./datos-inventario --json --execute 'SELECT * FROM productos;'
```

El ejemplo deja 11 teclados y 24 ratones; el total es 173090 céntimos. INTEGER evita el redondeo binario de REAL para importes de esta demostración; la aplicación debe comprobar rangos y política monetaria.

La reducción usa `WHERE stock>0`. La aplicación debe verificar `affected=1` antes de confirmar una reserva. El ejemplo SQL muestra el mecanismo; no implementa por sí solo todo el flujo de una venta. No hay CHECK ni relaciones foráneas: las reglas entre tablas son responsabilidad de la aplicación.

Para cargas iniciales, agrupar varias filas en un INSERT puede reducir el número de copias. BEGIN agrupa commits, pero cada sentencia de escritura sigue copiando el estado privado. No garantiza cargas masivas eficientes.

## 3. Sensores y análisis local

```sql
CREATE TABLE lecturas (id INTEGER PRIMARY KEY, sensor TEXT NOT NULL, instante INTEGER, valor REAL);
INSERT INTO lecturas VALUES (1,'sala-a',1792800000,22.5),(2,'sala-a',1792800060,23.0);
CREATE INDEX por_sensor ON lecturas(sensor);
SELECT COUNT(*) AS muestras,AVG(valor) AS media,MIN(valor) AS minimo,MAX(valor) AS maximo FROM lecturas WHERE sensor='sala-a';
SELECT * FROM lecturas WHERE sensor='sala-a' ORDER BY instante DESC LIMIT 20;
DELETE FROM lecturas WHERE instante<1792700000;
```

El instante es un entero que la aplicación interpreta como Unix time en segundos; no hay tipo timestamp ni funciones de fecha. Para estadísticas por sensor se ejecuta una consulta por sensor o se agrupa fuera del motor. La retención evita crecimiento ilimitado. Los índices aceleran ciertas igualdades, no las búsquedas temporales por rango en general.

## 4. API C: propiedad y errores

Consultar `examples/embed.c` e `include/atlasdb.h` para el contrato real.

```sh
make build/embed
./build/embed
cc -std=c17 -Iinclude mi_programa.c build/libatlasdb.a -pthread -lm -o mi_programa
```

Mantener el locale numérico C. Abrir `:memory:` para datos efímeros o un directorio para persistir. Crear una sesión por hilo, inicializar ADBResult a cero, comprobar el booleano de adb_execute y liberar cada resultado con adb_result_free antes de reutilizarlo. Liberar la cadena de adb_result_json con free. Destruir sesiones antes de adb_close. La base debe permanecer viva mientras sus sesiones estén en uso.

La API acepta una sentencia por llamada. No hay parámetros preparados: evitar concatenar entradas arbitrarias. Para identificadores usar una lista permitida; para texto controlado duplicar comillas simples, validar UTF-8 y límites. En entradas no confiables conviene diseñar una capa específica y auditarla antes de ofrecer el servicio.

## 5. Transacciones y concurrencia

Una instantánea es una copia completa del estado. El commit de una transacción de escritura valida una versión global: incluso escrituras en tablas distintas pueden entrar en conflicto.

| Paso | Sesión A | Sesión B |
|---|---|---|
| 1 | BEGIN | BEGIN |
| 2 | UPDATE de una fila | UPDATE de otra fila |
| 3 | COMMIT correcto | — |
| 4 | — | COMMIT devuelve código 8 |
| 5 | — | ROLLBACK, BEGIN y recalcular |

Tras un conflicto, ejecutar ROLLBACK y volver a leer antes de repetir toda la operación. Usar reintentos acotados y pausas en la aplicación. Un error de sentencia descarta esa sentencia pero conserva cambios anteriores de la transacción. Un error de COMMIT mantiene abierta la transacción. Cerrar la conexión revierte cambios pendientes.

No reintentar a ciegas un commit si se perdió la respuesta: puede haberse confirmado. Diseñar identificadores únicos de operación y consultar el estado al reconectar para resolver la incertidumbre; AtlasDB no implementa una capa de idempotencia automáticamente.

## 6. TCP desde Python

```sh
./build/atlasdb --db ./datos-demo --server 7373 --max-clients 16
# En otra terminal; usar una base sin lecturas_demo:
python3 examples/cliente_python.py
```

El cliente incluido envía una sentencia por línea y mantiene BEGIN/INSERT/COMMIT en la misma conexión. Una respuesta contiene ok, columns, rows y affected; las respuestas de error incluyen code, error y offset. Es TCP con JSON por línea, no una API HTTP. El cliente CLI abre una conexión por consulta y no sirve para encadenar una transacción entre varias invocaciones.

El cliente de ejemplo tiene timeout de cinco segundos y un límite de respuesta de 8 MiB, una decisión local del ejemplo. El motor admite resultados mayores. Para integración real, definir presupuestos, manejo de desconexión y observabilidad adecuados al caso.

## 7. Durabilidad y copias de seguridad

Cada commit persistente serializa una imagen completa al WAL y sincroniza antes de publicar el nuevo estado. El CRC32 ayuda a detectar corrupción; no es una protección criptográfica. CHECKPOINT consolida la imagen persistida y reduce el WAL.

Para una copia sencilla y coherente: terminar transacciones, ejecutar CHECKPOINT, detener ordenadamente el proceso, copiar el directorio completo y probar su apertura desde otra ubicación. No copiar únicamente un fichero ni asumir que una copia del directorio mientras hay escrituras es coherente. No ejecutar dos procesos sobre el mismo directorio; el almacenamiento usa bloqueo de archivo.

Una copia en memoria, índices reconstruidos y el resultado JSON consumen memoria adicional. El coste de una escritura crece con toda la base, no solo con la fila cambiada. Planificar checkpoints y medir el volumen real; no hay política automática de backup ni replicación.

## 8. Rendimiento y diagnóstico

```sh
make benchmark
./build/benchmark 20000
./build/atlasdb --db ./datos-inventario --execute "EXPLAIN SELECT * FROM productos WHERE nombre='Teclado';"
```

Comparar igualdad indexada con escaneo en el mismo conjunto. Los resultados del benchmark son locales y en memoria; incluyen parsing y resultados, no escrituras durables. Un árbol AVL tiene búsqueda logarítmica sobre claves, pero la localización de filas, copias de estado y reconstrucción de índices añaden costes. Varias sesiones no equivalen a lecturas paralelas fuera de transacciones: comparten un mutex.

Los límites documentados incluyen SQL y textos de 1 MiB, 128 columnas por tabla, 1024 tablas, un millón de filas por tabla, presupuesto de resultados de 64 MiB e imagen persistente de 256 MiB. No representan capacidad operativa garantizada.

## 9. Extender el motor

Para una nueva función SQL: parser y AST, validación de aridad/tipos, evaluación de NULL, propiedad de memoria y pruebas de error. Para índices incrementales: conservar identificadores de fila, atomicidad y coherencia después de rollback. Para WAL incremental o páginas: versionar el formato, diseñar recuperación y añadir fallos inyectados. Para JOIN o GROUP BY: definir semántica, planificación y límites antes de ampliar la sintaxis.

Validar con make check y make sanitize. Ampliar la comparación con SQLite solo donde la semántica coincida; diferencias deliberadas como división entera, Unicode por bytes o SQL no implementado deben quedar documentadas.

## 10. Referencias

[SQL](sql.md) · [Arquitectura](arquitectura.md) · [Almacenamiento](almacenamiento.md) · [Protocolo](protocolo.md) · [Verificación local](verificacion.md) · [Publicación](publicacion-github.md).
