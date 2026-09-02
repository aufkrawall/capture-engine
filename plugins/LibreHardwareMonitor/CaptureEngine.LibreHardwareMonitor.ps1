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
    [string]$CpuPackagePower = 'auto',

    [ValidatePattern('^(off|auto|/[A-Za-z0-9/_.-]{1,254})$')]
    [string]$GpuPackagePower = 'auto',

    [ValidatePattern('^(off|auto|/[A-Za-z0-9/_.-]{1,254})$')]
    [string]$GpuFan = 'auto',

    [ValidatePattern('^(off|auto|/[A-Za-z0-9/_.-]{1,254})$')]
    [string]$CpuCoreClock = 'auto',

    [ValidatePattern('^(off|auto|/[A-Za-z0-9/_.-]{1,254})$')]
    [string]$GpuCoreClock = 'auto',

    [ValidatePattern('^(off|auto|/[A-Za-z0-9/_.-]{1,254})$')]
    [string]$GpuMemoryClock = 'auto',

    [ValidatePattern('^(off|auto|/[A-Za-z0-9/_.-]{1,254})$')]
    [string]$GpuVoltage = 'auto'
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)

# Wire order of the CE_LHM_SAMPLE value/identifier pairs. New metrics append, so
# an older native reader can never misread a shifted field: it rejects the whole
# line on its field count instead. RejectZero separates a genuinely idle reading
# from a sensor the kernel driver could not fill in. A stopped fan really is
# 0 RPM, but a package reporting 0 W, 0 MHz, 0 V or 0 C is reporting nothing at
# all - without elevation LibreHardwareMonitor cannot open its driver and every
# CPU power and clock rail reads exactly zero.
$MetricDefinitions = @(
    @{ Key = 'CpuTemperature';  Type = 'Temperature'; Scope = 'Cpu'; RejectZero = $true;  Maximum = 250.0;
       PreferredNames = @('CPU Package', 'Core (Tctl/Tdie)', 'CPU (Tctl/Tdie)', 'CPU Die (average)') },
    @{ Key = 'GpuTemperature';  Type = 'Temperature'; Scope = 'Gpu'; RejectZero = $true;  Maximum = 250.0;
       PreferredNames = @('GPU Core') },
    @{ Key = 'CpuPackagePower'; Type = 'Power';       Scope = 'Cpu'; RejectZero = $true;  Maximum = 5000.0;
       PreferredNames = @('CPU Package', 'Package', 'CPU PPT') },
    @{ Key = 'GpuPackagePower'; Type = 'Power';       Scope = 'Gpu'; RejectZero = $true;  Maximum = 5000.0;
       PreferredNames = @('GPU Package', 'GPU Board', 'GPU Power') },
    @{ Key = 'GpuFan';          Type = 'Fan';         Scope = 'Gpu'; RejectZero = $false; Maximum = 100000.0;
       PreferredNames = @('GPU Fan') },
    @{ Key = 'CpuCoreClock';    Type = 'Clock';       Scope = 'Cpu'; RejectZero = $true;  Maximum = 20000.0;
       PreferredNames = @('Cores (Average)', 'CPU Core', 'Core') },
    @{ Key = 'GpuCoreClock';    Type = 'Clock';       Scope = 'Gpu'; RejectZero = $true;  Maximum = 20000.0;
       PreferredNames = @('GPU Core') },
    @{ Key = 'GpuMemoryClock';  Type = 'Clock';       Scope = 'Gpu'; RejectZero = $true;  Maximum = 20000.0;
       PreferredNames = @('GPU Memory') },
    @{ Key = 'GpuVoltage';      Type = 'Voltage';     Scope = 'Gpu'; RejectZero = $true;  Maximum = 10.0;
       PreferredNames = @('GPU Core Voltage', 'GPU Core') }
)

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

function Test-SensorUsable {
    param(
        [object]$Sensor,
        [bool]$RejectZero
    )

    $value = Get-SensorNumber -Sensor $Sensor
    if ($null -eq $value -or $value -lt 0) {
        return $false
    }
    return -not ($RejectZero -and $value -le 0)
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
        [string[]]$PreferredNames,
        [bool]$RejectZero,
        [string]$PreviousIdentifier
    )

    # 1. An exact preferred name wins, in preference order.
    foreach ($name in $PreferredNames) {
        foreach ($sensor in $Sensors) {
            if ($sensor.SensorType.ToString() -eq $SensorType -and
                [string]::Equals($sensor.Name, $name, [System.StringComparison]::OrdinalIgnoreCase) -and
                (Test-SensorUsable -Sensor $sensor -RejectZero $RejectZero)) {
                return $sensor
            }
        }
    }

    # 2. Multi-instance hardware numbers its sensors instead ("GPU Fan 1",
    #    "GPU Fan 2", "Core #1"), so no exact name matches. Take the lowest
    #    index: leaving this to the value comparison in step 4 would hand the
    #    selection to whichever instance reads highest in this one sample, so
    #    two idle fans a few RPM apart rename the selected identifier on nearly
    #    every poll and the reported RPM alternates between physical fans.
    foreach ($name in $PreferredNames) {
        $pattern = '^' + [regex]::Escape($name) + '\s*#?\s*(\d+)$'
        $selected = $null
        $selectedIndex = [int]::MaxValue
        foreach ($sensor in $Sensors) {
            if ($sensor.SensorType.ToString() -ne $SensorType -or
                -not (Test-SensorUsable -Sensor $sensor -RejectZero $RejectZero)) {
                continue
            }
            $match = [regex]::Match($sensor.Name, $pattern,
                [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
            if ($match.Success -and [int]$match.Groups[1].Value -lt $selectedIndex) {
                $selected = $sensor
                $selectedIndex = [int]$match.Groups[1].Value
            }
        }
        if ($null -ne $selected) {
            return $selected
        }
    }

    # 3. Keep an already-selected unrecognized sensor while it stays usable, so
    #    the last resort below cannot reselect a different one every sample.
    if (-not [string]::IsNullOrEmpty($PreviousIdentifier)) {
        foreach ($sensor in $Sensors) {
            if ($sensor.SensorType.ToString() -eq $SensorType -and
                [string]::Equals($sensor.Identifier.ToString(), $PreviousIdentifier,
                    [System.StringComparison]::OrdinalIgnoreCase) -and
                (Test-SensorUsable -Sensor $sensor -RejectZero $RejectZero)) {
                return $sensor
            }
        }
    }

    # 4. Nothing recognizable: the highest usable reading of the right type.
    $selected = $null
    $selectedValue = [double]::NegativeInfinity
    foreach ($sensor in $Sensors) {
        if ($sensor.SensorType.ToString() -ne $SensorType -or
            -not (Test-SensorUsable -Sensor $sensor -RejectZero $RejectZero)) {
            continue
        }
        $value = Get-SensorNumber -Sensor $sensor
        if ($value -gt $selectedValue) {
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
        [hashtable]$Metric,
        [string]$PreviousIdentifier
    )

    if ($Selector -eq 'off') {
        return $null
    }
    if ($Selector -ne 'auto') {
        return Find-ExactSensor -Sensors $ExactSensors -Selector $Selector -SensorType $Metric.Type
    }
    return Find-AutomaticSensor -Sensors $AutomaticSensors -SensorType $Metric.Type `
        -PreferredNames $Metric.PreferredNames -RejectZero $Metric.RejectZero `
        -PreviousIdentifier $PreviousIdentifier
}

function Convert-SensorPair {
    param(
        [object]$Sensor,
        [double]$Maximum,
        [bool]$RejectZero
    )

    if (-not (Test-SensorUsable -Sensor $Sensor -RejectZero $RejectZero)) {
        return @('-', '-')
    }
    $value = Get-SensorNumber -Sensor $Sensor
    if ($value -gt $Maximum) {
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

    $selectors = @{
        CpuTemperature  = $CpuTemperature
        GpuTemperature  = $GpuTemperature
        CpuPackagePower = $CpuPackagePower
        GpuPackagePower = $GpuPackagePower
        GpuFan          = $GpuFan
        CpuCoreClock    = $CpuCoreClock
        GpuCoreClock    = $GpuCoreClock
        GpuMemoryClock  = $GpuMemoryClock
        GpuVoltage      = $GpuVoltage
    }
    $previousIdentifiers = @{}
    $cpuRequested = $false
    $gpuRequested = $false
    foreach ($metric in $MetricDefinitions) {
        $previousIdentifiers[$metric.Key] = ''
        if ($selectors[$metric.Key] -eq 'off') {
            continue
        }
        if ($metric.Scope -eq 'Cpu') {
            $cpuRequested = $true
        }
        else {
            $gpuRequested = $true
        }
    }

    $computer = New-Object LibreHardwareMonitor.Hardware.Computer
    $computer.IsCpuEnabled = $cpuRequested
    $computer.IsGpuEnabled = $gpuRequested
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

        $sequence++
        $fields = New-Object System.Collections.Generic.List[string]
        $fields.Add('CE_LHM_SAMPLE')
        $fields.Add($sequence.ToString([System.Globalization.CultureInfo]::InvariantCulture))
        foreach ($metric in $MetricDefinitions) {
            $exactScope = if ($metric.Scope -eq 'Cpu') { $allCpuSensors } else { $allGpuSensors }
            $autoScope = if ($metric.Scope -eq 'Cpu') { $allCpuSensors } else { $activeGpuSensors }
            $sensor = Select-Sensor -Selector $selectors[$metric.Key] -ExactSensors $exactScope `
                -AutomaticSensors $autoScope -Metric $metric `
                -PreviousIdentifier $previousIdentifiers[$metric.Key]
            $pair = @(Convert-SensorPair -Sensor $sensor -Maximum $metric.Maximum -RejectZero $metric.RejectZero)
            $previousIdentifiers[$metric.Key] = if ($pair[1] -eq '-') { '' } else { $pair[1] }
            $fields.Add($pair[0])
            $fields.Add($pair[1])
        }
        [Console]::Out.WriteLine([string]::Join("`t", $fields.ToArray()))
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
