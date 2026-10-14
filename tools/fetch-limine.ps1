# WindClone — fetch the prebuilt Limine boot files (Windows / PowerShell).
# Needs: Windows 10+ (tar.exe is built in), internet access.
# After this, run the ISO build inside WSL or Linux (xorriso is not native
# to Windows). SPDX-License-Identifier: MIT
$ErrorActionPreference = 'Stop'

$Branch = if ($env:LIMINE_BRANCH) { $env:LIMINE_BRANCH } else { 'v9.x-binary' }
$RepoRoot = Split-Path -Parent $PSScriptRoot
$OutDir = Join-Path $RepoRoot 'limine'
$Tmp = Join-Path $env:TEMP ("limine-" + [guid]::NewGuid().ToString('N'))

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
New-Item -ItemType Directory -Force -Path $Tmp | Out-Null

$url = "https://github.com/limine-bootloader/limine/archive/refs/heads/$Branch.tar.gz"
$tar = Join-Path $Tmp 'limine.tar.gz'

Write-Host "[fetch-limine] downloading $url"
Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $tar

Write-Host "[fetch-limine] extracting"
tar -xzf $tar -C $Tmp
$src = Get-ChildItem $Tmp -Directory -Filter 'limine-*' | Select-Object -First 1

foreach ($f in @('limine-bios-cd.bin', 'limine-uefi-cd.bin', 'limine-bios.sys',
                 'BOOTX64.EFI', 'BOOTIA32.EFI', 'limine.h', 'limine.c', 'limine.exe')) {
    Copy-Item (Join-Path $src.FullName $f) (Join-Path $OutDir $f) -Force
    Write-Host "[fetch-limine]   -> limine/$f"
}

Remove-Item -Recurse -Force $Tmp
Write-Host '[fetch-limine] done. (ISO assembly still requires WSL/Linux: make)'
