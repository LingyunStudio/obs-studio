# OBS Studio Custom Build - Packaging Script
# Creates a portable ZIP and (optionally) an Inno Setup installer.
#
# Prerequisites:
#   - A configured RelWithDebInfo build in .\build_x64
#   - Inno Setup 6 (at D:\InnoSetup6) when using -Setup
#
# Usage:
#   .\package.ps1                     # Build + ZIP only
#   .\package.ps1 -Setup              # Build + ZIP + Inno Setup installer
#   .\package.ps1 -SkipBuild          # Package existing build output without rebuilding
#   .\package.ps1 -Version "36.0.1"   # Override the displayed version

param(
    [switch]$Setup,
    [switch]$SkipBuild,
    [string]$Version = "",
    [string]$VcRedist = ""
)

$ErrorActionPreference = "Stop"
$RootDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildDir = "$RootDir\build_x64"
$PackageDir = "$RootDir\packages"
$StageBase = "$PackageDir\stage"
$PortableStage = "$StageBase\portable"
$InstallerStage = "$StageBase\installer"
$InnoSetupDir = "D:\InnoSetup6"

function Assert-LastExit($Message) {
    if ($LASTEXITCODE -ne 0) {
        throw $Message
    }
}

# ---- Version ----
if (-not $Version) {
    $ProjectVer = "0.0.0"
    $CacheLine = Select-String -Path "$BuildDir\CMakeCache.txt" -Pattern "^CMAKE_PROJECT_VERSION:STATIC=(.+)$" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($CacheLine) {
        $ProjectVer = $CacheLine.Matches[0].Groups[1].Value
    }
    Push-Location $RootDir
    try {
        $Sha = git rev-parse --short HEAD 2>$null
    }
    finally {
        Pop-Location
    }
    $Version = if ($Sha) { "$ProjectVer-custom-$Sha" } else { "$ProjectVer-custom" }
}
if ($Version -match '^(\d+)\.(\d+)\.(\d+)') {
    $VerMajor = $Matches[1]
    $VerMinor = $Matches[2]
    $VerPatch = $Matches[3]
} else {
    $VerMajor = "0"
    $VerMinor = "0"
    $VerPatch = "0"
}
$VerBuild = "0"
$NumericVersion = "$VerMajor.$VerMinor.$VerPatch.$VerBuild"
$BaseName = "OBS-Studio-$Version-x64"

Write-Host "==> Version: $Version (numeric: $NumericVersion)" -ForegroundColor Cyan

# ---- Build ----
if (-not $SkipBuild) {
    Write-Host "`n==> Building RelWithDebInfo (x64)..." -ForegroundColor Yellow
    cmake --build $BuildDir --config RelWithDebInfo
    Assert-LastExit "Build failed"
    Write-Host "==> Build complete" -ForegroundColor Green
} else {
    Write-Host "==> Skipping build (-SkipBuild)" -ForegroundColor Yellow
}

if (-not (Test-Path "$BuildDir\cmake_install.cmake")) {
    throw "No configured CMake build found at $BuildDir (run the cmake configure step first)"
}

# ---- Install into staging ----
foreach ($stage in @($PortableStage, $InstallerStage)) {
    if (Test-Path $stage) {
        Remove-Item -Recurse -Force $stage
    }
    New-Item -ItemType Directory -Force -Path $stage | Out-Null
}

Write-Host "`n==> Staging install tree (cmake --install)..." -ForegroundColor Yellow
cmake --install $BuildDir --config RelWithDebInfo --prefix $PortableStage
Assert-LastExit "cmake --install failed for portable staging"

# ---- Strip debug/dev files for a release-grade package ----
Write-Host "    Stripping debug symbols (.pdb)..." -ForegroundColor Gray
Get-ChildItem -Path $PortableStage -Recurse -File -Include "*.pdb","*.lib","*.exp","*.ilk" |
    Remove-Item -Force -ErrorAction SilentlyContinue

# The installer uses the same file tree, minus the portable-mode marker.
Copy-Item -Recurse -Force "$PortableStage\*" $InstallerStage

# ---- Portable marker ----
New-Item -ItemType File -Force -Path "$PortableStage\portable_mode.txt" | Out-Null

# ---- Visual C++ runtime ----
# Fresh Windows machines lack vcruntime140.dll / msvcp140.dll, without which
# obs64.exe will not start. Bundle vc_redist.x64.exe: the installer runs it
# silently when the runtime is missing, and the portable zip ships it for the
# user to run manually.
if (-not $VcRedist) {
    $VcRedistCandidates = @(
        "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Redist\MSVC\v143\vc_redist.x64.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Redist\MSVC\v143\vc_redist.x64.exe",
        "C:\Program Files (x86)\Microsoft Visual Studio\2022\Community\VC\Redist\MSVC\v143\vc_redist.x64.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Redist\MSVC\v143\vc_redist.x64.exe"
    )
    $VcRedistCandidates = $VcRedistCandidates | Sort-Object -Unique
    foreach ($candidate in $VcRedistCandidates) {
        if (Test-Path $candidate) {
            $VcRedist = $candidate
            break
        }
    }
}

$VcRedistAvailable = $false
if ($VcRedist -and (Test-Path $VcRedist)) {
    $VcRedistAvailable = $true
    Copy-Item -Force $VcRedist "$PortableStage\vc_redist.x64.exe"
    Write-Host "    Bundled VC++ runtime: $VcRedist" -ForegroundColor Gray
} else {
    Write-Warning "vc_redist.x64.exe not found; packages will require the user to install the VC++ 2015-2022 runtime separately. Pass -VcRedist <path> to specify it."
}

function Copy-Tree($Source, $Dest) {
    New-Item -ItemType Directory -Force -Path $Dest | Out-Null
    Copy-Item -Recurse -Force "$Source\*" $Dest
}

function Assert-RunTree($Dir) {
    $missing = @()
    foreach ($f in @("bin\64bit\obs64.exe",
                     "bin\64bit\Qt6Core.dll",
                     "obs-plugins\64bit",
                     "data\obs-studio",
                     "data\obs-plugins\win-capture",
                     "data\obs-plugins\frontend-tools")) {
        if (-not (Test-Path (Join-Path $Dir $f))) {
            $missing += $f
        }
    }
    if ($missing.Count -gt 0) {
        throw "Staged runtime tree is broken; missing:`n  $($missing -join "`n  ")"
    }
}

Assert-RunTree $PortableStage
Assert-RunTree $InstallerStage

# ---- Portable ZIP ----
New-Item -ItemType Directory -Force -Path "$PackageDir\portable" | Out-Null
$ZipPath = "$PackageDir\portable\$BaseName-portable.zip"
if (Test-Path $ZipPath) {
    Remove-Item -Force $ZipPath
}

# Put everything under a top-level "obs-studio" folder inside the zip.
$ZipRoot = "$StageBase\ziproot\obs-studio"
if (Test-Path (Split-Path $ZipRoot)) {
    Remove-Item -Recurse -Force (Split-Path $ZipRoot)
}
Copy-Tree $PortableStage $ZipRoot

Write-Host "`n==> Creating portable ZIP..." -ForegroundColor Yellow
Push-Location "$StageBase\ziproot"
try {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [System.IO.Compression.ZipFile]::CreateFromDirectory(
        "$StageBase\ziproot",
        $ZipPath,
        [System.IO.Compression.CompressionLevel]::Optimal,
        $false) | Out-Null
}
finally {
    Pop-Location
}
Write-Host "==> Portable ZIP: $ZipPath" -ForegroundColor Green

# ---- Inno Setup installer ----
$InstallerExe = $null
if ($Setup) {
    Write-Host "`n==> Creating Inno Setup installer..." -ForegroundColor Yellow

    New-Item -ItemType Directory -Force -Path "$PackageDir\installer" | Out-Null
    $IssPath = "$PackageDir\installer\setup.iss"

    $LicensePath = "$RootDir\COPYING"
    $IconPath = "$RootDir\frontend\cmake\windows\obs-studio.ico"

    $VcRedistFileLine = ""
    $VcRedistRunLine = ""
    if ($VcRedistAvailable) {
        $VcRedistFileLine = "`r`n; VC++ runtime (extracted to {tmp}, deleted after install)`r`nSource: `"$VcRedist`"; DestDir: {tmp}; Flags: deleteafterinstall"
        $VcRedistRunLine = "`r`nFilename: {tmp}\vc_redist.x64.exe; Parameters: /install /quiet /norestart; StatusMsg: Installing Visual C++ runtime...; Check: NeedsVCRedist; Flags: waituntilterminated"
    }

    # Inno Setup constants must be escaped for the .iss file; paths use
    # backslashes already.
    @"
; OBS Studio Custom Build - auto-generated by package.ps1
#define MyAppName "OBS Studio (Custom)"
#define MyAppVersion "$Version"
#define MyAppPublisher "dahu"
#define MyAppURL "https://github.com/dahu/obs-studio"
#define MyAppExeName "obs64.exe"
#define MyAppId "{{CA9E1B4E-18E1-4C5B-8A2D-F1A0B3C4D5E6}"

[Setup]
AppId={#MyAppId}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
VersionInfoVersion=$NumericVersion
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={autopf}\obs-studio-custom
DefaultGroupName=OBS Studio (Custom)
AllowNoIcons=yes
LicenseFile=$LicensePath
OutputDir=$PackageDir\installer
OutputBaseFilename=$BaseName-setup
SetupIconFile=$IconPath
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayName={#MyAppName}
UsePreviousAppDir=yes

[Languages]
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; portable_mode.txt must NOT be shipped with the installer
Source: "$InstallerStage\*"; Excludes: "portable_mode.txt"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs$VcRedistFileLine

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\bin\64bit\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\bin\64bit\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\bin\64bit\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent$VcRedistRunLine

[Code]
// Return true if the installed VC++ 2015-2022 x64 runtime is missing or older
// than 14.40. OBS requires msvcp140.dll minor >= 40 to launch (see
// vc_runtime_outdated() in obs-main.cpp); the shared redist registry key may
// exist from an older VS 2015/2019 install, so the version must be compared,
// not just the key's existence.
function NeedsVCRedist: Boolean;
var
  Ver: String;
  MajorStr: String;
  MinorStr: String;
  p: Integer;
begin
  Result := True;
  if RegQueryStringValue(HKLM64,
       'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64',
       'Version', Ver) then
  begin
    // Value looks like "v14.44.35112.0" or "14.44.35112.0"
    if Ver[1] = 'v' then
      Delete(Ver, 1, 1);
    p := Pos('.', Ver);
    if p > 0 then
    begin
      MajorStr := Copy(Ver, 1, p - 1);
      Ver := Copy(Ver, p + 1, Length(Ver) - p);
      p := Pos('.', Ver);
      if p > 0 then
        MinorStr := Copy(Ver, 1, p - 1)
      else
        MinorStr := Ver;
      if (MajorStr = '14') and (StrToIntDef(MinorStr, 0) >= 40) then
        Result := False;
    end;
  end;
end;
"@ | Set-Content -Path $IssPath -Encoding UTF8

    $Iscc = "$InnoSetupDir\ISCC.exe"
    if (-not (Test-Path $Iscc)) {
        Write-Warning "Inno Setup not found at $Iscc - skipping installer (install Inno Setup 6 from https://jrsoftware.org/isinfo.php)"
    } else {
        & $Iscc $IssPath
        Assert-LastExit "Inno Setup compilation failed"
        $InstallerExe = "$PackageDir\installer\$BaseName-setup.exe"
        Write-Host "==> Installer: $InstallerExe" -ForegroundColor Green
    }
}

# ---- Summary ----
Write-Host "`n========================================" -ForegroundColor Cyan
Write-Host "  Packaging complete" -ForegroundColor Cyan
Write-Host "  Version: $Version" -ForegroundColor White
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  Portable ZIP : $ZipPath"
if ($InstallerExe) {
    Write-Host "  Installer    : $InstallerExe"
}
Write-Host "========================================" -ForegroundColor Cyan
