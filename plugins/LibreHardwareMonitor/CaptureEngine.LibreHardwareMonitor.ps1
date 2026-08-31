# SPDX-License-Identifier: MIT
# Copyright (c) 2026 aufkrawall

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^Local\\CE_LHM_Shutdown_[A-Fa-f0-9_]+$')]
    [string]$ShutdownEventName,

    [ValidateRange(250, 10000)]
    [int]$PollIntervalMs = 1000,

    [ValidatePattern('^(off|auto|/[A-Za-z0-9/_.-]{1,254})$')]
    [string]$CpuTemperature = 'auto',

    [ValidatePattern('^(off|auto|/[A-Za-z0-9/_.-]{1,254})$')]
    [string]$GpuTemperature = 'auto',

    [ValidatePattern('^(off|auto|/[A-Za-z0-9/_.-]{1,254})$')]
    [string]$CpuPackagePower = 'off',

    [ValidatePattern('^(off|auto|/[A-Za-z0-9/_.-]{1,254})$')]
    [string]$GpuPackagePower = 'off',

    [ValidatePattern('^(off|auto|/[A-Za-z0-9/_.-]{1,254})$')]
    [string]$GpuFan = 'off'
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)

function Update-HardwareTree {
    param([object]$Hardware)

    [void]$Hardware.Update()
    foreach ($child in @($Hardware.SubHardware)) {
        Update-HardwareTree -Hardware $child
    }
}

function Get-HardwareTreeSensors {
    param([object]$Hardware)

    $result = New-Object System.Collections.Generic.List[object]
    foreach ($sensor in @($Hardware.Sensors)) {
        $result.Add($sensor)
    }
    foreach ($child in @($Hardware.SubHardware)) {
        foreach ($sensor in @(Get-HardwareTreeSensors -Hardware $child)) {
            $result.Add($sensor)
        }
    }
    return $result.ToArray()
}

function Get-SensorNumber {
    param([object]$Sensor)

    if ($null -eq $Sensor -or $null -eq $Sensor.Value) {
        return $null
    }
    $number = [double]$Sensor.Value
    if ([double]::IsNaN($number) -or [double]::IsInfinity($number)) {
        return $null
    }
    return $number
}

function Find-ExactSensor {
    param(
        [object[]]$Sensors,
        [string]$Selector,
        [string]$SensorType
    )

    foreach ($sensor in $Sensors) {
        if ($sensor.SensorType.ToString() -eq $SensorType -and
            [string]::Equals($sensor.Identifier.ToString(), $Selector,
                [System.StringComparison]::OrdinalIgnoreCase)) {
            return $sensor
        }
    }
    return $null
}

function Find-AutomaticSensor {
    param(
        [object[]]$Sensors,
        [string]$SensorType,
        [string[]]$PreferredNames
    )

    foreach ($name in $PreferredNames) {
        foreach ($sensor in $Sensors) {
            if ($sensor.SensorType.ToString() -eq $SensorType -and
                [string]::Equals($sensor.Name, $name, [System.StringComparison]::OrdinalIgnoreCase) -and
                $null -ne (Get-SensorNumber -Sensor $sensor)) {
                $value = Get-SensorNumber -Sensor $sensor
                if ($SensorType -ne 'Temperature' -or $value -gt 0) {
                    return $sensor
                }
            }
        }
    }

    $selected = $null
    $selectedValue = [double]::NegativeInfinity
    foreach ($sensor in $Sensors) {
        if ($sensor.SensorType.ToString() -ne $SensorType) {
            continue
        }
        $value = Get-SensorNumber -Sensor $sensor
        if ($null -ne $value -and ($SensorType -ne 'Temperature' -or $value -gt 0) -and
            $value -gt $selectedValue) {
            $selected = $sensor
            $selectedValue = $value
        }
    }
    return $selected
}

function Get-ActiveGpuHardware {
    param(
        [object[]]$GpuHardware,
        [string]$PreviousIdentifier
    )

    $candidates = New-Object System.Collections.Generic.List[object]
    $highestLoad = [double]::NegativeInfinity
    foreach ($hardware in $GpuHardware) {
        $coreLoad = $null
        foreach ($sensor in @(Get-HardwareTreeSensors -Hardware $hardware)) {
            if ($sensor.SensorType.ToString() -eq 'Load' -and
                [string]::Equals($sensor.Name, 'GPU Core', [System.StringComparison]::OrdinalIgnoreCase)) {
                $value = Get-SensorNumber -Sensor $sensor
                if ($null -ne $value) {
                    $coreLoad = $value
                    break
                }
            }
        }
        if ($null -eq $coreLoad) {
            continue
        }
        if ($coreLoad -gt $highestLoad) {
            $candidates.Clear()
            $candidates.Add($hardware)
            $highestLoad = $coreLoad
        }
        elseif ($coreLoad -eq $highestLoad) {
            $candidates.Add($hardware)
        }
    }
    if ($candidates.Count -eq 1) {
        return $candidates[0]
    }
    if ($candidates.Count -gt 1 -and -not [string]::IsNullOrEmpty($PreviousIdentifier)) {
        foreach ($candidate in $candidates) {
            if ([string]::Equals($candidate.Identifier.ToString(), $PreviousIdentifier,
                    [System.StringComparison]::OrdinalIgnoreCase)) {
                return $candidate
            }
        }
    }
    if ($candidates.Count -eq 0 -and $GpuHardware.Count -eq 1) {
        return $GpuHardware[0]
    }
    return $null
}

function Select-Sensor {
    param(
        [string]$Selector,
        [object[]]$ExactSensors,
        [object[]]$AutomaticSensors,
        [string]$SensorType,
        [string[]]$PreferredNames
    )

    if ($Selector -eq 'off') {
        return $null
    }
    if ($Selector -ne 'auto') {
        return Find-ExactSensor -Sensors $ExactSensors -Selector $Selector -SensorType $SensorType
    }
    return Find-AutomaticSensor -Sensors $AutomaticSensors -SensorType $SensorType -PreferredNames $PreferredNames
}

function Convert-SensorPair {
    param(
        [object]$Sensor,
        [double]$Minimum,
        [double]$Maximum,
        [bool]$MinimumExclusive
    )

    $value = Get-SensorNumber -Sensor $Sensor
    if ($null -eq $value -or $value -gt $Maximum -or
        ($MinimumExclusive -and $value -le $Minimum) -or
        (-not $MinimumExclusive -and $value -lt $Minimum)) {
        return @('-', '-')
    }
    $identifier = $Sensor.Identifier.ToString()
    if ($identifier -notmatch '^/[A-Za-z0-9/_.-]{1,254}$') {
        return @('-', '-')
    }
    $formatted = ([single]$value).ToString('R', [System.Globalization.CultureInfo]::InvariantCulture)
    return @($formatted, $identifier)
}

$computer = $null
$shutdownEvent = $null
$activeGpuIdentifier = ''
try {
    $shutdownEvent = [System.Threading.EventWaitHandle]::OpenExisting($ShutdownEventName)
    $libraryPath = Join-Path $PSScriptRoot 'LibreHardwareMonitorLib.dll'
    $assembly = [System.Reflection.Assembly]::LoadFrom($libraryPath)

    $computer = New-Object LibreHardwareMonitor.Hardware.Computer
    $computer.IsCpuEnabled = ($CpuTemperature -ne 'off' -or $CpuPackagePower -ne 'off')
    $computer.IsGpuEnabled = ($GpuTemperature -ne 'off' -or $GpuPackagePower -ne 'off' -or $GpuFan -ne 'off')
    [void]$computer.Open()

    [Console]::Out.WriteLine("CE_LHM_READY`t{0}", $assembly.GetName().Version.ToString())
    [Console]::Out.Flush()

    [uint32]$sequence = 0
    while (-not $shutdownEvent.WaitOne(0)) {
        foreach ($hardware in @($computer.Hardware)) {
            Update-HardwareTree -Hardware $hardware
        }

        $cpuHardware = @($computer.Hardware | Where-Object { $_.HardwareType.ToString() -eq 'Cpu' })
        $gpuHardware = @($computer.Hardware | Where-Object { $_.HardwareType.ToString() -like 'Gpu*' })
        $allCpuSensors = @($cpuHardware | ForEach-Object { Get-HardwareTreeSensors -Hardware $_ })
        $allGpuSensors = @($gpuHardware | ForEach-Object { Get-HardwareTreeSensors -Hardware $_ })
        $activeGpu = Get-ActiveGpuHardware -GpuHardware $gpuHardware `
            -PreviousIdentifier $activeGpuIdentifier
        if ($null -ne $activeGpu) {
            $activeGpuIdentifier = $activeGpu.Identifier.ToString()
        }
        $activeGpuSensors = if ($null -eq $activeGpu) { @() } else { @(Get-HardwareTreeSensors -Hardware $activeGpu) }

        $cpuTempSensor = Select-Sensor -Selector $CpuTemperature -ExactSensors $allCpuSensors `
            -AutomaticSensors $allCpuSensors -SensorType 'Temperature' `
            -PreferredNames @('CPU Package', 'Core (Tctl/Tdie)', 'CPU (Tctl/Tdie)', 'CPU Die (average)')
        $gpuTempSensor = Select-Sensor -Selector $GpuTemperature -ExactSensors $allGpuSensors `
            -AutomaticSensors $activeGpuSensors -SensorType 'Temperature' -PreferredNames @('GPU Core')
        $cpuPowerSensor = Select-Sensor -Selector $CpuPackagePower -ExactSensors $allCpuSensors `
            -AutomaticSensors $allCpuSensors -SensorType 'Power' `
            -PreferredNames @('CPU Package', 'Package', 'CPU PPT')
        $gpuPowerSensor = Select-Sensor -Selector $GpuPackagePower -ExactSensors $allGpuSensors `
            -AutomaticSensors $activeGpuSensors -SensorType 'Power' `
            -PreferredNames @('GPU Package', 'GPU Board', 'GPU Power')
        $gpuFanSensor = Select-Sensor -Selector $GpuFan -ExactSensors $allGpuSensors `
            -AutomaticSensors $activeGpuSensors -SensorType 'Fan' -PreferredNames @('GPU Fan')

        $cpuTempPair = @(Convert-SensorPair -Sensor $cpuTempSensor -Minimum 0 -Maximum 250 -MinimumExclusive $true)
        $gpuTempPair = @(Convert-SensorPair -Sensor $gpuTempSensor -Minimum 0 -Maximum 250 -MinimumExclusive $true)
        $cpuPowerPair = @(Convert-SensorPair -Sensor $cpuPowerSensor -Minimum 0 -Maximum 5000 -MinimumExclusive $false)
        $gpuPowerPair = @(Convert-SensorPair -Sensor $gpuPowerSensor -Minimum 0 -Maximum 5000 -MinimumExclusive $false)
        $gpuFanPair = @(Convert-SensorPair -Sensor $gpuFanSensor -Minimum 0 -Maximum 100000 -MinimumExclusive $false)

        $sequence++
        $fields = @('CE_LHM_SAMPLE', $sequence.ToString([System.Globalization.CultureInfo]::InvariantCulture)) +
            $cpuTempPair + $gpuTempPair + $cpuPowerPair + $gpuPowerPair + $gpuFanPair
        [Console]::Out.WriteLine([string]::Join("`t", [string[]]$fields))
        [Console]::Out.Flush()

        if ($shutdownEvent.WaitOne($PollIntervalMs)) {
            break
        }
    }
}
catch {
    $failure = $_.Exception
    while ($null -ne $failure.InnerException) {
        $failure = $failure.InnerException
    }
    $kind = $failure.GetType().Name
    if ($kind -notmatch '^[A-Za-z0-9_.]{1,127}$') {
        $kind = 'BridgeFailure'
    }
    [Console]::Out.WriteLine("CE_LHM_ERROR`t{0}", $kind)
    [Console]::Out.Flush()
    exit 2
}
finally {
    if ($null -ne $computer) {
        try { [void]$computer.Close() } catch {}
    }
    if ($null -ne $shutdownEvent) {
        $shutdownEvent.Dispose()
    }
}
