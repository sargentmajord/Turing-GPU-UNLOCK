# GPL-2.0-only. Read-only hardware/firmware snapshot for anyone testing the capture tool.
# Changes nothing on the PC - every call here is a read (Get-*/Confirm-SecureBootUEFI/bcdedit /enum).
# Run: powershell -ExecutionPolicy Bypass -File system-report.ps1 | Tee-Object system-report.txt
# Then attach system-report.txt alongside the capture tool's DIAG0x.TXT log when reporting a result.
#
# Why this exists: the single most expensive thing in triaging a tester's report so far has been
# finding out - one GitHub comment at a time, over several days - what their motherboard/BIOS/PCIe
# layout actually looks like, after a failure already pointed at exactly that. This collects it all
# up front, once, instead.

function Get-SysBoard {
 $cs = Get-CimInstance Win32_ComputerSystem -ErrorAction SilentlyContinue
 $bb = Get-CimInstance Win32_BaseBoard -ErrorAction SilentlyContinue
 $bios = Get-CimInstance Win32_BIOS -ErrorAction SilentlyContinue
 $ch = Get-CimInstance Win32_SystemEnclosure -ErrorAction SilentlyContinue
 [ordered]@{
  'System manufacturer/model' = ('{0} / {1}' -f $cs.Manufacturer, $cs.Model)
  'Motherboard manufacturer/product' = ('{0} / {1}' -f $bb.Manufacturer, $bb.Product)
  'BIOS/UEFI version' = $bios.SMBIOSBIOSVersion
  'BIOS/UEFI release date' = $(if ($bios.ReleaseDate) { $bios.ReleaseDate.ToString('yyyy-MM-dd') } else { 'unknown' })
  'Chassis type(s)' = ($ch.ChassisTypes -join ',')
 }
}

function Get-SysCpu {
 $p = Get-CimInstance Win32_Processor -ErrorAction SilentlyContinue | Select-Object -First 1
 [ordered]@{ 'CPU' = $p.Name; 'Manufacturer' = $p.Manufacturer }
}

function Get-SysOsAndBoot {
 $o = Get-CimInstance Win32_OperatingSystem -ErrorAction SilentlyContinue
 # $env:firmware_type needs no elevation and is set by Windows itself; bcdedit /enum does need
 # elevation to open the BCD store, which this script deliberately doesn't require.
 $mode = if ($env:firmware_type) { $env:firmware_type } else { 'could not tell' }
 # Confirm-SecureBootUEFI needs SeSystemEnvironmentPrivilege, which a non-elevated process doesn't
 # hold even though this is only a read - so this one field is degraded (not missing data elsewhere)
 # unless the script happens to be run elevated.
 $sb = try { if (Confirm-SecureBootUEFI) { 'On' } else { 'Off' } } catch { 'could not tell without Run as Administrator' }
 [ordered]@{
  'Windows' = ('{0} (build {1}), {2}' -f $o.Caption, $o.BuildNumber, $(if ([Environment]::Is64BitOperatingSystem) { '64-bit' } else { '32-bit' }))
  'Boot mode' = $mode
  'Secure Boot' = $sb
 }
}

# Walks DEVPKEY_Device_Parent from a device up to the root, printing one line per hop - the thing
# a "Devices by connection" screenshot in Device Manager shows, but copy/paste-able and exact.
function Get-PciAncestry([string]$InstanceId) {
 $chain = @(); $cur = $InstanceId; $seen = @{}
 for ($i = 0; $i -lt 12 -and $cur -and -not $seen.ContainsKey($cur); $i++) {
  $seen[$cur] = $true
  $dev = Get-PnpDevice -InstanceId $cur -ErrorAction SilentlyContinue
  if (-not $dev) { break }
  $loc = (Get-PnpDeviceProperty -InstanceId $cur -KeyName 'DEVPKEY_Device_LocationInfo' -ErrorAction SilentlyContinue).Data
  $chain += [pscustomobject]@{ Depth = $i; Name = $dev.FriendlyName; InstanceId = $cur; Location = [string]$loc }
  $parent = (Get-PnpDeviceProperty -InstanceId $cur -KeyName 'DEVPKEY_Device_Parent' -ErrorAction SilentlyContinue).Data
  if (-not $parent -or $parent -eq $cur) { break }
  $cur = $parent
 }
 $chain
}

function Get-SysDisplayAdapters {
 @(Get-PnpDevice -Class Display -PresentOnly -ErrorAction SilentlyContinue | ForEach-Object {
  $ven = $null; $dev = $null; $sub = 'unknown'
  if ($_.InstanceId -match 'VEN_([0-9A-Fa-f]{4})&DEV_([0-9A-Fa-f]{4})') { $ven = $Matches[1].ToLower(); $dev = $Matches[2].ToLower() }
  if ($_.InstanceId -match 'SUBSYS_([0-9A-Fa-f]{4})([0-9A-Fa-f]{4})') { $sub = '{0}:{1}' -f $Matches[2].ToLower(), $Matches[1].ToLower() }
  $loc = (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName 'DEVPKEY_Device_LocationInfo' -ErrorAction SilentlyContinue).Data
  [pscustomobject]@{
   Name = $_.FriendlyName
   Id = $(if ($ven) { '{0}:{1}' -f $ven, $dev } else { 'unknown' })
   Subsystem = $sub
   Location = [string]$loc
   InstanceId = $_.InstanceId
   Ancestry = Get-PciAncestry $_.InstanceId
  }
 })
}

Write-Host '=== System / firmware report (read-only; nothing here changes your PC) ===' -ForegroundColor Cyan
Write-Host ''
Write-Host '--- Board and firmware ---'
(Get-SysBoard).GetEnumerator() | ForEach-Object { Write-Host (' {0}: {1}' -f $_.Key, $_.Value) }
(Get-SysCpu).GetEnumerator() | ForEach-Object { Write-Host (' {0}: {1}' -f $_.Key, $_.Value) }
Write-Host ''
Write-Host '--- Windows / boot ---'
(Get-SysOsAndBoot).GetEnumerator() | ForEach-Object { Write-Host (' {0}: {1}' -f $_.Key, $_.Value) }
Write-Host ''
Write-Host '--- Display adapters, with PCI identity and the firmware device-tree chain up to the root ---'
Write-Host '(this is the exact thing a "Devices by connection" screenshot shows - whether a card sits'
Write-Host ' directly under a CPU-connected root port, or behind an extra bridge/switch)'
$adapters = Get-SysDisplayAdapters
if (-not $adapters) { Write-Host ' (none found - Get-PnpDevice returned nothing for Class Display)' }
foreach ($a in $adapters) {
 Write-Host ''
 Write-Host (' {0}  [{1}, subsystem {2}]  {3}' -f $a.Name, $a.Id, $a.Subsystem, $a.Location)
 foreach ($node in $a.Ancestry) {
  Write-Host ('   {0}{1}  ({2})' -f ('  ' * $node.Depth), $node.Name, $node.InstanceId)
 }
}
Write-Host ''
Write-Host '=== End of report ===' -ForegroundColor Cyan
