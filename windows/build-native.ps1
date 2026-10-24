param([string]$Gcc = "gcc")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $PSScriptRoot "native/bin"
New-Item -ItemType Directory -Force $out | Out-Null
$files = @("windows/native/bridge.c", "src/util.c", "src/parser.c", "src/index.c", "src/model.c", "src/engine.c", "src/storage.c", "src/database.c")
$arguments = @("-shared", "-static-libgcc", "-D_POSIX_C_SOURCE=200809L", "-D_FILE_OFFSET_BITS=64", "-std=c17", "-O2", "-Wall", "-Wextra", "-Werror", "-I$root/include", "-I$root/src")
$arguments += $files | ForEach-Object { Join-Path $root $_ }
$arguments += @("-lm", "-o", (Join-Path $out "atlasdb_native.dll"))
& $Gcc @arguments
if ($LASTEXITCODE -ne 0) { throw "Fallo al compilar el motor para Windows." }
Write-Host "DLL creada: $out/atlasdb_native.dll"
