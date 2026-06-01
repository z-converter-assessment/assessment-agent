# env-setup.ps1
#
# Idempotent env-file populator. Called by install.ps1 — also runnable
# standalone to fix a single missing key:
#
#     .\deploy\env-setup.ps1 `
#         -Example   .\deploy\agent.env.example `
#         -EnvFile   C:\ProgramData\assessment-agent\agent.env `
#         -LocalFile C:\ProgramData\assessment-agent\agent.env.local
#
# Rules:
#   - reads the canonical key list from -Example
#   - non-secret keys go in -EnvFile; existing non-empty values are NEVER
#     overwritten
#   - only $PromptKeys are interactively prompted when missing. Other
#     non-secret keys silently fall back to the example default — most
#     operators run on standard topology and never change those values.
#   - secret keys (RABBITMQ_PASS, RABBITMQ_WORKER_PASS) go in -LocalFile,
#     entered via Read-Host -AsSecureString. ACLs tightened to SYSTEM +
#     Administrators only.
#   - atomic write: build full content, write to temp, Move-Item -Force

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Example,
    [Parameter(Mandatory = $true)][string]$EnvFile,
    [Parameter(Mandatory = $true)][string]$LocalFile
)

$ErrorActionPreference = 'Stop'

# Keys we *do* interactively prompt for. Mirror of PROMPT_KEYS in
# deploy/lib/env-setup.sh — keep the two lists in sync.
$PromptKeys = @('RABBITMQ_HOST', 'WORKER_DOWNLOAD_ALLOWED_HOSTS')

$SecretKeys = @('RABBITMQ_PASS', 'RABBITMQ_WORKER_PASS')

function Parse-Env([string]$path) {
    $h = [ordered]@{}
    if (-not (Test-Path -LiteralPath $path)) { return $h }
    foreach ($line in Get-Content -LiteralPath $path -Encoding UTF8) {
        if ($line -match '^\s*#') { continue }
        if ($line -match '^\s*$') { continue }
        if ($line -match '^\s*([A-Z_][A-Z0-9_]*)\s*=\s*(.*)$') {
            $v = $Matches[2].Trim()
            if ($v.Length -ge 2 -and (($v[0] -eq '"' -and $v[-1] -eq '"') -or
                                      ($v[0] -eq "'" -and $v[-1] -eq "'"))) {
                $v = $v.Substring(1, $v.Length - 2)
            }
            $h[$Matches[1]] = $v
        }
    }
    return $h
}

function Save-Env([string]$path, $hash) {
    $lines = @()
    foreach ($key in $hash.Keys) {
        $lines += ('{0}={1}' -f $key, $hash[$key])
    }
    $tmp = "$path.tmp"
    # UTF8 *without* BOM — Linux text consumers also read this format.
    [IO.File]::WriteAllLines($tmp, $lines, (New-Object Text.UTF8Encoding $false))
    Move-Item -LiteralPath $tmp -Destination $path -Force
}

function Read-Secret([string]$prompt) {
    $sec = Read-Host -Prompt $prompt -AsSecureString
    if ($sec.Length -eq 0) { return '' }
    $bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($sec)
    try {
        return [Runtime.InteropServices.Marshal]::PtrToStringAuto($bstr)
    } finally {
        [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr)
    }
}

# --- Parse example: canonical key list + suggested defaults
if (-not (Test-Path -LiteralPath $Example)) {
    throw "agent.env.example not found at $Example"
}
$exampleHash  = [ordered]@{}
$exampleOrder = New-Object System.Collections.ArrayList
foreach ($line in Get-Content -LiteralPath $Example -Encoding UTF8) {
    # commented-out keys (typically secrets) are tracked only via $SecretKeys.
    if ($line -match '^\s*([A-Z_][A-Z0-9_]*)\s*=\s*(.*)$') {
        $exampleHash[$Matches[1]] = $Matches[2].Trim()
        [void]$exampleOrder.Add($Matches[1])
    }
}

# --- Non-secret keys → $EnvFile
$existing = Parse-Env $EnvFile
foreach ($key in $exampleOrder) {
    if ($SecretKeys -contains $key) { continue }
    if ($existing.Contains($key) -and -not [string]::IsNullOrWhiteSpace($existing[$key])) {
        continue  # already filled — never overwrite
    }
    $def = $exampleHash[$key]
    if ($PromptKeys -contains $key) {
        if (-not [string]::IsNullOrWhiteSpace($def)) {
            $entered = Read-Host "$key [$def]"
            if ([string]::IsNullOrWhiteSpace($entered)) { $entered = $def }
        } else {
            $entered = Read-Host "$key"
        }
        $existing[$key] = $entered
    } else {
        # Silent default — operator can edit the env file post-install if a
        # non-standard value is required.
        $existing[$key] = $def
    }
}
Save-Env $EnvFile $existing
Write-Host "[env-setup] wrote $EnvFile"

# --- Secret keys → $LocalFile (strict ACL)
$localExisting = Parse-Env $LocalFile
foreach ($secret in $SecretKeys) {
    if ($localExisting.Contains($secret) -and -not [string]::IsNullOrWhiteSpace($localExisting[$secret])) {
        continue
    }
    $val = Read-Secret "$secret (입력 시 화면에 표시되지 않습니다)"
    if (-not [string]::IsNullOrWhiteSpace($val)) {
        $localExisting[$secret] = $val
    }
}
Save-Env $LocalFile $localExisting

# Tighten ACL: SYSTEM + Administrators only. /inheritance:r drops inherited
# ACEs; /grant:r replaces. Out-Null suppresses verbose icacls output.
& icacls $LocalFile /inheritance:r /grant:r 'SYSTEM:F' 'Administrators:F' | Out-Null
Write-Host "[env-setup] wrote $LocalFile (ACL: SYSTEM + Administrators only)"
