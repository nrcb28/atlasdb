# AtlasDB para Windows: aplicación C# y motor nativo

Consola, scripts SQL y servidor TCP en C# (.NET 10), para Windows 10/11 x64. `AtlasDB.Managed` integra mediante P/Invoke el motor C original compilado como DLL: conserva SQL, AVL, restricciones, transacciones y formato de almacenamiento. No es una reescritura del motor ni utiliza SQLite.

## Ejecutar sin instalar .NET

Descargar el artefacto `AtlasDB-Windows-x64` del workflow **Windows C#** en GitHub Actions. Extraer todos los archivos en una carpeta y abrir PowerShell allí. El paquete es autocontenido: no requiere .NET ni compilador. Mantener `AtlasDB.exe` y `atlasdb_native.dll` juntos.

```powershell
.\AtlasDB.exe --help
.\AtlasDB.exe --file .\examples\inventario.sql --json
.\AtlasDB.exe --db .\mis-datos
.\AtlasDB.exe --db .\mis-datos --json --execute 'SHOW TABLES;'
```

Consola: `.help`, `.tables`, `.schema nombre`, `.checkpoint` y `.quit`. Finalizar SQL con punto y coma. Los scripts y `--execute` admiten varias sentencias y se detienen en el primer error. Al cerrar una sesión se revierte su transacción pendiente. Los ejemplos que crean tablas necesitan una base vacía.

## Servidor local

```powershell
.\AtlasDB.exe --db .\datos-servidor --server 7373 --max-clients 32
# Otra terminal:
.\AtlasDB.exe --client 127.0.0.1:7373 --execute 'SHOW TABLES;'
```

UTF-8 con una sentencia y un objeto JSON por línea. Cada conexión conserva su sesión y transacción; el cliente CLI abre una conexión por consulta. Para transacciones usar una conexión persistente, por ejemplo `examples/cliente_python.py`. Timeout por petición: 30 segundos; SQL máximo: 1 MiB. Al alcanzar el límite de clientes se cierra la conexión nueva. Ctrl+C detiene el servidor y revierte los cambios pendientes.

Escucha en 127.0.0.1 por defecto y carece de autenticación/TLS; su propósito es desarrollo local.

## Compilar en Windows

Instalar .NET 10 SDK y MSYS2. En una terminal MSYS2 UCRT64:

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-gcc
```

Desde PowerShell, en la raíz del repositorio:

```powershell
.\windows\build-native.ps1 -Gcc C:\msys64\ucrt64\bin\gcc.exe
dotnet build .\windows\AtlasDB.Cli -c Release
dotnet run --project .\windows\AtlasDB.Tests -c Release
dotnet run --project .\windows\AtlasDB.Cli -c Release -- --file .\examples\inventario.sql --json
.\windows\publish.ps1 -Gcc C:\msys64\ucrt64\bin\gcc.exe
```

La publicación genera `build/windows-x64/` y `build/AtlasDB-Windows-x64.zip`. Respetar la política de ejecución de PowerShell de la organización; si bloquea los scripts, ejecutar sus órdenes manualmente. Sin paquetes NuGet de terceros; la publicación descarga los packs oficiales del runtime de Microsoft.

## Biblioteca C#

Referenciar `windows/AtlasDB.Managed/AtlasDB.Managed.csproj` y copiar la DLL nativa x64 junto al ejecutable:

```csharp
using AtlasDB.Managed;
using var database = new Database(@".\datos-csharp");
using var session = database.CreateSession();
session.Execute("CREATE TABLE IF NOT EXISTS notas (id INTEGER PRIMARY KEY, texto TEXT)");
var result = session.Execute("SELECT * FROM notas");
Console.WriteLine(result.GetRawText());
```

`Execute` devuelve un JsonElement independiente y lanza AtlasException en errores SQL; sus propiedades Code y Offset conservan el contrato del motor. El conflicto tiene código 8. `ExecuteJson` devuelve JSON con ok, también para errores SQL. Los errores de argumentos, memoria, DLL o uso después de Dispose son excepciones .NET.

Database y Session implementan IDisposable. SafeHandle conserva la base nativa hasta liberar su última sesión. Las llamadas sobre una sesión se serializan; las transacciones completas deben pertenecer a un solo flujo lógico. Crear una sesión por consumidor. Se rechaza NUL, UTF-16 inválido y SQL superior a 1 MiB antes del P/Invoke. No hay parámetros preparados: validar entradas sigue siendo responsabilidad de la aplicación.

## Compatibilidad y límites

- El codec binario se conserva. Para mover una base entre Windows y Linux, detener el proceso, copiar el directorio completo y verificar la copia antes de usarla. No compartir un directorio entre procesos activos.
- Los archivos se abren en modo binario para impedir traducción CRLF. Las rutas UTF-8 se convierten a UTF-16 y se usan APIs de archivo Unicode.
- LockFileEx mantiene el bloqueo exclusivo. _commit sincroniza archivos. MoveFileExW con reemplazo y MOVEFILE_WRITE_THROUGH publica checkpoints sobre imágenes anteriores.
- No se sincronizan directorios: Windows no ofrece la misma semántica POSIX de fsync. No se promete equivalencia ante cortes de energía. Se conserva la recuperación lógica y CRC32; falta una campaña de fallos eléctricos sobre Windows y distintos sistemas de archivos.
- Aplicación de consola x64, sin GUI, ARM64, instalador, servicio Windows ni firma de código. Mantiene los límites SQL y el coste de copias completas del motor.
- La salida legible usa cabeceras y filas JSON en lugar de la tabla ASCII de la edición C. --json conserva un objeto por sentencia con ok.

## Validación

AtlasDB.Tests comprueba CRUD, Unicode, índices, atomicidad, rollback, aislamiento, conflictos, vida de SafeHandle, concurrencia, bloqueo de archivos, checkpoints repetidos, reapertura, WAL incompleto y scripts. `tests/windows_frontend.py` verifica CLI, TCP y cierre abrupto. El workflow Windows C# ejecuta todo en Windows y genera el paquete autocontenido. Consultar su resultado real antes de atribuirle éxito.

Las pruebas originales de Linux permanecen en make check y make sanitize. Compilar una DLL de Windows desde Linux no sustituye una ejecución en Windows.
