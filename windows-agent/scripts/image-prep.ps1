# image-prep.ps1
#
# Run this on the GOLDEN VM IMAGE before snapshotting / sealing.
#
# Without this step, every VM cloned from the image inherits the same
# MachineGuid (HKLM\SOFTWARE\Microsoft\Cryptography\MachineGuid). The agent
# uses MachineGuid as `machine_id`; cloned VMs all publish under the same
# identifier and the engine overwrites records on every receive.
#
# What this does:
#   1. Stop the agent service (it should be stopped already, but defensive)
#   2. Generate a fresh MachineGuid in the registry
#   3. Remind the operator that sysprep /generalize is the most thorough
#      generalization step (handles SID, hostname, license — out of scope here)

#Requires -RunAsAdministrator

[CmdletBinding()]
param(
    [switch]$RunSysprep   # also kicks sysprep /generalize /oobe /shutdown
)

$ErrorActionPreference = 'Stop'

Write-Host ""
Write-Host "=== Assessment Agent — image preparation ==="
Write-Host "Run this once on the GOLDEN TEMPLATE before snapshotting."
Write-Host ""

# --- 1. Make sure the agent is not currently running.
$svc = Get-Service -Name 'assessment-agent' -ErrorAction SilentlyContinue
if ($svc -and $svc.Status -eq 'Running') {
    Write-Host "[image-prep] stopping assessment-agent service..."
    Stop-Service -Name 'assessment-agent' -Force
    Start-Sleep -Seconds 2
}

# --- 2. Reset MachineGuid.
#
# Windows does NOT auto-regenerate MachineGuid on boot (unlike systemd's
# /etc/machine-id behavior). We install a new GUID; the agent will read this
# value on first run after the image clone boots.
#
# Existing value is kept for reference in the host's audit trail (Event Log).
$prevGuid = (Get-ItemProperty -Path 'HKLM:\SOFTWARE\Microsoft\Cryptography' `
                              -Name 'MachineGuid' -ErrorAction SilentlyContinue).MachineGuid
$newGuid = [guid]::NewGuid().ToString()
Set-ItemProperty -Path 'HKLM:\SOFTWARE\Microsoft\Cryptography' `
                 -Name 'MachineGuid' -Value $newGuid -Force
Write-Host "[image-prep] MachineGuid"
Write-Host "[image-prep]   was : $prevGuid"
Write-Host "[image-prep]   now : $newGuid"

# --- 3. Clear leftover agent.env.local secret values? — NO.
#
# Secrets in C:\ProgramData\assessment-agent\agent.env.local are operator-
# scoped credentials, not per-machine. We keep them so the cloned VM doesn't
# require re-entering broker passwords on first boot. If the credentials are
# per-machine, the operator should delete agent.env.local manually before
# this step.

# --- 4. Optional: sysprep generalize.
if ($RunSysprep) {
    Write-Host "[image-prep] launching sysprep /generalize /oobe /shutdown..."
    & "$env:SystemRoot\System32\Sysprep\Sysprep.exe" /generalize /oobe /shutdown
} else {
    Write-Host ""
    Write-Host "[image-prep] recommended next step (for full generalization):"
    Write-Host "    $env:SystemRoot\System32\Sysprep\Sysprep.exe /generalize /oobe /shutdown"
    Write-Host "  or rerun this script with -RunSysprep"
    Write-Host ""
}

Write-Host "[image-prep] done."
