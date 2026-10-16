# Compilation de FLASH (Release).
# Le projet est compilé depuis un lecteur virtuel pour que les binaires ne contiennent
# aucun chemin de la machine.
param(
    [string]$Drive = "X:",
    [switch]$Clean,
    [switch]$TestObs,  # copie le plugin dans l'OBS portable de test (.test-obs)
    [string[]]$Cibles, # cibles en plus (outils d'essai)
    [switch]$Installeur # installeur Inno Setup dans dist\
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
    foreach ($c in $Cibles) {
        cmake --build $bld --target $c
        if ($LASTEXITCODE) { throw "échec de la compilation de $c" }
    }
    if ($Installeur) {
        $iscc = "$Drive\.deps\innosetup\ISCC.exe"
        if (-not (Test-Path $iscc)) { throw "Inno Setup introuvable (.deps\innosetup)" }
        $version = [regex]::Match((Get-Content "$src\CMakeLists.txt" -Raw), "project\(flash VERSION ([0-9.]+)").Groups[1].Value
        # Runtime Visual C++ redistribuable des Build Tools, livré à côté de l'appli pour srt.dll
        $crt = Get-Item "$vs\VC\Redist\MSVC\*\x64\Microsoft.VC143.CRT" | Sort-Object FullName | Select-Object -Last 1
        if (-not $crt) { throw "Runtime Visual C++ redistribuable introuvable" }
        & $iscc /Q "/DVersion=$version" "/DBuild=$bld" "/DCrt=$($crt.FullName)" "/DDeps=$Drive\.deps" "/O$Drive\dist" "$Drive\installer\flash.iss"
        if ($LASTEXITCODE) { throw "échec de l'installeur" }
    }
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
