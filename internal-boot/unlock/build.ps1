# Turing GPU UNLOCK - Windows UEFI port. Adapted from sargentmajord/Windows-GPU-UNLOCK's
# internal-boot/unlock/build.ps1 (GPL-2.0-only); see THIRD_PARTY_NOTICES.md.
#
# Deliberately scoped down from the Ampere original, the same way app.c/app_io.c already are (see their
# own header comments): no -AutomaticBoot/-InternalBoot/-HeadlessResearch/-StockProfile here. Those are
# refinements the Ampere build picked up only after its basic manual-unlock path was validated on real
# hardware; nothing analogous exists or should be invented for Turing yet (see PORTING-PLAN.md). This
# script only ever builds one of two things:
#   (default)           - diagnostic/preflight only (GU_PREFLIGHT_ONLY=1): stops after ColdCheck, never arms.
#   -ExperimentalUnlock  - the one real unlock transaction: manual "U" confirmation, Esc/30s-timeout cancels.
#
# NOT RUN in this environment: there is no clang/lld-link toolchain installed here to actually invoke this
# script with. It has been checked line-by-line against the exact flags, include paths and compat/ shim
# the Ampere build already uses successfully (same deps/edk2 headers, same compat/ copied unchanged - see
# PORTING-PLAN.md's porting-technique section) and the source list below was confirmed directly, file by
# file, against what each vendor file actually #includes (see PORTING-PLAN.md: layout.c/prep.c/vbios.c/
# boot.c need no Linux-API adaptation at all; unlock.c and gfw_eval.c do and already have it). But this
# script itself has not been executed anywhere. Do not read "this script exists" as "this has been built."
# Separately, until a real profiles/<name>/profile.h exists (none does yet - see machine-profile.h and
# profiles/TEMPLATE/profile.h), any attempt to actually compile app.c/checks.c against the TEMPLATE
# profile will fail on purpose, via its own #error - that is the intended behavior, not a bug here.
param(
 [string]$Clang = 'clang',
 [string]$LldLink = 'lld-link',
 [string]$Python = 'python',
 [switch]$ExperimentalUnlock,
 [string]$HardwareProfile = 'TEMPLATE'
)
$ErrorActionPreference = 'Stop'
Push-Location -LiteralPath $PSScriptRoot
try {
 $profileDir = if ($env:CPATH) { ($env:CPATH -split ';' | Where-Object { $_ -and (Test-Path -LiteralPath (Join-Path $_ 'profile.h')) } | Select-Object -First 1) } else { $null }
 $profileFile = if ($profileDir) { Join-Path $profileDir 'profile.h' } else { Join-Path $PSScriptRoot "profiles/$HardwareProfile/profile.h" }
 if (!(Test-Path -LiteralPath $profileFile)) { throw "No profile.h at $profileFile" }
 if (!$profileDir -and $HardwareProfile -eq 'TEMPLATE') {
  Write-Warning 'Using profiles/TEMPLATE/profile.h - every value in it is a placeholder, and it carries a build-time #error on purpose (see machine-profile.h). This run can only prove the build plumbing itself (flags, include paths, source list) is wired correctly; it cannot and must not produce working firmware. See PORTING-PLAN.md''s "two-stage architecture" for what a real profile actually needs.'
 }
 # The resolved profile (from $env:CPATH when set, else profiles/$HardwareProfile) is what actually got
 # compiled - log that, not the -HardwareProfile parameter's default, which is wrong/misleading whenever
 # CPATH is used (as every real build does): it would print "profile=TEMPLATE" even for a real machine.
 $resolvedProfileLabel = if ($profileDir) { Split-Path -Leaf $profileDir } else { $HardwareProfile }

 $out = if ($ExperimentalUnlock) { 'build/experimental' } else { 'build/preflight' }
 $name = if ($ExperimentalUnlock) { 'TURING2080TI-EXPERIMENTAL' } else { 'TURING2080TI-PREFLIGHT' }
 $defines = @()
 if ($ExperimentalUnlock) { $defines += '-DGU_EXPERIMENTAL_UNLOCK' }
 New-Item -ItemType Directory -Path $out -Force | Out-Null

 # Every source file that actually needs compiling, confirmed directly against PORTING-PLAN.md (not
 # assumed): the app-layer files ported from Ampere, the compat shim, and Turing's own six vendor .c
 # files (unlock.c and gfw_eval.c carry the GU_UEFI_PORT adaptation; boot.c/layout.c/prep.c/vbios.c are
 # pure freestanding C needing none - checked by grepping each for readl/writel/dma_*/pci_*/
 # request_firmware before relying on that). main.c is kept for provenance only and is never compiled.
 $sources = @('app', 'app_io', 'ownership', 'format', 'runtime', 'sha256', 'checks', 'port', 'compat',
  'vendor/unlock', 'vendor/boot', 'vendor/prep', 'vendor/layout', 'vendor/vbios', 'vendor/gfw_eval')
 $objects = @()
 foreach ($source in $sources) {
  $object = "$out/" + ($source -replace '/', '_') + '.obj'
  & $Clang -target x86_64-pc-windows-msvc -std=c11 -ffreestanding -fno-builtin -fno-stack-protector -mno-red-zone -fshort-wchar -Os -Wall -Wextra -Werror -Wno-unused-parameter -D__KERNEL__ -DGU_UEFI_PORT @defines -I compat -I ../../deps/edk2/MdePkg/Include -I ../../deps/edk2/MdePkg/Include/X64 -c "$source.c" -o $object
  if ($LASTEXITCODE -ne 0) { throw "Compile failed: $source" }
  $objects += $object
 }
 & $LldLink /nologo /nodefaultlib /subsystem:efi_application /entry:efi_main /machine:x64 /base:0 /dynamicbase /nxcompat /timestamp:0 /opt:ref /opt:icf "/map:$out/$name.map" "/out:$out/$name.EFI" @objects
 if ($LASTEXITCODE -ne 0) { throw 'EFI link failed' }
 & $Python verify_pe.py "$out/$name.EFI" "$out/$name.map" --output "$out/pe-verification.json"
 if ($LASTEXITCODE -ne 0) { throw 'PE verification failed' }

 [ordered]@{
  BuiltUtc         = [DateTime]::UtcNow.ToString('o')
  HardwareProfile  = $resolvedProfileLabel
  Mode             = if ($ExperimentalUnlock) { 'experimental-manual-unlock' } else { 'diagnostic-preflight-only' }
  CanArm           = [bool]$ExperimentalUnlock
  Defines          = $defines
  HardwareTested   = $false
  EfiSha256        = (Get-FileHash -LiteralPath "$out/$name.EFI" -Algorithm SHA256).Hash
  Sources          = @($sources | ForEach-Object { [ordered]@{ Path = "$_.c"; Sha256 = (Get-FileHash -LiteralPath "$_.c" -Algorithm SHA256).Hash } })
 } | ConvertTo-Json -Depth 5 | Set-Content -Encoding UTF8 -LiteralPath "$out/build-manifest.json"
 "Built $name.EFI; ExperimentalUnlock=$([bool]$ExperimentalUnlock); profile=$resolvedProfileLabel; hardware untested."
} finally { Pop-Location }
