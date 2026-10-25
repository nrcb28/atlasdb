# Protocolo TCP

El servidor escucha por defecto en `127.0.0.1:7373`. Usa `--host` y `--server PUERTO` para seleccionar otra dirección o puerto, y `--max-clients N` para limitar conexiones. Por seguridad, las ediciones C y Windows rechazan por defecto direcciones que no sean loopback; `--insecure-remote` permite omitir esa protección, pero no añade TLS ni autenticación.

```sh
./build/atlasdb --db ./datos --server 7373 --max-clients 32
```

Cada conexión tiene su propia sesión. Una petición es una sentencia SQL codificada en UTF-8 seguida de `\n`. No incluyas saltos de línea físicos dentro de la sentencia: el protocolo usa esos saltos como separadores. No hay negociación, encabezados HTTP ni autenticación.

Una respuesta correcta es un único objeto JSON terminado en `\n`:

```json
{"ok":true,"columns":["id","nombre"],"rows":[[1,"Ana"]],"affected":0,"message":null}
```

El campo `rows` contiene arreglos en el mismo orden que `columns`. Los valores nulos son `null`; booleanos, números y cadenas usan sus tipos JSON. `affected` indica filas afectadas por escrituras y `message` aporta información de la operación.

Un error tiene este formato:

```json
{"ok":false,"code":1,"error":"descripción del error","offset":7}
```

El texto del error puede cambiar. Para lógica de programa utiliza los códigos definidos por `ADBCode` en `include/atlasdb.h`. Entre ellos, `ADB_ERR_CONFLICT` vale 8 y pide volver a intentar una transacción a partir de una instantánea nueva.

Ejemplo de cliente con la biblioteca estándar de Python:

```python
import json
import socket

with socket.create_connection(("127.0.0.1", 7373), timeout=5) as connection:
    stream = connection.makefile("rwb", buffering=0)
    stream.write(b"SHOW TABLES;\n")
    response = json.loads(stream.readline())
    print(response)
```

Para una transacción envía `BEGIN`, las operaciones y `COMMIT` sobre el mismo `stream`. Un error SQL no obliga a cerrar la conexión. Al desconectarse, se revierten los cambios pendientes de la sesión.

El transporte impone límites de tamaño y tiempos de espera. Un cliente debe leer todas las respuestas, tratar desconexiones y considerar incierto el resultado de una operación si pierde la conexión antes de recibir su respuesta. Tras reiniciar, puede consultar los datos persistidos para resolver esa incertidumbre.

El servidor es una herramienta local de desarrollo. Carece de TLS y control de acceso; exponerlo en una red requiere medidas externas y una revisión de seguridad.
