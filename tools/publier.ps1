# Prépare une version de FLASH à publier : trois fichiers, dans l'ordre où GitHub les montre
# (par nom) : l'installeur, le zip portable (qui porte aussi le manifeste signé des mises à jour),
# le zip des licences. Rien n'est envoyé sans -Publier, qui crée la Release GitHub.
#   powershell -ExecutionPolicy Bypass -File tools\publier.ps1 -Notes "Ce qui change" [-Publier]
param(
    [string]$Notes = "",
    [string]$Cle = (Join-Path $env:USERPROFILE ".flash-cle\flash-maj.cle"),
    [string]$Depot = "OkamiOo/FLASH",
    [switch]$Publier
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not (Test-Path $Cle)) { throw "clé de signature introuvable : $Cle" }
# Une publication vient d'un commit, sans modification en cours
if ((git -C $root status --porcelain | Measure-Object).Count) { throw "modifications non commitées : publier depuis un commit" }
$commit = git -C $root rev-parse --short HEAD

# Le lecteur X: de la compilation n'est retiré que s'il a été créé ici
$avant = (subst) | Where-Object { $_ -like "X:\*" }
try {
    & (Join-Path $root "build.ps1") -Installeur -Cibles signer
} finally {
    if (-not $avant) { subst X: /D 2>$null }
}
$version = [regex]::Match((Get-Content (Join-Path $root "CMakeLists.txt") -Raw), "project\(flash VERSION ([0-9.]+)").Groups[1].Value
$portable = Join-Path $root "dist\FLASH-$version-portable"
$installeur = Join-Path $root "dist\FLASH-$version-installation.exe"

# Entrées en « / » : un zip fait par Compress-Archive (PowerShell 5) garde des « \ »
function Zipper([string]$dossier, [string]$zip) {
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($f in Get-ChildItem $dossier -Recurse -File) {
            $nom = $f.FullName.Substring($dossier.Length + 1).Replace("\", "/")
            [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $f.FullName, $nom, [IO.Compression.CompressionLevel]::Optimal)
        }
    } finally { $archive.Dispose() }
}

$sortie = Join-Path $root "dist\publication\$version"
if (Test-Path $sortie) { Remove-Item -Recurse -Force $sortie }
New-Item -ItemType Directory -Force $sortie | Out-Null
$etape = Join-Path $sortie "etape"

# 1. Le zip portable, avec le manifeste signé (chemins relatifs, SHA-256 de chaque fichier)
New-Item -ItemType Directory -Force $etape | Out-Null
Copy-Item $portable "$etape\portable" -Recurse
# Le manifeste garde les notes sur une ligne : il lit une clé par ligne
$notesManifeste = (($Notes -split "\r?\n") | ForEach-Object { $_.Trim() } | Where-Object { $_ }) -join " "
& (Join-Path $root "build\tools\signer.exe") manifeste "$etape\portable" $version $Cle $notesManifeste | Out-Null
if ($LASTEXITCODE) { throw "manifeste : échec" }
Zipper "$etape\portable" (Join-Path $sortie "FLASH-$version-portable.zip")

# 2. Les licences, le LISEZMOI et l'endroit où trouver les sources
New-Item -ItemType Directory -Force "$etape\licences" | Out-Null
Copy-Item "$portable\licences\*" "$etape\licences"
Copy-Item "$portable\LISEZMOI.txt" "$etape\licences"
$sources = @"
FLASH $version est un logiciel libre sous licence GPL-3.0 ou plus récente (flash.txt).
Sources de cette version : https://github.com/$Depot/tree/v$version (archives « Source code »
de la Release).
SRT (MPL-2.0) : sources sur https://github.com/Haivision/srt.
"@
[IO.File]::WriteAllText("$etape\licences\SOURCES.txt", $sources.Replace("`r`n", "`n").Trim() + "`n", (New-Object Text.UTF8Encoding $false))
Zipper "$etape\licences" (Join-Path $sortie "FLASH-Licences-$version.zip")

# 3. L'installeur
Copy-Item $installeur $sortie
Remove-Item -Recurse -Force $etape

$fichiers = Get-ChildItem $sortie -File | Sort-Object Name
$empreintes = foreach ($f in $fichiers) { "- ``$($f.Name)`` : ``$((Get-FileHash $f.FullName -Algorithm SHA256).Hash.ToLower())``" }
$texte = @"
$Notes

- ``FLASH-$version-installation.exe`` : FLASH installé pour l'utilisateur, sans droits d'administrateur.
- ``FLASH-$version-portable.zip`` : dézipper, lancer ``flash.exe``.
- ``FLASH-Licences-$version.zip`` : licences des composants et emplacement des sources.

Les copies en 0.3.1 ou plus récente se mettent à jour toutes seules.

Empreintes SHA-256 (GitHub les affiche aussi à côté de chaque fichier) :
$($empreintes -join "`n")

FLASH est sous licence GPL-3.0 ou plus récente ; les sources de cette version sont celles du tag ``v$version``.
"@
[IO.File]::WriteAllText((Join-Path $sortie "notes.md"), $texte.Replace("`r`n", "`n").Trim() + "`n", (New-Object Text.UTF8Encoding $false))

$fichiers | Select-Object Name, @{ n = "Ko"; e = { [math]::Round($_.Length / 1KB) } } | Format-Table -AutoSize | Out-String | Write-Host
if (-not $Publier) {
    "FLASH $version (commit $commit) prêt dans $sortie. Rien n'a été publié."
    return
}
# La Release porte le tag de ce commit : ses sources sont celles des binaires
gh release create "v$version" @($fichiers.FullName) --repo $Depot --target (git -C $root rev-parse HEAD) --title "FLASH $version" --notes-file (Join-Path $sortie "notes.md")
if ($LASTEXITCODE) { throw "gh release : échec" }
"FLASH $version (commit $commit) publié sur $Depot."
