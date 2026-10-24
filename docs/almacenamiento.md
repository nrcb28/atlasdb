# Almacenamiento y recuperación

`adb_open(":memory:", ...)` crea una base sin archivos. Cualquier otro nombre representa un directorio persistente, que AtlasDB crea si es necesario.

| Archivo | Uso |
|---|---|
| `atlas.lock` | Bloqueo exclusivo del directorio |
| `atlas.wal` | Secuencia de imágenes confirmadas |
| `atlas.snapshot` | Imagen consolidada en el último checkpoint |
| `atlas.snapshot.tmp` | Archivo temporal durante un checkpoint |

Solo un proceso puede abrir el directorio a la vez. Un servidor mantiene una única apertura y comparte la base con sus sesiones. El bloqueo no sustituye las garantías del sistema de archivos: usa almacenamiento local con operaciones POSIX fiables.

## Tramas

Cada registro WAL y cada snapshot usa una cabecera de 32 bytes seguida de una carga útil. Los enteros del formato son little endian.

| Desplazamiento | Tamaño | Contenido |
|---|---:|---|
| 0 | 8 | Firma `ATLASDB\0` |
| 8 | 4 | Versión del formato: 1 |
| 12 | 4 | Tamaño de cabecera: 32 |
| 16 | 8 | Longitud de la carga útil |
| 24 | 4 | CRC32 IEEE de la carga útil |
| 28 | 4 | CRC32 IEEE de los primeros 28 bytes |

La carga contiene la versión de la base, los esquemas, las filas y las definiciones de índice. Los árboles en memoria se reconstruyen al decodificar. Los lectores comprueban longitudes y límites antes de reservar memoria o recorrer los datos. La versión inicial del formato no promete compatibilidad con futuras versiones de AtlasDB.

CRC32 permite detectar daños accidentales; no es una firma criptográfica.

## Commit

Una escritura valida un estado privado y lo serializa completamente. El almacenamiento añade una trama al WAL y sincroniza el archivo antes de devolver éxito al runtime. Después se publica el nuevo estado confirmado en memoria. Así, un commit reconocido puede recuperarse sin depender de un cierre limpio del proceso.

Los registros contienen imágenes completas, no cambios incrementales. Una transacción que modifica una fila puede escribir toda la base. El WAL crece con cada commit hasta que se ejecuta `CHECKPOINT`; planifica checkpoints según el tamaño y la frecuencia de escritura.

Si falla una escritura o su sincronización, se intenta truncar y sincronizar el WAL hasta su posición anterior. Si esa reparación también falla, el almacenamiento rechaza nuevas escrituras y checkpoints: el resultado durable del intento es incierto. Las lecturas del estado en memoria siguen disponibles; es necesario cerrar y volver a abrir la base para recuperar lo que quedó en disco. La API informa expresamente de este caso en el mensaje de error.

## Checkpoint

El orden de las operaciones es:

1. Escribir la imagen actual en `atlas.snapshot.tmp`.
2. Sincronizar ese archivo.
3. Renombrarlo atómicamente como `atlas.snapshot`.
4. Sincronizar el directorio.
5. Truncar el WAL y sincronizarlo.

El snapshot queda instalado antes de vaciar el WAL. Si el proceso se detiene entre ambos pasos, la recuperación puede encontrar el snapshot nuevo y registros WAL anteriores; la versión del estado permite elegir la imagen vigente.

## Apertura y daños

La recuperación valida el snapshot existente y recorre las tramas del WAL. Una cabecera final incompleta o una carga final incompleta se trata como una escritura interrumpida y se elimina del final del WAL. Los commits completos anteriores se conservan.

Una trama completa con una suma de comprobación incorrecta produce un error de corrupción. El motor no salta silenciosamente ese registro ni sustituye datos dañados por una base vacía. Un snapshot debe contener exactamente una trama completa válida.

Haz copias con la base cerrada, o detén las escrituras y coordina la copia de todos los archivos. Copiar solo el snapshot mientras hay commits activos puede omitir datos reconocidos. No edites manualmente archivos de una base abierta.
