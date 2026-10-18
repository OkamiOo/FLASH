# Prépare une version de FLASH à publier : édition portable, installeur, fichiers des mises à jour
# (à plat) et manifeste signé, empreintes SHA-256, notes. Rien n'est envoyé sans -Publier, qui crée
# la Release GitHub (les mises à jour automatiques la lisent).
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
$zip = Join-Path $root "dist\FLASH-$version-portable.zip"
$installeur = Join-Path $root "dist\FLASH-$version-installation.exe"

# Le manifeste est calculé sur une copie du dossier portable (chemins relatifs), puis chaque
# fichier est rangé à plat sous le nom que téléchargent les mises à jour (« / » devient « . »)
$sortie = Join-Path $root "dist\publication\$version"
$dossier = Join-Path $root "dist\publication\$version-dossier"
foreach ($d in $sortie, $dossier) { if (Test-Path $d) { Remove-Item -Recurse -Force $d } }
Copy-Item $portable $dossier -Recurse
New-Item -ItemType Directory -Force $sortie | Out-Null
& (Join-Path $root "build\tools\signer.exe") manifeste $dossier $version $Cle $Notes | Out-Null
if ($LASTEXITCODE) { throw "manifeste : échec" }
foreach ($f in Get-ChildItem $dossier -Recurse -File) {
    $nom = $f.FullName.Substring($dossier.Length + 1).Replace("\", ".")
    Copy-Item $f.FullName (Join-Path $sortie $nom)
}
Remove-Item -Recurse -Force $dossier
Copy-Item $zip, $installeur $sortie

$empreintes = foreach ($f in Get-Item $zip, $installeur) { "$((Get-FileHash $f -Algorithm SHA256).Hash.ToLower())  $($f.Name)" }
[IO.File]::WriteAllLines((Join-Path $sortie "SHA256.txt"), [string[]]$empreintes, (New-Object Text.UTF8Encoding $false))
$texte = @"
$Notes

Édition portable : télécharger ``FLASH-$version-portable.zip``, le dézipper, lancer ``flash.exe``.
Édition installable : ``FLASH-$version-installation.exe``, pour l'utilisateur seul.
Les versions déjà en place se mettent à jour toutes seules.

FLASH est sous licence GPL-3.0 ou plus récente ; les sources sont dans ce dépôt (le tag de la
version). SRT (MPL-2.0) : sources sur https://github.com/Haivision/srt.

Empreintes SHA-256 :
$($empreintes -join "`n")
"@
[IO.File]::WriteAllText((Join-Path $sortie "notes.md"), $texte.Trim() + "`n", (New-Object Text.UTF8Encoding $false))

Get-ChildItem $sortie | Select-Object Name, @{ n = "Ko"; e = { [math]::Round($_.Length / 1KB) } } | Format-Table -AutoSize | Out-String | Write-Host
if (-not $Publier) {
    "FLASH $version (commit $commit) prêt dans $sortie. Rien n'a été publié."
    return
}
# La Release porte le tag de ce commit : ses sources sont celles des binaires
$fichiers = Get-ChildItem $sortie -File | Where-Object { $_.Name -ne "notes.md" } | ForEach-Object { $_.FullName }
gh release create "v$version" @fichiers --repo $Depot --target (git -C $root rev-parse HEAD) --title "FLASH $version" --notes-file (Join-Path $sortie "notes.md")
if ($LASTEXITCODE) { throw "gh release : échec" }
"FLASH $version (commit $commit) publié sur $Depot."
