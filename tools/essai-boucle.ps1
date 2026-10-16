# Essai en boucle locale : l'appli FLASH vers l'OBS portable de test (.test-obs), avec le
# journal de mesure du plugin, puis analyse.
# -Banc : au lieu de la mire, capture réelle de la fenêtre d'essai (WGC) et de son son
# (loopback de l'application), comme pour un jeu.
# Ne touche jamais à l'OBS habituel : seuls les processus lancés ici sont arrêtés, par PID.
param(
    [int]$Duree = 60,
    [int]$Latence = 20,
    [switch]$Enregistrer,
    [string[]]$Sources = @("--mire"),
    [switch]$Banc,
    [int]$EcranBanc = 0,          # 0 = dernier écran
    [string]$SortieBanc = "Steam Streaming Speakers",
    [string]$Collection = "FlashTest"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$obsDir = Join-Path $root ".test-obs\bin\64bit"
$out = Join-Path $root ".test-obs\essais"
New-Item -ItemType Directory -Force $out | Out-Null
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$journal = Join-Path $out "mesure-$stamp.csv"

$app = Join-Path $root "build\app\flash.exe"
$bancProc = $null
if ($Banc) {
    # Le banc tourne sous un autre nom d'exécutable pour que son son soit désigné sans ambiguïté
    $bancDir = Join-Path $out "banc"
    New-Item -ItemType Directory -Force $bancDir | Out-Null
    Copy-Item $app (Join-Path $bancDir "banc.exe") -Force
    Copy-Item (Join-Path (Split-Path $app) "srt.dll") $bancDir -Force
    $bancProc = Start-Process -FilePath (Join-Path $bancDir "banc.exe") `
        -ArgumentList "--banc", "--sortie", "`"$SortieBanc`"", "--ecran", $EcranBanc, "--duree", ($Duree + 20) `
        -WorkingDirectory $bancDir -RedirectStandardError (Join-Path $out "banc-$stamp.log") -WindowStyle Hidden -PassThru
    Start-Sleep -Seconds 1
    if (-not $PSBoundParameters.ContainsKey("Sources")) {
        $Sources = @("--fenetre", "`"Banc FLASH`"", "--appli", "banc.exe")
    }
}
$sender = Start-Process -FilePath $app -ArgumentList ($Sources + @("--duree", ($Duree + 10), "--latence", $Latence)) `
    -WorkingDirectory (Split-Path $app) -RedirectStandardError (Join-Path $out "envoi-$stamp.log") `
    -WindowStyle Hidden -PassThru

# Collection de scènes de l'OBS de test (FlashTest : une vidéo et un son ; FlashMulti : trois et trois)
$userIni = Join-Path $root ".test-obs\config\obs-studio\user.ini"
$ini = (Get-Content $userIni) -replace "^SceneCollection=.*", "SceneCollection=$Collection" `
    -replace "^SceneCollectionFile=.*", "SceneCollectionFile=$Collection"
[IO.File]::WriteAllLines($userIni, $ini, (New-Object Text.UTF8Encoding $false))

$env:FLASH_MESURE = $journal
$obsArgs = @("--portable", "--multi", "--disable-shutdown-check", "--disable-updater")
if ($Enregistrer) { $obsArgs += "--startrecording" }
$obs = Start-Process -FilePath (Join-Path $obsDir "obs64.exe") -ArgumentList $obsArgs -WorkingDirectory $obsDir `
    -WindowStyle Minimized -PassThru
Remove-Item Env:FLASH_MESURE

Write-Host "Essai de $Duree s (envoi PID $($sender.Id), OBS de test PID $($obs.Id))"

# Relevé de charge pendant l'essai, une fois l'ensemble lancé : processeur de l'envoi et
# de l'OBS de test (en % du processeur entier), et GPU (3D, encodeur, décodeur)
Start-Sleep -Seconds 8
$cores = [Environment]::ProcessorCount
$samples = @()
$t0 = Get-Date
$cpu0 = @{ s = $sender.TotalProcessorTime.TotalSeconds; o = $obs.TotalProcessorTime.TotalSeconds }
while (((Get-Date) - $t0).TotalSeconds -lt ($Duree - 5)) {
    $g = (nvidia-smi --query-gpu=utilization.gpu,utilization.encoder,utilization.decoder --format=csv,noheader,nounits) -split ","
    $samples += , @([int]$g[0], [int]$g[1], [int]$g[2])
    Start-Sleep -Seconds 2
}
$sender.Refresh(); $obs.Refresh()
$el = ((Get-Date) - $t0).TotalSeconds
$cpuS = ($sender.TotalProcessorTime.TotalSeconds - $cpu0.s) / $el / $cores * 100
$cpuO = ($obs.TotalProcessorTime.TotalSeconds - $cpu0.o) / $el / $cores * 100
$avg = { param($i) ($samples | ForEach-Object { $_[$i] } | Measure-Object -Average).Average }
Write-Host ("Charge : envoi {0:N2} % CPU, OBS de test {1:N2} % CPU, GPU 3D {2:N0} %, encodeur {3:N0} %, décodeur {4:N0} %" -f `
    $cpuS, $cpuO, (& $avg 0), (& $avg 1), (& $avg 2))
$reste = $Duree + 5 - 8 - $el
if ($reste -gt 0) { Start-Sleep -Seconds $reste }

# Fermeture propre de l'OBS de test, puis arrêt forcé par PID s'il ne répond pas
$obs.Refresh()
if (-not $obs.HasExited) {
    $obs.CloseMainWindow() | Out-Null
    if (-not $obs.WaitForExit(20000)) {
        $check = tasklist /FI "PID eq $($obs.Id)" /FI "IMAGENAME eq obs64.exe" /NH
        if ($check -match "obs64.exe") { taskkill /F /FI "PID eq $($obs.Id)" /FI "IMAGENAME eq obs64.exe" | Out-Null }
    }
}
if (-not $sender.WaitForExit(20000)) {
    taskkill /F /FI "PID eq $($sender.Id)" /FI "IMAGENAME eq flash.exe" | Out-Null
}
if ($bancProc -and -not $bancProc.WaitForExit(15000)) {
    taskkill /F /FI "PID eq $($bancProc.Id)" /FI "IMAGENAME eq banc.exe" | Out-Null
}

python -I (Join-Path $root "tools\analyse_mesure.py") $journal
Write-Host "Journal : $journal"
