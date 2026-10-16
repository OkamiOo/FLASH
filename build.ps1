# Compilation de FLASH (Release).
# Le projet est compilé depuis un lecteur virtuel pour que les binaires ne contiennent
# aucun chemin de la machine.
param(
    [string]$Drive = "X:",
    [switch]$Clean,
    [switch]$TestObs   # copie le plugin dans l'OBS portable de test (.test-obs)
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

$mapped = (subst) | Where-Object { $_ -like "$Drive\*" }
if (-not $mapped) {
    subst $Drive $root
} elseif ($mapped -notlike "*=> $root") {
    throw "Le lecteur $Drive est déjà utilisé : $mapped"
}

$vs = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools"
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { throw "Visual Studio Build Tools introuvable" }

# Reprend l'environnement MSVC dans cette session
cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match "^([^=]+)=(.*)$") { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
}

$src = "$Drive\"
$bld = "$Drive\build"
if ($Clean -and (Test-Path $bld)) { Remove-Item -Recurse -Force $bld }

Push-Location $src
try {
    cmake -S $src -B $bld -G Ninja -DCMAKE_BUILD_TYPE=Release
    if ($LASTEXITCODE) { throw "échec de la configuration" }
    cmake --build $bld
    if ($LASTEXITCODE) { throw "échec de la compilation" }
} finally {
    Pop-Location
}

if ($TestObs) {
    $obs = Join-Path $root ".test-obs"
    Copy-Item "$bld\obs\flash-obs.dll" "$obs\obs-plugins\64bit\" -Force
    $data = "$obs\data\obs-plugins\flash-obs"
    New-Item -ItemType Directory -Force $data | Out-Null
    Copy-Item "$root\obs\data\*" $data -Recurse -Force
}
