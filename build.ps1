# Compilation de FLASH (Release).
# Le projet est compilé depuis un lecteur virtuel pour que les binaires ne contiennent
# aucun chemin de la machine.
param(
    [string]$Drive = "X:",
    [switch]$Clean,
    [switch]$TestObs,   # copie le plugin dans l'OBS portable de test (.test-obs)
    [string[]]$Cibles,  # cibles en plus (outils d'essai)
    [switch]$Portable,  # édition portable dans dist\ (dossier et zip)
    [switch]$Installeur # installeur Inno Setup dans dist\ (fait aussi l'édition portable)
)

# Signature Authenticode, inactive par défaut. Pour l'activer, donner l'empreinte du
# certificat (magasin de l'utilisateur) et l'adresse d'horodatage du fournisseur :
#   $env:FLASH_CERTIFICAT = "empreinte SHA-1"; $env:FLASH_HORODATAGE = "http://..."
# Sans ces deux variables, rien n'est signé.

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
if ($Installeur) { $Portable = $true }

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

function Signer([string[]]$Fichiers) {
    if (-not $env:FLASH_CERTIFICAT -or -not $env:FLASH_HORODATAGE) { return }
    $signtool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\signtool.exe" | Sort-Object FullName | Select-Object -Last 1
    if (-not $signtool) { throw "signtool introuvable (SDK Windows)" }
    & $signtool.FullName sign /sha1 $env:FLASH_CERTIFICAT /fd SHA256 /tr $env:FLASH_HORODATAGE /td SHA256 $Fichiers
    if ($LASTEXITCODE) { throw "échec de la signature" }
}

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

    if ($Portable) {
        $version = [regex]::Match((Get-Content "$src\CMakeLists.txt" -Raw), "project\(flash VERSION ([0-9.]+)").Groups[1].Value
        # Runtime Visual C++ redistribuable des Build Tools, livré à côté de l'appli pour srt.dll
        $crt = Get-Item "$vs\VC\Redist\MSVC\*\x64\Microsoft.VC143.CRT" | Sort-Object FullName | Select-Object -Last 1
        if (-not $crt) { throw "Runtime Visual C++ redistribuable introuvable" }
        Signer @("$bld\app\flash.exe", "$bld\obs\flash-obs.dll")

        # Le dossier de FLASH, tel qu'il sera sur le PC : l'appli, une copie du plugin pour OBS
        # (que l'appli installe dans ProgramData), les licences. L'installeur reprend ce dossier.
        $out = "$Drive\dist\FLASH-$version-portable"
        if (Test-Path $out) { Remove-Item -Recurse -Force $out }
        foreach ($d in "", "licences", "plugin-obs\bin\64bit", "plugin-obs\data") { New-Item -ItemType Directory -Force "$out\$d" | Out-Null }
        Copy-Item "$bld\app\flash.exe", "$bld\app\srt.dll" $out
        Copy-Item "$($crt.FullName)\msvcp140.dll", "$($crt.FullName)\vcruntime140.dll", "$($crt.FullName)\vcruntime140_1.dll" $out
        Copy-Item "$bld\obs\flash-obs.dll" "$out\plugin-obs\bin\64bit"
        Copy-Item "$src\obs\data\*" "$out\plugin-obs\data" -Recurse
        Copy-Item "$src\installer\LISEZMOI.txt" $out
        $licences = [ordered]@{
            "srt.txt" = ".deps\obs-deps\licenses\srt\LICENSE"; "mbedtls.txt" = ".deps\obs-deps\licenses\mbedtls\LICENSE"
            "opus.txt" = ".deps\opus\opus-1.5.2\COPYING"; "monocypher.txt" = "third_party\monocypher\LICENCE.md"
            "nlohmann-json.txt" = ".deps\obs-deps\licenses\nlohmann-json\LICENSE.MIT"
            "nv-codec-headers.txt" = ".deps\obs-deps\licenses\nv-codec-headers\LICENSE"; "webview2.txt" = ".deps\webview2\LICENSE.txt"
        }
        foreach ($l in $licences.GetEnumerator()) { Copy-Item "$src\$($l.Value)" "$out\licences\$($l.Key)" }

        $zip = "$Drive\dist\FLASH-$version-portable.zip"
        if (Test-Path $zip) { Remove-Item -Force $zip }
        # Entrées en « / » : un zip fait par Compress-Archive (PowerShell 5) garde des « \ », et
        # hors de Windows, unzip les laisse dans les noms de fichiers
        Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
        $archive = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
        try {
            foreach ($fichier in Get-ChildItem $out -Recurse -File) {
                $nom = $fichier.FullName.Substring($out.Length + 1).Replace("\", "/")
                [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $fichier.FullName, $nom, [IO.Compression.CompressionLevel]::Optimal)
            }
        } finally { $archive.Dispose() }
    }

    if ($Installeur) {
        $iscc = "$Drive\.deps\innosetup\ISCC.exe"
        if (-not (Test-Path $iscc)) { throw "Inno Setup introuvable (.deps\innosetup)" }
        & $iscc /Q "/DVersion=$version" "/DDossier=$out" "/O$Drive\dist" "$Drive\installer\flash.iss"
        if ($LASTEXITCODE) { throw "échec de l'installeur" }
        Signer @("$Drive\dist\FLASH-$version-installation.exe")
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
