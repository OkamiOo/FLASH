# Essai en boucle locale : l'appli FLASH vers l'OBS portable de test (.test-obs), avec le
# journal de mesure du plugin, puis analyse.
# -Banc : au lieu de la mire, capture réelle de la fenêtre d'essai (WGC) et de son son
# (loopback de l'application), comme pour un jeu.
# -Appaire : appairage du plugin avec le code, sources par identifiant d'émetteur, flux chiffré.
# Ne touche jamais à l'OBS habituel : seuls les processus lancés ici sont arrêtés, par PID.
param(
    [int]$Duree = 60,
    [int]$Latence = 20,
    [switch]$Enregistrer,
    [string[]]$Sources = @("--mire"),
    [switch]$Banc,
    [int]$EcranBanc = 0,          # 0 = dernier écran
    [string]$SortieBanc = "Steam Streaming Speakers",
    [string]$Collection = "FlashTest",
    [switch]$Appaire
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
# Sans -Appaire, --libre : les scènes de test se connectent par adresse, sans appairage.
# Avec -Appaire, l'envoi n'accepte que les récepteurs appairés et le flux est chiffré.
$envoiArgs = $Sources + @("--duree", ($Duree + 10), "--latence", $Latence)
if (-not $Appaire) { $envoiArgs += "--libre" }
$sender = Start-Process -FilePath $app -ArgumentList $envoiArgs `
    -WorkingDirectory (Split-Path $app) -RedirectStandardError (Join-Path $out "envoi-$stamp.log") `
    -WindowStyle Hidden -PassThru

if ($Appaire) {
    # Le plugin de l'OBS de test s'appaire avec l'émetteur, puis ses sources visent l'émetteur
    # par son identifiant et les pistes par leur nom
    Start-Sleep -Seconds 2
    $cfg = Get-Content (Join-Path $env:APPDATA "FLASH\reglages.json") -Raw | ConvertFrom-Json
    $carnet = Join-Path $root ".test-obs\config\obs-studio\plugin_config\flash-obs\flash.json"
    New-Item -ItemType Directory -Force (Split-Path $carnet) | Out-Null
    & (Join-Path $root "build\tools\essai-appairage.exe") 127.0.0.1 $cfg.code $carnet

    $items = @(); $listeSources = @(); $i = 1
    foreach ($s in @(@("flash_video_source", "FLASH vidéo", "Mire"), @("flash_audio_source", "FLASH audio", "Bip"))) {
        $uuid = [guid]::NewGuid().ToString()
        $listeSources += @{ id = $s[0]; versioned_id = $s[0]; name = $s[1]; uuid = $uuid; enabled = $true; muted = $false
            volume = 1.0; balance = 0.5; mixers = 255; sync = 0; flags = 0; monitoring_type = 0; private_settings = @{}
            settings = @{ emetteur = $cfg.id; piste = $s[2]; latence = $Latence } }
        $items += @{ name = $s[1]; source_uuid = $uuid; visible = $true; locked = $false; id = $i; align = 5; rot = 0.0
            pos = @{ x = 0.0; y = 0.0 }; scale = @{ x = 1.0; y = 1.0 }; bounds_type = 0; bounds_align = 0
            bounds = @{ x = 0.0; y = 0.0 }; scale_filter = "disable"; blend_method = "default"; blend_type = "normal"
            private_settings = @{} }
        $i++
    }
    $scene = @{ id = "scene"; versioned_id = "scene"; name = "Scène"; uuid = [guid]::NewGuid().ToString()
        enabled = $true; mixers = 255; private_settings = @{}; settings = @{ items = $items; id_counter = $i; custom_size = $false } }
    $col = @{ name = "FlashAppairage"; current_scene = "Scène"; current_program_scene = "Scène"
        scene_order = @(@{ name = "Scène" }); sources = @($scene) + $listeSources; groups = @(); transitions = @()
        quick_transitions = @(); saved_projectors = @(); current_transition = "Fondu"; transition_duration = 300
        preview_locked = $false; scaling_enabled = $false; modules = @{} }
    $scenes = Join-Path $root ".test-obs\config\obs-studio\basic\scenes\FlashAppairage.json"
    [IO.File]::WriteAllText($scenes, ($col | ConvertTo-Json -Depth 10), (New-Object Text.UTF8Encoding $false))
    $Collection = "FlashAppairage"
}

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
