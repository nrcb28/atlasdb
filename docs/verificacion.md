# Verificación local — 24 de octubre de 2026

## Resultado

Compilación, make check y make sanitize completados con código de salida 0. No se han necesitado cambios en el motor. Se amplían README, documentación y ejemplos. Esta verificación cubre los escenarios ejecutados; no equivale a una certificación de producción ni auditoría exhaustiva.

Entorno: Linux x86_64, GCC 14.2.0 (Debian), Python 3.12.14. Compilación C17 con las advertencias configuradas en Makefile. Sanitizadores AddressSanitizer y UndefinedBehaviorSanitizer con la configuración original, sin desactivar LeakSanitizer.

| Grupo | Resultado observado |
|---|---|
| API C | 390 comprobaciones |
| Parser | 18297 comprobaciones, incluidos 18000 inputs deterministas |
| Índices/modelo | 60000 claves AVL y casos de límites/propiedad |
| Almacenamiento | 459 aserciones |
| Casos límite de motor | 437 comprobaciones |
| Utilidades | 95 comprobaciones |
| Integración | 78 comprobaciones, incluida comunicación TCP y recuperación |
| Frontend | Unicode, cabeceras escapadas, alineación y delimitación de scripts |
| Memoria del parser | 960 inyecciones de fallo sin asignaciones pendientes |
| Memoria del runtime | 97121 comprobaciones, 1150 inyecciones en 23 escenarios |
| Fallos de almacenamiento | 954 aserciones en 26 escenarios |
| Diferencial | 20323 comparaciones: SQLite y consultas indexadas frente a escaneo |
| Sanitizadores | Pruebas principales e integración completadas sin diagnósticos de ASan/UBSan |

No sumar estas cifras como si fueran casos independientes equivalentes: algunas son aserciones internas, otras entradas o comparaciones.

## Ejecución funcional

- examples/demo.sql y examples/transacciones.sql ejecutados correctamente.
- examples/embed.c compilado y ejecutado.
- Nuevo inventario: dos referencias, stock de teclado 11, total 173090 céntimos; EXPLAIN muestra INDEX LOOKUP para nombre.
- Persistencia: se abrió un directorio nuevo, se cargó inventario, terminó el proceso y se reabrió; el total siguió siendo 173090.
- Servidor iniciado en 127.0.0.1:7373 dentro del entorno de trabajo; cliente Python nuevo: dos muestras y media 20.35. Cliente CLI recuperó las dos filas.

El proceso del servidor depende de la vida de esta sesión. No se ha publicado un endpoint accesible desde Internet.

## Benchmark exploratorio

Ejecución local única, 5000 filas en memoria: carga 0.013 s (390802 filas/s); 300 búsquedas por PRIMARY KEY, 0.001 ms/consulta; 300 escaneos, 0.292 ms/consulta; cuatro sesiones, 1200 consultas, 340818 consultas/s. Valores redondeados por la herramienta. No son cifras garantizadas, no miden escrituras persistentes y no permiten comparar otros motores sin un protocolo común y repeticiones.

## Reproducir

```sh
make -j2 check
make sanitize
make build/embed benchmark
./build/embed
./build/atlasdb --file examples/demo.sql
./build/atlasdb --file examples/transacciones.sql
./build/atlasdb --file examples/inventario.sql
```

Consultar docs/evidencias para logs de consola. La CI incluida en .github/workflows/ci.yml no ha sido ejecutada remotamente como parte de este trabajo.
