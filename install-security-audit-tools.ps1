<#
.SYNOPSIS
  Installs or detects Windows security-audit/debugging tools referenced by debug-tools-security-audit.md.

.DESCRIPTION
  Conservative by default:
  - Downloads small portable tools from official/safe sources where practical.
  - Verifies Authenticode signatures for Microsoft/Sysinternals downloads.
  - Does NOT install large packages such as Visual Studio Build Tools, Windows SDK, or LLVM by default.
  - Detects large/existing toolchains and prints warnings when tools are unavailable.
  - Installs portable, low-side-effect scanners by default. Python/pip-based scanners are opt-in because they can mutate user Python environments and PATH assumptions.
  - Generates a manifest and warnings file for audit-report evidence.

  Intended repo location:
    install-security-audit-tools.ps1

.PARAMETER InstallRoot
  Directory where portable tools and manifest files are placed.

.PARAMETER DebugToolsMdPath
  Path to debug-tools-security-audit.md or debug-tools.md. Used for evidence/logging.
  The script does not execute arbitrary commands from this file.


.PARAMETER ProjectRoot
  Repository root used to decide which SAST/secrets/dependency tools are applicable.



.PARAMETER Full
  Install as much supported tooling as practical. This enables default portable installs plus optional/heavier installs:
  GUI Sysinternals, WinDbg, LLVM, FFmpeg, trufflehog, CodeQL, and Python/pip-based SAST tools.

.PARAMETER Uninstall
  Remove script-managed portable tools and evidence under InstallRoot, then exit.

.PARAMETER RemoveSharedPackages
  With -Uninstall, also attempt to remove package-manager/shared installs the script can create, such as WinDbg and LLVM.
  This is not enabled by default because those packages may have existed before this script was run.

.PARAMETER RemovePythonPackages
  With -Uninstall, also attempt to uninstall Python/pipx packages the script can install, such as semgrep, flawfinder, and pip-audit.
  This is not enabled by default because those packages may be used outside this audit tooling.

.PARAMETER Minimal
  Conservative mode. Do not install default portable scanners; detect only unless explicit Include* switches are provided.

.PARAMETER SkipSastInstall
  Skip default SAST installation attempts. Currently Python/pip-based SAST is not installed by default, so this mainly affects future portable SAST defaults.

.PARAMETER SkipSecretsInstall
  Skip default gitleaks installation.

.PARAMETER SkipDependencyScannerInstall
  Skip default osv-scanner and pip-audit installation attempts.

.PARAMETER IncludeSast
  Explicitly install semgrep and flawfinder where possible. These use pipx or Python user installs and are opt-in.

.PARAMETER IncludePythonSast
  Alias-style group switch for Python/pip-based SAST/dependency tools: semgrep, flawfinder, and pip-audit.

.PARAMETER IncludeSecrets
  Explicitly install secrets scanners. gitleaks is already installed by default unless -Minimal or -SkipSecretsInstall is used; trufflehog remains opt-in because it is heavier/noisier.

.PARAMETER IncludeDependencyScanners
  Explicitly install dependency scanners. osv-scanner is installed by default unless -Minimal or -SkipDependencyScannerInstall is used; pip-audit is Python/pip-based and remains opt-in unless requested.

.PARAMETER IncludeSemgrep
  Install semgrep with pipx or Python user install where possible.

.PARAMETER IncludeFlawfinder
  Install flawfinder with pipx or Python user install where possible.

.PARAMETER IncludeGitleaks
  Download a portable gitleaks release from the official gitleaks GitHub repository.

.PARAMETER IncludeTruffleHog
  Download a portable trufflehog release from the official trufflesecurity/trufflehog GitHub repository.

.PARAMETER IncludeOSVScanner
  Download a portable osv-scanner release from the official google/osv-scanner GitHub repository.

.PARAMETER IncludePipAudit
  Install pip-audit with pipx or Python user install where possible.

.PARAMETER IncludeCodeQL
  Download the CodeQL bundle from the official GitHub CodeQL release. This is large and opt-in.

.PARAMETER RequireTools
  Tool names that must be available for the intended audit. Use with -StrictRequiredTools to fail if missing.

.PARAMETER StrictRequiredTools
  Exit with code 3 after writing evidence if any tool listed in -RequireTools is missing.

.PARAMETER SkipSysinternals
  Skip downloading small portable Microsoft Sysinternals CLI tools.

.PARAMETER IncludeGuiSysinternals
  Also download GUI Sysinternals tools such as Procmon and Process Explorer.

.PARAMETER IncludeWinDbg
  Install WinDbg Preview using winget package Microsoft.WinDbg.
  This is official Microsoft tooling, but it is not portable.

.PARAMETER IncludeFFmpeg
  Download and extract ffmpeg-release-essentials.zip from gyan.dev.
  FFmpeg upstream provides source only and links to third-party Windows builds; this is opt-in.

.PARAMETER IncludeLLVMViaWinget
  Install LLVM using winget package LLVM.LLVM.
  This is larger and not portable, so it is opt-in.

.PARAMETER SkipVSWhere
  Skip downloading portable vswhere.exe from the official microsoft/vswhere GitHub release.

.PARAMETER AddToUserPath
  Add installed portable tool directories to the current user's PATH.

.PARAMETER WhatIfOnly
  Print planned actions and detection results without downloading or installing.

.EXAMPLE
  .\install-security-audit-tools.ps1

.EXAMPLE
  .\install-security-audit-tools.ps1 -IncludeGuiSysinternals -IncludeWinDbg -AddToUserPath

.EXAMPLE
  .\install-security-audit-tools.ps1 -IncludeFFmpeg -IncludeLLVMViaWinget

.NOTES
  Safe-source policy:
  - Microsoft Sysinternals tools: https://live.sysinternals.com/
  - WinDbg Preview: winget package Microsoft.WinDbg
  - vswhere: official microsoft/vswhere GitHub release
  - FFmpeg Windows build: gyan.dev, opt-in third-party build linked from FFmpeg download resources
  - LLVM: winget package LLVM.LLVM, opt-in because it is relatively large

  This script intentionally does not install Windows SDK Debugging Tools, Visual Studio Build Tools,
  or MSVC by default because they are large. It detects them and reports missing coverage.
#>

[CmdletBinding()]
param(
  [string]$InstallRoot = "$env:LOCALAPPDATA\SecurityAuditTools",
  [string]$DebugToolsMdPath = ".\llm-wiki\debug-tools-security-audit.md",
  [switch]$SkipSysinternals,
  [switch]$IncludeGuiSysinternals,
  [switch]$IncludeWinDbg,
  [switch]$IncludeFFmpeg,
  [switch]$IncludeLLVMViaWinget,
  [switch]$SkipVSWhere,
  [string]$ProjectRoot = ".",
  [switch]$Full,
  [switch]$Uninstall,
  [switch]$RemoveSharedPackages,
  [switch]$RemovePythonPackages,
  [switch]$Minimal,
  [switch]$SkipSastInstall,
  [switch]$SkipSecretsInstall,
  [switch]$SkipDependencyScannerInstall,
  [switch]$IncludeSast,
  [switch]$IncludePythonSast,
  [switch]$IncludeSecrets,
  [switch]$IncludeDependencyScanners,
  [switch]$IncludeSemgrep,
  [switch]$IncludeFlawfinder,
  [switch]$IncludeGitleaks,
  [switch]$IncludeTruffleHog,
  [switch]$IncludeOSVScanner,
  [switch]$IncludePipAudit,
  [switch]$IncludeCodeQL,
  [string[]]$RequireTools = @(),
  [switch]$StrictRequiredTools,
  [switch]$AddToUserPath,
  [switch]$WhatIfOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

try {
  [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 -bor [Net.SecurityProtocolType]::Tls13
} catch {
  [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
}

$InstallRoot = [IO.Path]::GetFullPath($InstallRoot)
$ProjectRoot = [IO.Path]::GetFullPath($ProjectRoot)
$BinRoot = Join-Path $InstallRoot "bin"
$SysinternalsDir = Join-Path $BinRoot "sysinternals"
$VSWhereDir = Join-Path $BinRoot "vswhere"
$FFmpegDir = Join-Path $BinRoot "ffmpeg"
$SastDir = Join-Path $BinRoot "sast"
$LogDir = Join-Path $InstallRoot "logs"
$ManifestPath = Join-Path $InstallRoot "security-audit-tool-manifest.json"
$WarningsPath = Join-Path $InstallRoot "security-audit-tool-warnings.txt"
$MarkdownPath = Join-Path $InstallRoot "security-audit-tool-availability.md"

$script:Results = New-Object System.Collections.Generic.List[object]
$script:Warnings = New-Object System.Collections.Generic.List[string]
$script:RequiredToolMissing = $false


# Mode normalization.
if ($Full -and $Minimal) {
  throw "Use either -Full or -Minimal, not both."
}

if ($Full) {
  $IncludeGuiSysinternals = $true
  $IncludeWinDbg = $true
  $IncludeFFmpeg = $true
  $IncludeLLVMViaWinget = $true
  $IncludeSecrets = $true
  $IncludeDependencyScanners = $true
  $IncludePythonSast = $true
  $IncludeTruffleHog = $true
  $IncludeCodeQL = $true
}


function Add-WarningMessage {
  param([string]$Message)
  $script:Warnings.Add($Message) | Out-Null
  Write-Warning $Message
}

function Add-Result {
  param(
    [string]$Name,
    [string]$Category,
    [string]$Status,
    [string]$Path = "",
    [string]$Source = "",
    [string]$SignatureStatus = "",
    [string]$Notes = ""
  )
  $script:Results.Add([pscustomobject]@{
    name = $Name
    category = $Category
    status = $Status
    path = $Path
    source = $Source
    signature_status = $SignatureStatus
    notes = $Notes
  }) | Out-Null
}

function Ensure-Dir {
  param([string]$Path)
  if (-not (Test-Path -LiteralPath $Path)) {
    if ($WhatIfOnly) {
      Write-Host "Would create directory: $Path"
    } else {
      New-Item -ItemType Directory -Path $Path -Force | Out-Null
    }
  }
}

function Test-CommandPath {
  param([string]$Command)
  $cmd = Get-Command $Command -ErrorAction SilentlyContinue
  if ($null -ne $cmd) { return $cmd.Source }
  return $null
}


function Test-LocalToolPath {
  param([string]$Command)

  $cmdPath = Test-CommandPath $Command
  if ($cmdPath) { return $cmdPath }

  $name = $Command
  if ($name -notmatch '\.exe$') { $nameExe = "$name.exe" } else { $nameExe = $name }

  $roots = New-Object System.Collections.Generic.List[string]

  foreach ($root in @($SastDir, $SysinternalsDir, $VSWhereDir, $FFmpegDir, $BinRoot)) {
    if ($root) { $roots.Add($root) | Out-Null }
  }

  if ($env:APPDATA) {
    $pythonRoot = Join-Path $env:APPDATA "Python"
    if (Test-Path -LiteralPath $pythonRoot) { $roots.Add($pythonRoot) | Out-Null }
  }

  if ($env:USERPROFILE) {
    foreach ($candidate in @(
      (Join-Path $env:USERPROFILE ".local\bin"),
      (Join-Path $env:USERPROFILE ".local\pipx"),
      (Join-Path $env:USERPROFILE "pipx")
    )) {
      if (Test-Path -LiteralPath $candidate) { $roots.Add($candidate) | Out-Null }
    }
  }

  foreach ($candidate in @(
    "${env:ProgramFiles}\LLVM\bin",
    "${env:ProgramFiles(x86)}\LLVM\bin",
    "${env:LOCALAPPDATA}\Programs\LLVM\bin"
  )) {
    if ($candidate -and (Test-Path -LiteralPath $candidate)) {
      $roots.Add($candidate) | Out-Null
    }
  }

  foreach ($root in ($roots | Select-Object -Unique)) {
    if (-not $root -or -not (Test-Path -LiteralPath $root)) { continue }

    # Direct candidate first.
    $direct = Join-Path $root $nameExe
    if (Test-Path -LiteralPath $direct) { return $direct }

    # Recursive search for managed roots and known script/tool roots.
    $match = Get-ChildItem -LiteralPath $root -Recurse -File -ErrorAction SilentlyContinue |
      Where-Object { $_.Name -ieq $name -or $_.Name -ieq $nameExe } |
      Select-Object -First 1
    if ($match) { return $match.FullName }
  }

  return $null
}


function Add-ToolAvailability {
  param(
    [string]$Name,
    [string]$Category,
    [string]$Command = "",
    [bool]$Applicable = $true,
    [string]$MissingWarning = ""
  )

  if (-not $Command) { $Command = $Name }

  $path = Test-LocalToolPath $Command
  if ($path) {
    Add-Result -Name $Name -Category $Category -Status "available" -Path $path -Notes "Path resolved by PATH or local install roots"
    return $true
  }

  $status = "missing"
  if (-not $Applicable) { $status = "missing-not-applicable" }

  Add-Result -Name $Name -Category $Category -Status $status -Notes $MissingWarning
  if ($Applicable -and $MissingWarning) {
    Add-WarningMessage $MissingWarning
  }
  return $false
}

function Test-AnyProjectFile {
  param(
    [string]$Root,
    [string[]]$Names = @(),
    [string[]]$Extensions = @(),
    [switch]$Recurse
  )

  foreach ($name in $Names) {
    if (Test-Path -LiteralPath (Join-Path $Root $name)) { return $true }
  }

  if ($Extensions.Count -gt 0) {
    try {
      if ($Recurse) {
        $found = Get-ChildItem -LiteralPath $Root -Recurse -File -ErrorAction SilentlyContinue |
          Where-Object { $Extensions -contains $_.Extension.ToLowerInvariant() } |
          Select-Object -First 1
      } else {
        $found = Get-ChildItem -LiteralPath $Root -File -ErrorAction SilentlyContinue |
          Where-Object { $Extensions -contains $_.Extension.ToLowerInvariant() } |
          Select-Object -First 1
      }
      if ($found) { return $true }
    } catch {
      Add-WarningMessage ("Project file scan failed under {0}: {1}" -f $Root, $_.Exception.Message)
    }
  }

  return $false
}

function Get-ProjectSignals {
  param([string]$Root)

  $hasGit = Test-Path -LiteralPath (Join-Path $Root ".git")
  $hasCpp = Test-AnyProjectFile -Root $Root -Extensions @(".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".hh", ".hxx") -Recurse
  $hasRust = Test-AnyProjectFile -Root $Root -Names @("Cargo.toml", "Cargo.lock")
  $hasNode = Test-AnyProjectFile -Root $Root -Names @("package.json", "package-lock.json", "pnpm-lock.yaml", "yarn.lock")
  $hasPython = Test-AnyProjectFile -Root $Root -Names @("requirements.txt", "pyproject.toml", "poetry.lock", "Pipfile.lock", "setup.py")
  $hasGo = Test-AnyProjectFile -Root $Root -Names @("go.mod", "go.sum")
  $hasJvm = Test-AnyProjectFile -Root $Root -Names @("pom.xml", "build.gradle", "build.gradle.kts", "gradle.lockfile")
  $hasLockfiles = Test-AnyProjectFile -Root $Root -Names @(
    "Cargo.lock", "package-lock.json", "pnpm-lock.yaml", "yarn.lock", "poetry.lock",
    "Pipfile.lock", "requirements.txt", "go.sum", "pom.xml", "build.gradle", "build.gradle.kts",
    "gradle.lockfile"
  )

  $hasSource = $hasCpp -or $hasRust -or $hasNode -or $hasPython -or $hasGo -or $hasJvm -or
    (Test-AnyProjectFile -Root $Root -Extensions @(".js", ".ts", ".jsx", ".tsx", ".py", ".rs", ".go", ".java", ".kt", ".cs", ".php", ".rb", ".swift") -Recurse)

  return [pscustomobject]@{
    has_git = $hasGit
    has_c_cpp = $hasCpp
    has_rust = $hasRust
    has_node = $hasNode
    has_python = $hasPython
    has_go = $hasGo
    has_jvm = $hasJvm
    has_lockfiles = $hasLockfiles
    has_source = $hasSource
  }
}

function Invoke-GitHubLatestAssetDownload {
  param(
    [string]$Name,
    [string]$Repo,
    [string]$AssetRegex,
    [string]$ExpectedExe
  )

  Ensure-Dir $SastDir
  $api = "https://api.github.com/repos/$Repo/releases/latest"

  try {
    if ($WhatIfOnly) {
      Write-Host "Would query $api and download asset matching $AssetRegex for $Name"
      Add-Result -Name $Name -Category "SAST/secrets/dependency install" -Status "planned" -Source $api -Notes "WhatIfOnly"
      return
    }

    Write-Host "Resolving latest $Name release from $api"
    $headers = @{ "User-Agent" = "security-audit-tools-installer" }
    $release = Invoke-RestMethod -Uri $api -Headers $headers
    $asset = $release.assets | Where-Object { $_.name -match $AssetRegex } | Select-Object -First 1

    if (-not $asset) {
      Add-WarningMessage "Could not find a $Name release asset matching '$AssetRegex' in $Repo latest release."
      Add-Result -Name $Name -Category "SAST/secrets/dependency install" -Status "asset-not-found" -Source $api -Notes $AssetRegex
      return
    }

    $downloadUrl = $asset.browser_download_url
    $outFile = Join-Path $SastDir $asset.name
    Invoke-SafeDownload -Name $asset.name -Uri $downloadUrl -OutFile $outFile
    Write-Host "Resolving executable for $Name from $outFile"

    if (-not (Test-Path -LiteralPath $outFile)) {
      Add-WarningMessage "$Name download did not produce $outFile"
      return
    }

    if ($outFile -match '\.zip$') {
      $extractDir = Join-Path $SastDir ([IO.Path]::GetFileNameWithoutExtension($asset.name))
      Ensure-Dir $extractDir
      Expand-Archive -LiteralPath $outFile -DestinationPath $extractDir -Force
    } elseif ($outFile -match '\.tar\.gz$') {
      $extractDir = Join-Path $SastDir ($asset.name -replace '\.tar\.gz$', '')
      Ensure-Dir $extractDir
      $tar = Test-CommandPath "tar.exe"
      if (-not $tar) { $tar = Test-CommandPath "tar" }
      if ($tar) {
        & $tar -xzf $outFile -C $extractDir
      } else {
        Add-WarningMessage "tar was unavailable; could not extract $outFile"
      }
    } elseif ($outFile -match '\.exe$') {
      $extractDir = $SastDir
      $targetExe = Join-Path $SastDir $ExpectedExe
      if ((Split-Path -Leaf $outFile) -ne $ExpectedExe) {
        Copy-Item -LiteralPath $outFile -Destination $targetExe -Force
      }
    } else {
      $extractDir = $SastDir
    }

    $resolved = Test-LocalToolPath $ExpectedExe
    if ($resolved) {
      $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $resolved).Hash
      Add-Result -Name $Name -Category "SAST/secrets/dependency install" -Status "installed-or-resolved" -Path $resolved -Source $downloadUrl -Notes "SHA256=$hash; GitHub release asset; executable resolved after download/extract/copy; Authenticode may be unavailable"
    } else {
      Add-WarningMessage "$Name was downloaded but $ExpectedExe could not be resolved after extraction."
      Add-Result -Name $Name -Category "SAST/secrets/dependency install" -Status "installed-unresolved" -Source $downloadUrl
    }
  } catch {
    Add-WarningMessage "$Name install failed: $($_.Exception.Message)"
    Add-Result -Name $Name -Category "SAST/secrets/dependency install" -Status "install-failed" -Source $api -Notes $_.Exception.Message
  }
}

function Install-PythonUserTool {
  param(
    [string]$Command,
    [string]$PackageName
  )

  if (Test-LocalToolPath $Command) { return }

  if ($WhatIfOnly) {
    Write-Host "Would install $PackageName using pipx or Python user install"
    Add-Result -Name $Command -Category "Python-based SAST/dependency install" -Status "planned" -Source $PackageName -Notes "WhatIfOnly"
    return
  }

  $pipx = Test-CommandPath "pipx.exe"
  if (-not $pipx) { $pipx = Test-CommandPath "pipx" }

  if ($pipx) {
    Write-Host "Installing $PackageName with pipx"
    & $pipx install $PackageName
    if ($LASTEXITCODE -eq 0) {
      $resolvedAfterPipx = Test-LocalToolPath $Command
      if ($resolvedAfterPipx) {
        Add-Result -Name $Command -Category "Python-based SAST/dependency install" -Status "installed-or-present" -Path $resolvedAfterPipx -Source "pipx:$PackageName"
      } else {
        Add-Result -Name $Command -Category "Python-based SAST/dependency install" -Status "installed-unresolved" -Source "pipx:$PackageName" -Notes "Tool may be outside PATH; inspect Python/pipx script directories"
      }
      return
    }
    Add-WarningMessage "pipx install failed for $PackageName with exit code $LASTEXITCODE"
  }

  $py = Test-CommandPath "py.exe"
  if ($py) {
    Write-Host "Installing $PackageName with py -m pip --user"
    & $py -m pip install --user $PackageName
    if ($LASTEXITCODE -eq 0) {
      $resolvedAfterPy = Test-LocalToolPath $Command
      if ($resolvedAfterPy) {
        Add-Result -Name $Command -Category "Python-based SAST/dependency install" -Status "installed-or-present" -Path $resolvedAfterPy -Source "py -m pip:$PackageName"
      } else {
        Add-Result -Name $Command -Category "Python-based SAST/dependency install" -Status "installed-unresolved" -Source "py -m pip:$PackageName" -Notes "Tool may be in %APPDATA%\Python\Python*\Scripts and not on PATH"
      }
      return
    }
    Add-WarningMessage "py -m pip install --user failed for $PackageName with exit code $LASTEXITCODE"
  }

  $python = Test-CommandPath "python.exe"
  if (-not $python) { $python = Test-CommandPath "python" }
  if ($python) {
    Write-Host "Installing $PackageName with python -m pip --user"
    & $python -m pip install --user $PackageName
    if ($LASTEXITCODE -eq 0) {
      $resolvedAfterPython = Test-LocalToolPath $Command
      if ($resolvedAfterPython) {
        Add-Result -Name $Command -Category "Python-based SAST/dependency install" -Status "installed-or-present" -Path $resolvedAfterPython -Source "python -m pip:$PackageName"
      } else {
        Add-Result -Name $Command -Category "Python-based SAST/dependency install" -Status "installed-unresolved" -Source "python -m pip:$PackageName" -Notes "Tool may be in %APPDATA%\Python\Python*\Scripts and not on PATH"
      }
      return
    }
    Add-WarningMessage "python -m pip install --user failed for $PackageName with exit code $LASTEXITCODE"
  } else {
    Add-WarningMessage "No Python launcher was found; cannot install $PackageName"
  }
}

function Install-OptionalSastTools {
  $defaultPortableSecrets = (-not $Minimal) -and (-not $SkipSecretsInstall)
  $defaultPortableDependency = (-not $Minimal) -and (-not $SkipDependencyScannerInstall)

  # Deliberately false by default: semgrep/flawfinder/pip-audit use pipx/Python user installs,
  # can change user Python package state, can be slow/noisy, and may install scripts outside PATH.
  $defaultPythonSast = $false

  $installSemgrep = [bool]$IncludeSemgrep -or [bool]$IncludeSast -or [bool]$IncludePythonSast -or (($defaultPythonSast) -and (-not $SkipSastInstall))
  $installFlawfinder = [bool]$IncludeFlawfinder -or [bool]$IncludeSast -or [bool]$IncludePythonSast -or (($defaultPythonSast) -and (-not $SkipSastInstall))
  $installGitleaks = [bool]$IncludeGitleaks -or [bool]$IncludeSecrets -or $defaultPortableSecrets
  $installTruffleHog = [bool]$IncludeTruffleHog
  $installOSVScanner = [bool]$IncludeOSVScanner -or [bool]$IncludeDependencyScanners -or $defaultPortableDependency
  $installPipAudit = [bool]$IncludePipAudit -or [bool]$IncludePythonSast -or ([bool]$IncludeDependencyScanners -and [bool]$IncludePipAudit)

  if ($Minimal) {
    Add-WarningMessage "Minimal mode is enabled. Default portable scanner installation is disabled; detection still runs."
  }
  if ($SkipSecretsInstall) {
    Add-WarningMessage "Default gitleaks installation was skipped by -SkipSecretsInstall."
  }
  if ($SkipDependencyScannerInstall) {
    Add-WarningMessage "Default osv-scanner installation was skipped by -SkipDependencyScannerInstall."
  }
  if (-not $installSemgrep -and -not $installFlawfinder -and -not $installPipAudit) {
    Add-WarningMessage "Python/pip-based tools semgrep, flawfinder, and pip-audit are not installed by default. Use -IncludePythonSast, -IncludeSast, -IncludeSemgrep, -IncludeFlawfinder, or -IncludePipAudit if you accept user-Python environment changes."
  }

  if ($installGitleaks) {
    Invoke-GitHubLatestAssetDownload -Name "gitleaks" -Repo "gitleaks/gitleaks" -AssetRegex "windows.*(x64|amd64).*\.zip$" -ExpectedExe "gitleaks.exe"
  }
  if ($installTruffleHog) {
    Invoke-GitHubLatestAssetDownload -Name "trufflehog" -Repo "trufflesecurity/trufflehog" -AssetRegex "windows.*(x64|amd64).*(\.zip|\.tar\.gz)$" -ExpectedExe "trufflehog.exe"
  } else {
    Add-WarningMessage "trufflehog was not installed by default because it is heavier/noisier. Use -IncludeTruffleHog if deeper secrets scanning is needed."
  }
  if ($installOSVScanner) {
    Invoke-GitHubLatestAssetDownload -Name "osv-scanner" -Repo "google/osv-scanner" -AssetRegex "windows.*(x64|amd64).*(\.zip|\.exe)$" -ExpectedExe "osv-scanner.exe"
  }
  if ($IncludeCodeQL) {
    Invoke-GitHubLatestAssetDownload -Name "CodeQL" -Repo "github/codeql-action" -AssetRegex "codeql-bundle-win64\.tar\.gz$" -ExpectedExe "codeql.exe"
    Add-WarningMessage "CodeQL bundle is large and was installed only because -IncludeCodeQL was requested."
  } else {
    Add-WarningMessage "CodeQL was not installed by default because it is large. Use -IncludeCodeQL if deep data-flow analysis is required."
  }
  if ($installSemgrep) {
    Add-WarningMessage "semgrep installation uses pipx or Python user install. Prefer pipx; verify local support and results before relying on it."
    Install-PythonUserTool -Command "semgrep.exe" -PackageName "semgrep"
  }
  if ($installFlawfinder) {
    Install-PythonUserTool -Command "flawfinder.exe" -PackageName "flawfinder"
  }
  if ($installPipAudit) {
    Install-PythonUserTool -Command "pip-audit.exe" -PackageName "pip-audit"
  }
}

function Detect-SastSecretsDependencyTools {
  param([object]$Signals)

  Add-Result -Name "project signals" -Category "project detection" -Status "detected" -Path $ProjectRoot -Notes ("git={0}; source={1}; c_cpp={2}; rust={3}; node={4}; python={5}; go={6}; jvm={7}; lockfiles={8}" -f $Signals.has_git, $Signals.has_source, $Signals.has_c_cpp, $Signals.has_rust, $Signals.has_node, $Signals.has_python, $Signals.has_go, $Signals.has_jvm, $Signals.has_lockfiles)

  [void](Add-ToolAvailability -Name "semgrep" -Category "SAST" -Command "semgrep.exe" -Applicable $Signals.has_source -MissingWarning "semgrep was applicable but unavailable; source-level SAST confidence may be reduced.")
  [void](Add-ToolAvailability -Name "flawfinder" -Category "SAST C/C++" -Command "flawfinder.exe" -Applicable $Signals.has_c_cpp -MissingWarning "flawfinder was applicable for C/C++ but unavailable; risky API scan confidence may be reduced.")
  [void](Add-ToolAvailability -Name "CodeQL" -Category "SAST/data-flow" -Command "codeql.exe" -Applicable $Signals.has_source -MissingWarning "CodeQL was applicable but unavailable; deep data-flow SAST coverage may be reduced.")

  [void](Add-ToolAvailability -Name "gitleaks" -Category "secrets" -Command "gitleaks.exe" -Applicable $Signals.has_git -MissingWarning "gitleaks was applicable because .git exists but unavailable; Git history secrets-scanning confidence may be reduced.")
  [void](Add-ToolAvailability -Name "trufflehog" -Category "secrets" -Command "trufflehog.exe" -Applicable $Signals.has_git -MissingWarning "trufflehog was unavailable; deeper secrets scanning is unavailable.")

  [void](Add-ToolAvailability -Name "osv-scanner" -Category "dependency" -Command "osv-scanner.exe" -Applicable $Signals.has_lockfiles -MissingWarning "osv-scanner was applicable because dependency manifests/lockfiles were detected but unavailable; dependency vulnerability confidence may be reduced.")
  [void](Add-ToolAvailability -Name "pip-audit" -Category "dependency Python" -Command "pip-audit.exe" -Applicable $Signals.has_python -MissingWarning "pip-audit was applicable for Python dependency files but unavailable.")
  [void](Add-ToolAvailability -Name "cargo-audit" -Category "dependency Rust" -Command "cargo-audit.exe" -Applicable $Signals.has_rust -MissingWarning "cargo-audit was applicable for Rust but unavailable.")
  [void](Add-ToolAvailability -Name "npm" -Category "dependency Node" -Command "npm.cmd" -Applicable $Signals.has_node -MissingWarning "npm was applicable for Node dependency audit but unavailable.")
  [void](Add-ToolAvailability -Name "govulncheck" -Category "dependency Go" -Command "govulncheck.exe" -Applicable $Signals.has_go -MissingWarning "govulncheck was applicable for Go but unavailable.")
}

function Test-RequiredToolGate {
  if ($RequireTools.Count -eq 0) { return }

  foreach ($tool in $RequireTools) {
    $resolved = Test-LocalToolPath $tool
    if ($resolved) {
      Add-Result -Name $tool -Category "required tool gate" -Status "available" -Path $resolved
    } else {
      $script:RequiredToolMissing = $true
      Add-WarningMessage "Required tool '$tool' was not found by manifest/local/PATH-style discovery. Strict gate will fail if -StrictRequiredTools is set."
      Add-Result -Name $tool -Category "required tool gate" -Status "missing"
    }
  }
}


function Remove-PathIfExists {
  param([string]$Path)

  if (-not $Path) { return }
  if (Test-Path -LiteralPath $Path) {
    if ($WhatIfOnly) {
      Write-Host "Would remove: $Path"
    } else {
      Write-Host "Removing: $Path"
      Remove-Item -LiteralPath $Path -Recurse -Force -ErrorAction Stop
    }
  }
}

function Uninstall-WithWinget {
  param(
    [string]$Name,
    [string]$PackageId
  )

  $winget = Test-CommandPath "winget.exe"
  if (-not $winget) { $winget = Test-CommandPath "winget" }

  if (-not $winget) {
    Add-WarningMessage "winget was not found; cannot uninstall $Name package $PackageId."
    Add-Result -Name $Name -Category "uninstall" -Status "winget-missing" -Source $PackageId
    return
  }

  if ($WhatIfOnly) {
    Write-Host "Would uninstall $Name with winget package $PackageId"
    Add-Result -Name $Name -Category "uninstall" -Status "planned" -Source $PackageId -Notes "WhatIfOnly"
    return
  }

  Write-Host "Uninstalling $Name with winget package $PackageId"
  & $winget uninstall --id $PackageId --silent --accept-source-agreements --disable-interactivity
  if ($LASTEXITCODE -eq 0) {
    Add-Result -Name $Name -Category "uninstall" -Status "uninstalled-or-not-present" -Source $PackageId
  } else {
    Add-WarningMessage "winget uninstall failed for $Name ($PackageId) with exit code $LASTEXITCODE"
    Add-Result -Name $Name -Category "uninstall" -Status "failed" -Source $PackageId -Notes "ExitCode=$LASTEXITCODE"
  }
}

function Uninstall-PythonUserTool {
  param(
    [string]$Command,
    [string]$PackageName
  )

  $pipx = Test-CommandPath "pipx.exe"
  if (-not $pipx) { $pipx = Test-CommandPath "pipx" }

  if ($pipx) {
    if ($WhatIfOnly) {
      Write-Host "Would uninstall $PackageName with pipx"
    } else {
      Write-Host "Uninstalling $PackageName with pipx if present"
      & $pipx uninstall $PackageName
    }
    Add-Result -Name $Command -Category "uninstall python" -Status "pipx-uninstall-attempted" -Source "pipx:$PackageName"
  }

  foreach ($pycmd in @("py.exe", "python.exe", "python")) {
    $py = Test-CommandPath $pycmd
    if ($py) {
      if ($WhatIfOnly) {
        Write-Host "Would uninstall $PackageName with $pycmd -m pip uninstall -y"
      } else {
        Write-Host "Uninstalling $PackageName with $pycmd -m pip uninstall -y if present"
        & $py -m pip uninstall -y $PackageName
      }
      Add-Result -Name $Command -Category "uninstall python" -Status "pip-uninstall-attempted" -Source "$pycmd -m pip:$PackageName"
      return
    }
  }

  Add-WarningMessage "No Python command was found for uninstalling $PackageName."
  Add-Result -Name $Command -Category "uninstall python" -Status "python-missing" -Source $PackageName
}

function Invoke-UninstallMode {
  Write-Host "Security audit tool uninstaller"
  Write-Host "InstallRoot: $InstallRoot"
  Write-Host "RemoveSharedPackages: $RemoveSharedPackages"
  Write-Host "RemovePythonPackages: $RemovePythonPackages"
  Write-Host "WhatIfOnly: $WhatIfOnly"
if ($Uninstall) { Invoke-UninstallMode }

  Remove-PathIfExists -Path $InstallRoot
  Add-Result -Name "InstallRoot" -Category "uninstall" -Status "removed-or-not-present" -Path $InstallRoot -Notes "Managed portable tools and evidence root"

  if ($RemoveSharedPackages) {
    Uninstall-WithWinget -Name "WinDbg Preview" -PackageId "Microsoft.WinDbg"
    Uninstall-WithWinget -Name "LLVM" -PackageId "LLVM.LLVM"
  } else {
    Add-WarningMessage "Shared package-manager installs were not removed. Use -Uninstall -RemoveSharedPackages to attempt removing WinDbg/LLVM packages."
  }

  if ($RemovePythonPackages) {
    Uninstall-PythonUserTool -Command "semgrep.exe" -PackageName "semgrep"
    Uninstall-PythonUserTool -Command "flawfinder.exe" -PackageName "flawfinder"
    Uninstall-PythonUserTool -Command "pip-audit.exe" -PackageName "pip-audit"
  } else {
    Add-WarningMessage "Python/pip-based packages were not removed. Use -Uninstall -RemovePythonPackages to attempt removing semgrep/flawfinder/pip-audit."
  }

  if (-not $WhatIfOnly) {
    Ensure-Dir $InstallRoot
    $uninstallManifestPath = Join-Path $InstallRoot "security-audit-tool-uninstall-manifest.json"
    $uninstallWarningsPath = Join-Path $InstallRoot "security-audit-tool-uninstall-warnings.txt"
    $uninstallManifest = [pscustomobject]@{
      generated_at = (Get-Date).ToString("o")
      install_root = $InstallRoot
      remove_shared_packages = [bool]$RemoveSharedPackages
      remove_python_packages = [bool]$RemovePythonPackages
      results = $script:Results
      warnings = $script:Warnings
    }
    $uninstallManifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $uninstallManifestPath -Encoding UTF8
    $script:Warnings | Set-Content -LiteralPath $uninstallWarningsPath -Encoding UTF8
    Write-Host "Wrote uninstall manifest: $uninstallManifestPath"
    Write-Host "Wrote uninstall warnings: $uninstallWarningsPath"
  }

  if ($script:Warnings.Count -gt 0) {
    Write-Host "Uninstall completed with warnings." -ForegroundColor Yellow
    exit 2
  }

  Write-Host "Uninstall completed." -ForegroundColor Green
  exit 0
}

function Get-FileSignatureSummary {
  param(
    [string]$Path,
    [string]$ExpectedPublisherRegex = ""
  )

  if (-not (Test-Path -LiteralPath $Path)) {
    return "Missing"
  }

  try {
    $sig = Get-AuthenticodeSignature -FilePath $Path
    $subject = ""
    if ($sig.SignerCertificate) { $subject = $sig.SignerCertificate.Subject }

    if ($sig.Status -ne "Valid") {
      return "Invalid:$($sig.Status); Subject=$subject"
    }

    if ($ExpectedPublisherRegex -and ($subject -notmatch $ExpectedPublisherRegex)) {
      return "ValidSignatureUnexpectedPublisher; Subject=$subject"
    }

    return "Valid; Subject=$subject"
  } catch {
    return "SignatureCheckFailed:$($_.Exception.Message)"
  }
}

function Invoke-SafeDownload {
  param(
    [string]$Name,
    [string]$Uri,
    [string]$OutFile,
    [string]$ExpectedPublisherRegex = "",
    [switch]$RequireValidSignature
  )

  $parent = Split-Path -Parent $OutFile
  Ensure-Dir $parent

  if (Test-Path -LiteralPath $OutFile) {
    Write-Host "Already present: $OutFile"
  } elseif ($WhatIfOnly) {
    Write-Host "Would download $Name from $Uri to $OutFile"
    Add-Result -Name $Name -Category "download" -Status "planned" -Path $OutFile -Source $Uri -Notes "WhatIfOnly"
    return
  } else {
    Write-Host "Downloading $Name from $Uri"
    Invoke-WebRequest -Uri $Uri -OutFile $OutFile -UseBasicParsing
  }

  if (Test-Path -LiteralPath $OutFile) {
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $OutFile).Hash
    $sigSummary = Get-FileSignatureSummary -Path $OutFile -ExpectedPublisherRegex $ExpectedPublisherRegex

    if ($RequireValidSignature -and ($sigSummary -notlike "Valid;*")) {
      Add-WarningMessage "$Name was downloaded but did not have the expected valid Authenticode signature. Signature: $sigSummary"
      Add-Result -Name $Name -Category "download" -Status "signature-warning" -Path $OutFile -Source $Uri -SignatureStatus $sigSummary -Notes "SHA256=$hash"
    } else {
      Add-Result -Name $Name -Category "download" -Status "available" -Path $OutFile -Source $Uri -SignatureStatus $sigSummary -Notes "SHA256=$hash"
    }
  }
}

function Add-UserPathEntry {
  param([string]$PathToAdd)
  if (-not (Test-Path -LiteralPath $PathToAdd)) { return }

  $current = [Environment]::GetEnvironmentVariable("Path", "User")
  $parts = @()
  if ($current) { $parts = $current -split ";" | Where-Object { $_ -ne "" } }

  if ($parts -contains $PathToAdd) {
    Write-Host "User PATH already contains: $PathToAdd"
    return
  }

  if ($WhatIfOnly) {
    Write-Host "Would add to user PATH: $PathToAdd"
    return
  }

  $newPath = (($parts + $PathToAdd) -join ";")
  [Environment]::SetEnvironmentVariable("Path", $newPath, "User")
  Write-Host "Added to user PATH: $PathToAdd"
}

function Test-KnownPath {
  param(
    [string]$Name,
    [string]$Category,
    [string[]]$Paths,
    [string]$WarningIfMissing = ""
  )

  foreach ($p in $Paths) {
    $expanded = [Environment]::ExpandEnvironmentVariables($p)
    if (Test-Path -LiteralPath $expanded) {
      Add-Result -Name $Name -Category $Category -Status "available" -Path $expanded
      return $expanded
    }
  }

  $cmdPath = Test-CommandPath $Name
  if ($cmdPath) {
    Add-Result -Name $Name -Category $Category -Status "available-in-path" -Path $cmdPath
    return $cmdPath
  }

  Add-Result -Name $Name -Category $Category -Status "missing" -Notes $WarningIfMissing
  if ($WarningIfMissing) { Add-WarningMessage $WarningIfMissing }
  return $null
}

function Test-WingetPackageInstalled {
  param(
    [string]$PackageId
  )

  $winget = Test-CommandPath "winget.exe"
  if (-not $winget) { return $false }

  try {
    & $winget list --id $PackageId -e --disable-interactivity | Out-Null
    return ($LASTEXITCODE -eq 0)
  } catch {
    return $false
  }
}

function Install-WithWinget {
  param(
    [string]$Name,
    [string]$PackageId,
    [string]$Reason
  )

  $winget = Test-CommandPath "winget.exe"
  if (-not $winget) {
    Add-WarningMessage "winget.exe is unavailable; cannot install $Name ($PackageId). $Reason"
    Add-Result -Name $Name -Category "winget" -Status "missing-winget" -Notes $Reason
    return
  }

  if ($WhatIfOnly) {
    Write-Host "Would install $Name via winget package $PackageId"
    Add-Result -Name $Name -Category "winget" -Status "planned" -Source "winget:$PackageId" -Notes $Reason
    return
  }

  if (Test-WingetPackageInstalled -PackageId $PackageId) {
    Write-Host "$Name already appears installed according to winget package $PackageId"
    Add-Result -Name $Name -Category "winget" -Status "already-installed" -Source "winget:$PackageId" -Notes $Reason
    return
  }

  Write-Host "Installing $Name via winget package $PackageId"
  & $winget install --id $PackageId -e --accept-package-agreements --accept-source-agreements --disable-interactivity
  $exit = $LASTEXITCODE

  if ($exit -eq 0) {
    Add-Result -Name $Name -Category "winget" -Status "installed-or-present" -Source "winget:$PackageId" -Notes $Reason
    return
  }

  if (Test-WingetPackageInstalled -PackageId $PackageId) {
    Write-Host "$Name appears installed after winget returned exit code $exit; treating as available."
    Add-Result -Name $Name -Category "winget" -Status "available-after-nonzero-winget" -Source "winget:$PackageId" -Notes "winget exit code $exit; $Reason"
    return
  }

  Add-WarningMessage "winget install failed for $Name ($PackageId), exit code $exit"
  Add-Result -Name $Name -Category "winget" -Status "install-failed" -Source "winget:$PackageId" -Notes "ExitCode=$exit; $Reason"
}

function Find-VSTools {
  param([string]$VSWherePath)

  $vsInstall = $null
  if ($VSWherePath -and (Test-Path -LiteralPath $VSWherePath)) {
    try {
      $vsInstall = & $VSWherePath -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    } catch {
      Add-WarningMessage "vswhere failed while locating MSVC tools: $($_.Exception.Message)"
    }
  }

  $roots = @()
  if ($vsInstall) { $roots += $vsInstall }
  $roots += @(
    "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community",
    "${env:ProgramFiles}\Microsoft Visual Studio\2022\Professional",
    "${env:ProgramFiles}\Microsoft Visual Studio\2022\Enterprise",
    "${env:ProgramFiles}\Microsoft Visual Studio\2022\BuildTools"
  )

  $toolNames = @("dumpbin.exe", "link.exe", "lib.exe", "editbin.exe", "undname.exe")
  foreach ($tool in $toolNames) {
    $found = $null
    foreach ($root in $roots | Select-Object -Unique) {
      if (-not $root -or -not (Test-Path -LiteralPath $root)) { continue }
      $matches = Get-ChildItem -LiteralPath $root -Recurse -Filter $tool -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match "\\VC\\Tools\\MSVC\\.*\\bin\\Hostx64\\x64\\" } |
        Sort-Object FullName -Descending |
        Select-Object -First 1
      if ($matches) {
        $found = $matches.FullName
        break
      }
    }

    if ($found) {
      Add-Result -Name $tool -Category "MSVC binary tools" -Status "available" -Path $found
    } else {
      Add-WarningMessage "$tool was not found. MSVC PE/COFF inspection or mutation-capable tooling may be unavailable. Visual Studio Build Tools are intentionally not installed by this script."
      Add-Result -Name $tool -Category "MSVC binary tools" -Status "missing" -Notes "Large dependency intentionally not installed"
    }
  }
}

Ensure-Dir $InstallRoot
Ensure-Dir $BinRoot
Ensure-Dir $SastDir
Ensure-Dir $LogDir

Write-Host "Security audit tool installer/detector"
Write-Host "InstallRoot: $InstallRoot"
Write-Host "ProjectRoot: $ProjectRoot"
Write-Host "Full: $Full"
Write-Host "Minimal: $Minimal"
Write-Host "WhatIfOnly: $WhatIfOnly"
if ($Uninstall) { Invoke-UninstallMode }

# Locate local docs.
$debugDocCandidates = @(
  $DebugToolsMdPath,
  ".\llm-wiki\debug-tools-security-audit.md",
  ".\llm-wiki\debug-tools.md",
  ".\debug-tools-security-audit.md",
  ".\debug-tools.md"
) | Select-Object -Unique

$debugDocFound = $null
foreach ($doc in $debugDocCandidates) {
  $expanded = [IO.Path]::GetFullPath($doc)
  if (Test-Path -LiteralPath $expanded) {
    $debugDocFound = $expanded
    break
  }
}

if ($debugDocFound) {
  Add-Result -Name "debug-tools local doc" -Category "local guidance" -Status "available" -Path $debugDocFound
} else {
  Add-WarningMessage "No debug-tools-security-audit.md or debug-tools.md file was found in the expected locations. Local project-specific tool guidance is unavailable."
  Add-Result -Name "debug-tools local doc" -Category "local guidance" -Status "missing"
}


$ProjectSignals = Get-ProjectSignals -Root $ProjectRoot
Install-OptionalSastTools
Detect-SastSecretsDependencyTools -Signals $ProjectSignals
Test-RequiredToolGate

# Download small portable Sysinternals tools by default.
if (-not $SkipSysinternals) {
  $sysinternalsBase = "https://live.sysinternals.com"
  $coreTools = @(
    "procdump.exe",
    "sigcheck.exe",
    "strings.exe",
    "handle.exe",
    "listdlls.exe",
    "vmmap.exe"
  )

  if ($IncludeGuiSysinternals) {
    $coreTools += @("Procmon.exe", "procexp.exe")
  } else {
    Add-WarningMessage "GUI Sysinternals tools procmon.exe and procexp.exe were not downloaded by default. Use -IncludeGuiSysinternals if runtime tracing/process inspection is needed."
  }

  foreach ($tool in $coreTools | Select-Object -Unique) {
    $url = "$sysinternalsBase/$tool"
    $out = Join-Path $SysinternalsDir $tool
    Invoke-SafeDownload -Name $tool -Uri $url -OutFile $out -ExpectedPublisherRegex "Microsoft" -RequireValidSignature
  }
} else {
  Add-WarningMessage "Sysinternals downloads were skipped. Crash capture, signatures, strings, handles, DLL listing, and runtime inspection coverage may be reduced."
}

# Download portable vswhere by default for Visual Studio detection.
$vswherePath = $null
if (-not $SkipVSWhere) {
  $vswherePath = Join-Path $VSWhereDir "vswhere.exe"
  Invoke-SafeDownload `
    -Name "vswhere.exe" `
    -Uri "https://github.com/microsoft/vswhere/releases/latest/download/vswhere.exe" `
    -OutFile $vswherePath `
    -ExpectedPublisherRegex "Microsoft" `
    -RequireValidSignature
} else {
  Add-WarningMessage "vswhere download was skipped. MSVC tool discovery may be less reliable."
}

if (-not $vswherePath -or -not (Test-Path -LiteralPath $vswherePath)) {
  $vswherePath = Test-CommandPath "vswhere.exe"
}

# Optional WinDbg Preview.
if ($IncludeWinDbg) {
  Install-WithWinget -Name "WinDbg Preview" -PackageId "Microsoft.WinDbg" -Reason "Official Microsoft debugger; not portable"
} else {
  Add-WarningMessage "WinDbg Preview was not installed by default. Use -IncludeWinDbg if crash dump debugging is needed and Windows SDK Debugging Tools are unavailable."
}

# Optional LLVM.
if ($IncludeLLVMViaWinget) {
  Install-WithWinget -Name "LLVM" -PackageId "LLVM.LLVM" -Reason "Provides llvm-strings/llvm-objdump; larger non-portable package"
} else {
  Add-WarningMessage "LLVM was not installed by default. Use -IncludeLLVMViaWinget if llvm-strings or llvm-objdump is needed."
}

# Optional FFmpeg build.
if ($IncludeFFmpeg) {
  $zipPath = Join-Path $FFmpegDir "ffmpeg-release-essentials.zip"
  Invoke-SafeDownload `
    -Name "ffmpeg-release-essentials.zip" `
    -Uri "https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip" `
    -OutFile $zipPath

  if ((Test-Path -LiteralPath $zipPath) -and -not $WhatIfOnly) {
    $extractRoot = Join-Path $FFmpegDir "extract"
    Ensure-Dir $extractRoot
    Write-Host "Extracting FFmpeg archive to $extractRoot"
    Expand-Archive -LiteralPath $zipPath -DestinationPath $extractRoot -Force
    $ffmpegExe = Get-ChildItem -LiteralPath $extractRoot -Recurse -Filter "ffmpeg.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
    $ffprobeExe = Get-ChildItem -LiteralPath $extractRoot -Recurse -Filter "ffprobe.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($ffmpegExe) { Add-Result -Name "ffmpeg.exe" -Category "media/capture" -Status "available" -Path $ffmpegExe.FullName -Source "gyan.dev" }
    if ($ffprobeExe) { Add-Result -Name "ffprobe.exe" -Category "media/capture" -Status "available" -Path $ffprobeExe.FullName -Source "gyan.dev" }
    if (-not $ffmpegExe -or -not $ffprobeExe) {
      Add-WarningMessage "FFmpeg archive extracted but ffmpeg.exe or ffprobe.exe was not found."
    }
  }

  Add-WarningMessage "FFmpeg Windows binary was downloaded from an opt-in third-party build source. Record hash and source in audit evidence."
} else {
  Add-WarningMessage "FFmpeg/ffprobe were not installed by default. Use -IncludeFFmpeg only when capture/media inspection is needed."
}

# Detect Windows SDK Debugging Tools.
$sdkDebuggerPaths = @{
  "cdb.exe" = @(
    "${env:ProgramFiles(x86)}\Windows Kits\10\Debuggers\x64\cdb.exe",
    "${env:ProgramFiles}\Windows Kits\10\Debuggers\x64\cdb.exe"
  )
  "windbg.exe" = @(
    "${env:ProgramFiles(x86)}\Windows Kits\10\Debuggers\x64\windbg.exe",
    "${env:ProgramFiles}\Windows Kits\10\Debuggers\x64\windbg.exe"
  )
  "dumpchk.exe" = @(
    "${env:ProgramFiles(x86)}\Windows Kits\10\Debuggers\x64\dumpchk.exe",
    "${env:ProgramFiles}\Windows Kits\10\Debuggers\x64\dumpchk.exe"
  )
  "symchk.exe" = @(
    "${env:ProgramFiles(x86)}\Windows Kits\10\Debuggers\x64\symchk.exe",
    "${env:ProgramFiles}\Windows Kits\10\Debuggers\x64\symchk.exe"
  )
  "dbh.exe" = @(
    "${env:ProgramFiles(x86)}\Windows Kits\10\Debuggers\x64\dbh.exe",
    "${env:ProgramFiles}\Windows Kits\10\Debuggers\x64\dbh.exe"
  )
  "pdbcopy.exe" = @(
    "${env:ProgramFiles(x86)}\Windows Kits\10\Debuggers\x64\pdbcopy.exe",
    "${env:ProgramFiles}\Windows Kits\10\Debuggers\x64\pdbcopy.exe"
  )
  "symstore.exe" = @(
    "${env:ProgramFiles(x86)}\Windows Kits\10\Debuggers\x64\symstore.exe",
    "${env:ProgramFiles}\Windows Kits\10\Debuggers\x64\symstore.exe"
  )
  "gflags.exe" = @(
    "${env:ProgramFiles(x86)}\Windows Kits\10\Debuggers\x64\gflags.exe",
    "${env:ProgramFiles}\Windows Kits\10\Debuggers\x64\gflags.exe"
  )
  "umdh.exe" = @(
    "${env:ProgramFiles(x86)}\Windows Kits\10\Debuggers\x64\umdh.exe",
    "${env:ProgramFiles}\Windows Kits\10\Debuggers\x64\umdh.exe"
  )
}

foreach ($name in $sdkDebuggerPaths.Keys) {
  Test-KnownPath -Name $name -Category "Windows SDK Debugging Tools" -Paths $sdkDebuggerPaths[$name] -WarningIfMissing "$name was not found. Windows SDK Debugging Tools are intentionally not installed by this script because they are large; dump/symbol/debug coverage may be reduced." | Out-Null
}

# Detect WinDbg Preview alias.
$windbgX = Test-KnownPath -Name "WinDbgX.exe" -Category "WinDbg Preview" -Paths @(
  "%LOCALAPPDATA%\Microsoft\WindowsApps\WinDbgX.exe"
)
if (-not $windbgX) {
  if (Test-WingetPackageInstalled -PackageId "Microsoft.WinDbg") {
    Add-Result -Name "WinDbgX.exe" -Category "WinDbg Preview" -Status "package-installed-alias-not-resolved" -Source "winget:Microsoft.WinDbg" -Notes "Package appears installed, but alias was not found in current session"
  } else {
    Add-WarningMessage "WinDbgX.exe was not found. Use -IncludeWinDbg to install WinDbg Preview through winget."
    Add-Result -Name "WinDbgX.exe" -Category "WinDbg Preview" -Status "missing"
  }
}

# Detect Visual Studio PE/COFF tools.
Find-VSTools -VSWherePath $vswherePath

function Find-LlvmTool {
  param([string]$ToolName)

  $pathFound = Test-LocalToolPath $ToolName
  if ($pathFound) { return $pathFound }

  foreach ($root in @(
    "${env:ProgramFiles}\LLVM\bin",
    "${env:ProgramFiles(x86)}\LLVM\bin",
    "${env:LOCALAPPDATA}\Programs\LLVM\bin"
  )) {
    if (-not $root -or -not (Test-Path -LiteralPath $root)) { continue }
    $candidate = Join-Path $root $ToolName
    if (Test-Path -LiteralPath $candidate) { return $candidate }
  }

  return $null
}

# Detect LLVM tools.
foreach ($tool in @("llvm-strings.exe", "llvm-objdump.exe")) {
  $pathFound = Find-LlvmTool -ToolName $tool
  if ($pathFound) {
    Add-Result -Name $tool -Category "LLVM tools" -Status "available" -Path $pathFound -Notes "Resolved from PATH, local roots, or known LLVM install directories"
  } else {
    Add-Result -Name $tool -Category "LLVM tools" -Status "missing" -Notes "Use -IncludeLLVMViaWinget if needed; a new shell may be needed after winget PATH changes"
    Add-WarningMessage "$tool was not found in PATH or known LLVM install directories. LLVM-specific binary inspection coverage may be reduced."
  }
}

# Detect FFmpeg if not installed here.
foreach ($tool in @("ffmpeg.exe", "ffprobe.exe")) {
  $pathFound = Test-CommandPath $tool
  if ($pathFound) {
    Add-Result -Name $tool -Category "media/capture" -Status "available-in-path" -Path $pathFound
  } else {
    $installed = Get-ChildItem -LiteralPath $FFmpegDir -Recurse -Filter $tool -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($installed) {
      Add-Result -Name $tool -Category "media/capture" -Status "available" -Path $installed.FullName
    } else {
      Add-Result -Name $tool -Category "media/capture" -Status "missing" -Notes "Use -IncludeFFmpeg if needed"
    }
  }
}

# Add portable dirs to PATH if requested.
if ($AddToUserPath) {
  Add-UserPathEntry $SysinternalsDir
  Add-UserPathEntry $VSWhereDir
  Add-UserPathEntry $SastDir

  $ffmpegBin = Get-ChildItem -LiteralPath $FFmpegDir -Recurse -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match "\\bin$" } |
    Select-Object -First 1
  if ($ffmpegBin) { Add-UserPathEntry $ffmpegBin.FullName }

  # Add Python user Scripts directories only when explicitly requested.
  if ($env:APPDATA) {
    $pythonRoot = Join-Path $env:APPDATA "Python"
    if (Test-Path -LiteralPath $pythonRoot) {
      Get-ChildItem -LiteralPath $pythonRoot -Recurse -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match "\\Scripts$" } |
        ForEach-Object { Add-UserPathEntry $_.FullName }
    }
  }
}

# Write outputs.
$manifest = [pscustomobject]@{
  generated_at = (Get-Date).ToString("o")
  install_root = $InstallRoot
  project_root = $ProjectRoot
  debug_tools_doc = $debugDocFound
  strict_required_tools = [bool]$StrictRequiredTools
  required_tools = $RequireTools
  required_tool_missing = [bool]$script:RequiredToolMissing
  full_mode = [bool]$Full
  minimal_mode = [bool]$Minimal
  default_python_sast_install = $false
  default_secrets_install = [bool]((-not $Minimal) -and (-not $SkipSecretsInstall))
  default_dependency_scanner_install = [bool]((-not $Minimal) -and (-not $SkipDependencyScannerInstall))
  host = [pscustomobject]@{
    computer_name = $env:COMPUTERNAME
    user = $env:USERNAME
    os = (Get-CimInstance Win32_OperatingSystem -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Caption)
    os_architecture = (Get-CimInstance Win32_OperatingSystem -ErrorAction SilentlyContinue | Select-Object -ExpandProperty OSArchitecture)
    processor_architecture = $env:PROCESSOR_ARCHITECTURE
    powershell_version = $PSVersionTable.PSVersion.ToString()
  }
  results = $script:Results
  warnings = $script:Warnings
}

if ($WhatIfOnly) {
  Write-Host "WhatIfOnly enabled; manifest files will not be written."
} else {
  $manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $ManifestPath -Encoding UTF8
  $script:Warnings | Set-Content -LiteralPath $WarningsPath -Encoding UTF8

  $md = New-Object System.Collections.Generic.List[string]
  $md.Add("# Security Audit Tool Availability")
  $md.Add("")
  $md.Add("- Generated: $($manifest.generated_at)")
  $md.Add('- Install root: `' + $InstallRoot + '`')
  $md.Add('- Debug tools doc: `' + $debugDocFound + '`')
  $md.Add('- Full mode: ' + [string]([bool]$Full))
  $md.Add('- Minimal mode: ' + [string]([bool]$Minimal))
  $md.Add('- Strict required tools: ' + [string]([bool]$StrictRequiredTools))
  $md.Add('- Required tool missing: ' + [string]([bool]$script:RequiredToolMissing))
  $md.Add("")
  $md.Add("## Warnings")
  $md.Add("")
  if ($script:Warnings.Count -eq 0) {
    $md.Add("- None")
  } else {
    foreach ($w in $script:Warnings) { $md.Add("- WARNING: $w") }
  }
  $md.Add("")
  $md.Add("## Tool results")
  $md.Add("")
  $md.Add("| Tool | Category | Status | Path | Signature | Notes |")
  $md.Add("|---|---|---|---|---|---|")
  foreach ($r in $script:Results) {
    $md.Add(('| {0} | {1} | {2} | `{3}` | {4} | {5} |' -f $r.name, $r.category, $r.status, $r.path, $r.signature_status, $r.notes))
  }
  $md | Set-Content -LiteralPath $MarkdownPath -Encoding UTF8

  Write-Host "Wrote manifest: $ManifestPath"
  Write-Host "Wrote warnings: $WarningsPath"
  Write-Host "Wrote Markdown evidence: $MarkdownPath"
}

if ($StrictRequiredTools -and $script:RequiredToolMissing) {
  Write-Host ""
  Write-Host "Completed with missing required tools. Strict required-tool gate failed." -ForegroundColor Red
  exit 3
}

if ($script:Warnings.Count -gt 0) {
  Write-Host ""
  Write-Host "Completed with warnings. These should be reflected in audit confidence/scoring." -ForegroundColor Yellow
  exit 2
}

Write-Host "Completed without warnings." -ForegroundColor Green
exit 0
