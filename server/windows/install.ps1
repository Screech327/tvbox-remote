<#
  tvbox-remote: Windows host installer (Windows 10/11, 64-bit).

  Installs the stream (ffmpeg + MediaMTX) and the control page into %LOCALAPPDATA%\tvbox-remote, finds the
  capture card, picks the best video encoder (NVIDIA / Intel / AMD / CPU) and starts it at every log-on.
  No admin needed, except one prompt for the firewall rules.

  Run it in PowerShell (not as admin):
      irm https://raw.githubusercontent.com/Screech327/tvbox-remote/main/server/windows/install.ps1 | iex

  With options (from a downloaded copy):
      powershell -ExecutionPolicy Bypass -File install.ps1 [-PicoIp 192.168.1.50] [-Token xyz] [-Name vSeeBox]
                                                         [-TestPattern] [-Update] [-Restart] [-Uninstall]
    -TestPattern  stream a test picture instead of a capture card (to try everything else first)
    -Update       download ffmpeg / MediaMTX / Python again, even if they're already there
    -Restart      restart it (after editing tvbox.env)
    -Firewall     only (re)create the firewall rules
    -Uninstall    stop it and remove everything it installed
#>
[CmdletBinding()]
param(
    [string]$PicoIp,
    [string]$Token,
    [string]$Name,
    [switch]$TestPattern,
    [switch]$Update,
    [switch]$Restart,
    [switch]$Firewall,
    [switch]$Uninstall,
    [string]$Ref = 'main'
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'          # Windows PowerShell downloads crawl with the progress bar on
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$Repo     = 'Screech327/tvbox-remote'
$Raw      = "https://raw.githubusercontent.com/$Repo/$Ref"
$Dir      = Join-Path $env:LOCALAPPDATA 'tvbox-remote'
$Python   = Join-Path $Dir 'python\python.exe'
$PythonW  = Join-Path $Dir 'python\pythonw.exe'
$Host_    = Join-Path $Dir 'tvbox-host.pyw'
$EnvFile  = Join-Path $Dir 'tvbox.env'
$Startup  = Join-Path ([Environment]::GetFolderPath('Startup')) 'tvbox-remote.lnk'
$MenuLink = Join-Path ([Environment]::GetFolderPath('Programs')) 'TV Box Remote.url'
$Rule     = 'tvbox-remote'

# Pinned fallbacks, used only if the GitHub API can't be reached. Checked Nov 2026.
$PyUrl       = 'https://www.python.org/ftp/python/3.15.2/python-3.15.2-embed-amd64.zip'
$MtxFallback = 'https://github.com/bluenviron/mediamtx/releases/download/v1.21.3/mediamtx_v1.21.3_windows_amd64.zip'
$FfFallback  = 'https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-n8.1-latest-win64-gpl-8.1.zip'

function Say($msg)  { Write-Host "== $msg" -ForegroundColor Cyan }
function Note($msg) { Write-Host "   $msg" }
function Warn($msg) { Write-Host "   $msg" -ForegroundColor Yellow }

function Stop-TvBox {
    # Everything we run lives under $Dir: the host (pythonw), MediaMTX and ffmpeg.
    Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -and $_.ExecutablePath -like "$Dir\*" } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Seconds 1
}

function Get-EnvValue($key, $default) {
    if (Test-Path $EnvFile) {
        $line = Get-Content $EnvFile | Where-Object { $_ -match "^$key=" } | Select-Object -First 1
        if ($line) { return $line.Substring($key.Length + 1) }
    }
    return $default
}

function Start-TvBox {
    Start-Process -FilePath $PythonW -ArgumentList "`"$Host_`"" -WorkingDirectory $Dir
    $port = Get-EnvValue 'PORT' '80'
    for ($i = 0; $i -lt 20; $i++) {
        Start-Sleep -Seconds 1
        try { Invoke-WebRequest -UseBasicParsing "http://127.0.0.1:$port/healthz" -TimeoutSec 2 | Out-Null; return $true }
        catch { }
    }
    return $false
}

function Invoke-Admin($script) {
    # Runs a small script elevated (one UAC prompt). The elevated script writes "OK" or its error to a file,
    # because an elevated process's exit code isn't reliable here. Returns $true only if it really worked.
    $result = Join-Path $Dir 'logs\admin-result.txt'
    Remove-Item $result -Force -ErrorAction SilentlyContinue
    $full = "try { `$ErrorActionPreference='Stop'; $script; 'OK' | Set-Content -LiteralPath '$($result -replace "'", "''")' } " +
            "catch { `$_.Exception.Message | Set-Content -LiteralPath '$($result -replace "'", "''")' }"
    $bytes = [Text.Encoding]::Unicode.GetBytes($full)
    try {
        Start-Process -FilePath "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -Verb RunAs -Wait `
            -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-EncodedCommand', [Convert]::ToBase64String($bytes)
    } catch {
        Warn "Admin prompt didn't run: $($_.Exception.Message)"
        return $false
    }
    $out = if (Test-Path $result) { (Get-Content $result -Raw).Trim() } else { 'no result (prompt declined?)' }
    if ($out -ne 'OK') { Warn "Admin step failed: $out" }
    return ($out -eq 'OK')
}

function Get-File($url, $out) {
    Note "downloading $(Split-Path $url -Leaf)"
    Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $out
}

function Get-LatestAsset($repo, $pattern, $fallback) {
    try {
        $rel = Invoke-RestMethod -UseBasicParsing "https://api.github.com/repos/$repo/releases/latest" -TimeoutSec 20
        $assets = @($rel.assets | Where-Object { $_.name -match $pattern })
        if ($assets.Count -gt 0) {
            # Highest version number first (matters for ffmpeg's floating "latest" release with several branches)
            $best = $assets | Sort-Object { [version](($_.name -replace '^\D*(\d+\.\d+(\.\d+)?).*$', '$1')) } -Descending |
                    Select-Object -First 1
            return $best.browser_download_url
        }
    } catch { }
    return $fallback
}

function Expand-To($zip, $dest) {
    if (Test-Path $dest) { Remove-Item $dest -Recurse -Force }
    Expand-Archive -Path $zip -DestinationPath $dest -Force
    Remove-Item $zip -Force
}

function Get-AppFile($repoPath, $dest) {
    # Use the files next to this script if it's run from a copy of the repo, otherwise download them.
    $local = if ($PSScriptRoot) { Join-Path $PSScriptRoot "..\..\$repoPath" } else { $null }
    if ($local -and (Test-Path $local)) { Copy-Item $local $dest -Force }
    else { Invoke-WebRequest -UseBasicParsing -Uri "$Raw/$repoPath" -OutFile $dest }
}

function Test-PortFree($port) {
    try {
        $l = New-Object Net.Sockets.TcpListener([Net.IPAddress]::Any, $port)
        $l.Start(); $l.Stop(); return $true
    } catch { return $false }
}

function Set-FirewallRules {
    Say 'Firewall'
    Note 'Windows will ask for admin once, to let your home network and Tailscale reach the page and the stream.'
    $pyw = $PythonW -replace "'", "''"                 # quote-safe for the elevated script
    $mtx = "$Dir\mediamtx\mediamtx.exe" -replace "'", "''"
    $fw = "Remove-NetFirewallRule -DisplayName '$Rule*' -ErrorAction SilentlyContinue; " +
          "New-NetFirewallRule -DisplayName '$Rule (control page)' -Direction Inbound -Action Allow -Program '$pyw' -RemoteAddress LocalSubnet,100.64.0.0/10 -Profile Any | Out-Null; " +
          "New-NetFirewallRule -DisplayName '$Rule (stream)' -Direction Inbound -Action Allow -Program '$mtx' -RemoteAddress LocalSubnet,100.64.0.0/10 -Profile Any | Out-Null"
    if (Invoke-Admin $fw) { Note 'Allowed: your local network and Tailscale (100.64.0.0/10) only.' }
    else { Warn 'Skipped. When Windows asks about Python or MediaMTX the first time, click Allow (Private networks).' }
}

# ------------------------------------------------------------------------------------------------------------

if (-not [Environment]::Is64BitOperatingSystem) { throw 'tvbox-remote needs 64-bit Windows 10 or 11.' }

if ($Uninstall) {
    Say 'Removing tvbox-remote'
    Stop-TvBox
    Remove-Item $Startup, $MenuLink -Force -ErrorAction SilentlyContinue
    if (-not (Invoke-Admin "Remove-NetFirewallRule -DisplayName '$Rule*' -ErrorAction SilentlyContinue")) {
        Warn "Couldn't remove the firewall rules (admin prompt declined). They're named '$Rule ...' if you want to remove them by hand."
    }
    Remove-Item $Dir -Recurse -Force -ErrorAction SilentlyContinue
    Note 'Done. (Sleep settings were left as they are.)'
    return
}

if ($Firewall) { Set-FirewallRules; return }

if ($Restart) {
    Say 'Restarting tvbox-remote'
    Stop-TvBox
    if (Start-TvBox) { Note "Running: http://localhost:$(Get-EnvValue 'PORT' '80')/" }
    else { Warn "It didn't come up. Check $Dir\logs\host.log" }
    return
}

Say "Installing tvbox-remote into $Dir"
New-Item -ItemType Directory -Force -Path $Dir, "$Dir\ffmpeg", "$Dir\logs" | Out-Null
Stop-TvBox

if ($Update -or -not (Test-Path $Python)) {
    Get-File $PyUrl "$Dir\python.zip"
    Expand-To "$Dir\python.zip" "$Dir\python"
}
if ($Update -or -not (Test-Path "$Dir\mediamtx\mediamtx.exe")) {
    $url = Get-LatestAsset 'bluenviron/mediamtx' '^mediamtx_v[\d.]+_windows_amd64\.zip$' $MtxFallback
    Get-File $url "$Dir\mediamtx.zip"
    Expand-To "$Dir\mediamtx.zip" "$Dir\mediamtx"
}
if ($Update -or -not (Test-Path "$Dir\ffmpeg\ffmpeg.exe")) {
    $url = Get-LatestAsset 'BtbN/FFmpeg-Builds' '^ffmpeg-n\d+\.\d+-latest-win64-gpl-\d+\.\d+\.zip$' $FfFallback
    Get-File $url "$Dir\ffmpeg.zip"          # about 200 MB; only ffmpeg.exe is kept
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead("$Dir\ffmpeg.zip")
    try {
        $entry = $zip.Entries | Where-Object { $_.FullName -like '*/bin/ffmpeg.exe' } | Select-Object -First 1
        [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, "$Dir\ffmpeg\ffmpeg.exe", $true)
    } finally { $zip.Dispose() }
    Remove-Item "$Dir\ffmpeg.zip" -Force
}

Say 'Copying the control page'
Get-AppFile 'server/control/server.py'      "$Dir\server.py"
Get-AppFile 'server/control/index.html'     "$Dir\index.html"
Get-AppFile 'server/windows/tvbox-host.pyw' $Host_
Get-AppFile 'server/stream/mediamtx.yml'    "$Dir\mediamtx\mediamtx.yml"

# Port for the control page: 80 (no :port in the address) unless something else already uses it.
$port = Get-EnvValue 'PORT' ''
if (-not $port) {
    $port = if (Test-PortFree 80) { '80' } else { '8080' }
    if ($port -eq '8080') { Warn 'Port 80 is in use by another program, so the page will be on port 8080.' }
}

$setupArgs = @($Host_, '--setup', '--port', $port)
if ($TestPattern) { $setupArgs += '--test-pattern' }
if ($PicoIp)      { $setupArgs += @('--pico', $PicoIp) }
if ($Token)       { $setupArgs += @('--token', $Token) }
if ($Name)        { $setupArgs += @('--name', $Name) }
& $Python @setupArgs
if ($LASTEXITCODE -ne 0) { throw 'Setup stopped (see the message above). Fix it and run the installer again.' }

Set-FirewallRules

Say 'Start at log-on'
$ws = New-Object -ComObject WScript.Shell
$lnk = $ws.CreateShortcut($Startup)
$lnk.TargetPath = $PythonW
$lnk.Arguments = "`"$Host_`""
$lnk.WorkingDirectory = $Dir
$lnk.Description = 'tvbox-remote: TV box stream + control page'
$lnk.Save()
Set-Content -Path $MenuLink -Value "[InternetShortcut]`r`nURL=http://localhost:$port/" -Encoding ASCII
Note 'Starts automatically when you log in. Start menu: "TV Box Remote" opens the page.'

Say 'Sleep'
$answer = Read-Host '   This PC has to stay awake to stream. Turn off sleep while plugged in? (Y/n)'
if ($answer -notmatch '^[nN]') {
    powercfg /change standby-timeout-ac 0
    powercfg /change hibernate-timeout-ac 0
    Note 'Sleep is off while plugged in. (The screen can still turn off; that is fine.)'
} else { Warn 'Left as is. If the PC sleeps, the stream and remote stop until it wakes up.' }

Say 'Starting'
if (Start-TvBox) {
    $ips = Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
           Where-Object { $_.IPAddress -notlike '127.*' -and $_.IPAddress -notlike '169.254.*' } |
           Select-Object -ExpandProperty IPAddress
    $sfx = if ($port -eq '80') { '' } else { ":$port" }
    Note "Running. Open it on this PC:  http://localhost$sfx/"
    foreach ($ip in $ips) {
        $what = if ($ip -like '100.*') { '(Tailscale, from anywhere)' } else { '(other devices at home)' }
        Note "                              http://$ip$sfx/   $what"
    }
    Note "Logs: $Dir\logs   Settings: $EnvFile"
    Start-Process "http://localhost$sfx/"
} else {
    Warn "It didn't start. Check $Dir\logs\host.log"
}
