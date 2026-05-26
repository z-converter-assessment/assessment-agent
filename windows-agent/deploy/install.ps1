# Assessment Agent (Windows) — installer
#
# Single entry point. Run from an elevated PowerShell:
#
#     PS C:\> cd path\to\assessment-agent-windows
#     PS C:\> .\deploy\install.ps1
#
# Re-runnable (idempotent). Each step short-circuits when already done:
#   - directories already created  → skipped
#   - service already registered    → skipped
#   - env keys already filled       → skipped (env-setup.ps1 only asks for empties)
#
# Flags:
#   -ImagePrep     register service but do NOT start (use before sealing a
#                  golden VM image; then run scripts\image-prep.ps1 to reset
#                  MachineGuid before snapshot)
#   -SkipSha256    skip SHA256 verification of dist\assessment-agent.exe
#                  (only when SHA256SUMS is intentionally absent)

#Requires -RunAsAdministrator

[CmdletBinding()]
param(
    [switch]$ImagePrep,
    [switch]$SkipSha256
)

$ErrorActionPreference = 'Stop'

# --- 0. Banner (echoes the image-clone caveat — machine_id collision is the
#        single biggest gotcha for fleet operators).
Write-Host ""
Write-Host "=== Assessment Agent installer ==="
Write-Host "NOTE: If this server was cloned from a VM image, the agent will inherit"
Write-Host "      the source machine's MachineGuid and the engine will overwrite"
Write-Host "      its records. Before cloning, run scripts\image-prep.ps1 on the"
Write-Host "      golden image (or use sysprep /generalize) to clear MachineGuid."
Write-Host ""

# --- 1. Resolve paths
$scriptRoot = $PSScriptRoot
$repoRoot   = (Resolve-Path "$scriptRoot\..").Path
$distDir    = "$repoRoot\dist"
$exeSource  = "$distDir\assessment-agent.exe"
$shaFile    = "$distDir\SHA256SUMS"
$envExample = "$scriptRoot\agent.env.example"

$progDir = 'C:\Program Files\assessment-agent'
$dataDir = 'C:\ProgramData\assessment-agent'

$exeTarget   = Join-Path $progDir 'assessment-agent.exe'
$envTarget   = Join-Path $dataDir 'agent.env'
$envLocal    = Join-Path $dataDir 'agent.env.local'

Write-Host "[install] repo root : $repoRoot"

# --- 2. Windows version gate (Server 2016 / Windows 10 = NT 10.0)
$os = [Environment]::OSVersion.Version
if ($os.Major -lt 10) {
    throw "Unsupported Windows version: $os. Requires Windows 10 / Server 2016 or later."
}
$caption = (Get-CimInstance Win32_OperatingSystem).Caption
Write-Host "[install] OS         : $caption ($os)"

# --- 3. Binary present?
if (-not (Test-Path $exeSource)) {
    throw "Binary not found: $exeSource`nRun 'make release' on a Windows build host first."
}

# --- 4. SHA256 verify (the build-host gate that the prod box trusts)
if ($SkipSha256) {
    Write-Warning "[install] -SkipSha256: skipping integrity check"
} elseif (Test-Path $shaFile) {
    $expectedLine = Get-Content $shaFile | Where-Object { $_ -match 'assessment-agent\.exe' } | Select-Object -First 1
    if (-not $expectedLine) {
        throw "SHA256SUMS does not list assessment-agent.exe"
    }
    $expected = ($expectedLine -split '\s+')[0].ToLower()
    $actual   = (Get-FileHash -Path $exeSource -Algorithm SHA256).Hash.ToLower()
    if ($expected -ne $actual) {
        throw "SHA256 mismatch:`n  expected $expected`n  actual   $actual"
    }
    Write-Host "[install] SHA256 OK  : $actual"
} else {
    Write-Warning "[install] SHA256SUMS not found ($shaFile) — proceeding without integrity check"
}

# --- 5. Prepare directories
New-Item -ItemType Directory -Path $progDir -Force | Out-Null
New-Item -ItemType Directory -Path $dataDir -Force | Out-Null

# Worker state dirs (task.install consumer). v1 binary 는 worker 비활성이지만,
# v2 도입 시 install.ps1 을 다시 돌리지 않아도 되도록 미리 생성. Linux 측의
# /var/lib/agent-worker/{results,done,running}/ 와 동일 역할.
$workerDir = Join-Path $dataDir 'worker'
foreach ($sub in 'results','done','running') {
    New-Item -ItemType Directory -Path (Join-Path $workerDir $sub) -Force | Out-Null
}

# --- 6. Stop existing service (upgrade path)
$svc = Get-Service -Name 'assessment-agent' -ErrorAction SilentlyContinue
if ($svc -and $svc.Status -ne 'Stopped') {
    Write-Host "[install] stopping existing service for upgrade..."
    Stop-Service -Name 'assessment-agent' -Force
    # SCM Stop is async — wait until really stopped before overwriting the exe
    for ($i = 0; $i -lt 30; $i++) {
        $s = (Get-Service -Name 'assessment-agent').Status
        if ($s -eq 'Stopped') { break }
        Start-Sleep -Milliseconds 500
    }
}

# --- 7. Copy binary
Copy-Item -Path $exeSource -Destination $exeTarget -Force
Write-Host "[install] binary     : $exeTarget"

# --- 8. Seed env file (idempotent — only on first run)
if (-not (Test-Path $envTarget)) {
    Copy-Item -Path $envExample -Destination $envTarget
    Write-Host "[install] seeded env : $envTarget (from agent.env.example)"
}

# --- 9. Prompt for missing env values (idempotent — skips filled keys)
& "$scriptRoot\env-setup.ps1" `
    -Example   $envExample `
    -EnvFile   $envTarget  `
    -LocalFile $envLocal

# --- 10. Tighten ACLs on the data dir (admins + SYSTEM only)
& icacls $dataDir /inheritance:r /grant:r 'SYSTEM:(OI)(CI)F' 'Administrators:(OI)(CI)F' | Out-Null

# --- 11. Register service (idempotent)
if (-not (Get-Service -Name 'assessment-agent' -ErrorAction SilentlyContinue)) {
    Write-Host "[install] registering service 'assessment-agent'..."
    New-Service -Name 'assessment-agent' `
                -BinaryPathName "`"$exeTarget`"" `
                -DisplayName 'Assessment Agent' `
                -Description 'Resource assessment collector — publishes inventory/metrics/error to RabbitMQ.' `
                -StartupType Automatic | Out-Null

    # Failure recovery — New-Service has no equivalent, fall back to sc.exe.
    # reset=86400 → failure counter resets after 1 day of clean run.
    # actions=restart/5s, /10s, /30s — first 3 failures get progressive backoff.
    & sc.exe failure assessment-agent reset= 86400 actions= restart/5000/restart/10000/restart/30000 | Out-Null
}

# --- 12. Start (unless preparing a golden image)
if ($ImagePrep) {
    Write-Host ""
    Write-Host "[install] image-prep mode — service registered, NOT started."
    Write-Host "[install] before sealing this VM into an image, run:"
    Write-Host "[install]     .\scripts\image-prep.ps1"
    Write-Host ""
    return
}

Write-Host "[install] starting service..."
Start-Service -Name 'assessment-agent'

# Wait briefly for the agent to actually transition to Running.
for ($i = 0; $i -lt 15; $i++) {
    $s = (Get-Service -Name 'assessment-agent').Status
    if ($s -eq 'Running') { break }
    Start-Sleep -Milliseconds 500
}

$final = (Get-Service -Name 'assessment-agent').Status
if ($final -eq 'Running') {
    Write-Host ""
    Write-Host "[install] OK — assessment-agent is running."
    Write-Host "[install] view logs:  Get-EventLog -LogName Application -Source assessment-agent -Newest 20"
    Write-Host "[install] stop:       Stop-Service assessment-agent"
    Write-Host "[install] uninstall:  Stop-Service assessment-agent; sc.exe delete assessment-agent"
} else {
    Write-Warning "[install] service status: $final — recent application events:"
    try {
        Get-WinEvent -LogName Application -MaxEvents 20 `
            | Where-Object { $_.ProviderName -match 'assessment' -or $_.Message -match 'assessment-agent' } `
            | Format-Table TimeCreated, LevelDisplayName, Message -AutoSize
    } catch {
        Write-Host "(no events; check Event Viewer manually)"
    }
    throw "service failed to start — see events above."
}
