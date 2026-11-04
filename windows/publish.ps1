param([string]$Gcc = "gcc")
$ErrorActionPreference = "Stop"
& "$PSScriptRoot/build-native.ps1" -Gcc $Gcc
$target = Join-Path $PSScriptRoot "../build/windows-x64"
dotnet publish "$PSScriptRoot/AtlasDB.Cli/AtlasDB.Cli.csproj" -c Release -r win-x64 --self-contained true -p:PublishSingleFile=true -o $target
if ($LASTEXITCODE -ne 0) { throw "Fallo al publicar C#." }
Copy-Item "$PSScriptRoot/README.md" "$target/README.md"
Copy-Item "$PSScriptRoot/../LICENSE" "$target/LICENSE"
New-Item -ItemType Directory -Force "$target/examples" | Out-Null
Copy-Item "$PSScriptRoot/../examples/*.sql" "$target/examples/"
$archive = Join-Path $PSScriptRoot "../build/AtlasDB-Windows-x64.zip"
if (Test-Path $archive) { Remove-Item $archive }
Compress-Archive -Path "$target/*" -DestinationPath $archive
$hash = (Get-FileHash $archive -Algorithm SHA256).Hash.ToLowerInvariant()
"$hash  $([IO.Path]::GetFileName($archive))" | Set-Content "$archive.sha256" -Encoding ascii
Write-Host "SHA256 ($archive) = $hash"
