# Turing GPU UNLOCK - Windows UEFI port. Adapted from
# sargentmajord/Windows-GPU-UNLOCK's internal-boot/boot/build-diagnostic.ps1
# (GPL-2.0-only); see THIRD_PARTY_NOTICES.md. No -HardwareProfile parameter here, unlike
# the Ampere original - this tool deliberately has no profile.h dependency at all (see
# probe.c's own header comment): it's meant to run BEFORE a profile exists, not after.
param([string]$Clang = 'clang', [string]$LldLink = 'lld-link', [string]$Python = 'python')
$ErrorActionPreference = 'Stop'
Push-Location -LiteralPath $PSScriptRoot
try {
 New-Item -ItemType Directory -Path build/diagnostic -Force | Out-Null
 $sources = @('handoff.c', 'diagnostic.c', 'probe.c', '../unlock/runtime.c', '../unlock/sha256.c')
 $objects = @()
 foreach ($source in $sources) {
  $obj = 'build/diagnostic/' + [IO.Path]::GetFileNameWithoutExtension($source) + '.obj'
  & $Clang -target x86_64-pc-windows-msvc -std=c11 -ffreestanding -fno-builtin -fno-stack-protector -mno-red-zone -fshort-wchar -Os -Wall -Wextra -Werror -DGU_DIAGNOSTIC_ONLY -DGU_DIAGNOSTIC_PROBE -I ../../deps/edk2/MdePkg/Include -I ../../deps/edk2/MdePkg/Include/X64 -c $source -o $obj
  if ($LASTEXITCODE -ne 0) { throw "Compile failed: $source" }
  $objects += $obj
 }
 & $LldLink /nologo /nodefaultlib /subsystem:efi_application /entry:efi_main /machine:x64 /base:0 /dynamicbase /nxcompat /timestamp:0 /opt:ref /opt:icf /map:build/diagnostic/BOOTX64.map /out:build/diagnostic/BOOTX64.EFI @objects
 if ($LASTEXITCODE -ne 0) { throw 'Diagnostic link failed' }
 & $Python ../unlock/verify_pe.py build/diagnostic/BOOTX64.EFI build/diagnostic/BOOTX64.map --output build/diagnostic/pe-verification.json
 if ($LASTEXITCODE -ne 0) { throw 'Diagnostic PE verification failed' }
 $sourceManifest = @(($sources + @('diagnostic.h', 'probe.h')) | ForEach-Object {
  [ordered]@{ Path = $_; Sha256 = (Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash }
 })
 [ordered]@{
  HardwareTested  = $false; Mode = 'read-only-capture-then-halt'; CanUnlock = $false
  CanDetach       = $false; WritesFirmwareVariables = $false
  EfiSha256       = (Get-FileHash -LiteralPath build/diagnostic/BOOTX64.EFI -Algorithm SHA256).Hash
  BuiltUtc        = [DateTime]::UtcNow.ToString('o'); Sources = $sourceManifest
 } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath build/diagnostic/manifest.json -Encoding utf8
 "Built BOOTX64.EFI (read-only capture); copy to a FAT32 USB stick as \EFI\BOOT\BOOTX64.EFI."
} finally { Pop-Location }
