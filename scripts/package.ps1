param(
    [string]$BuildDir = "build",
    [string]$Configuration = "Release",
    [string]$Version = "dev",
    [string]$OutputDir = "dist"
)

$ErrorActionPreference = "Stop"

function Resolve-RepoPath {
    param([string]$Path)

    if ([System.IO.Path]::IsPathRooted($Path)) {
        return $Path
    }

    $repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
    return (Join-Path $repoRoot $Path)
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$buildRoot = Resolve-RepoPath -Path $BuildDir
$outputRoot = Resolve-RepoPath -Path $OutputDir

$exeCandidates = @(
    (Join-Path (Join-Path $buildRoot $Configuration) "loopguard.exe"),
    (Join-Path $buildRoot "loopguard.exe")
)

$exePath = $exeCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $exePath) {
    throw "loopguard.exe not found. Checked: $($exeCandidates -join ', ')"
}

$safeVersion = ($Version -replace '[^0-9A-Za-z._-]', '-').Trim('-')
if ([string]::IsNullOrWhiteSpace($safeVersion)) {
    $safeVersion = "dev"
}

$packageName = "loopguard-$safeVersion-windows-x64"
$stageDir = Join-Path $outputRoot $packageName
$zipPath = Join-Path $outputRoot "$packageName.zip"
$hashPath = Join-Path $outputRoot "$packageName.zip.sha256"

New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null
if (Test-Path $stageDir) {
    Remove-Item -Recurse -Force $stageDir
}
if (Test-Path $zipPath) {
    Remove-Item -Force $zipPath
}
if (Test-Path $hashPath) {
    Remove-Item -Force $hashPath
}

New-Item -ItemType Directory -Force -Path $stageDir | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stageDir "examples") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stageDir "prompts") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stageDir "scripts") | Out-Null

Copy-Item -Path $exePath -Destination (Join-Path $stageDir "loopguard.exe")
Copy-Item -Path (Join-Path $repoRoot "README.md") -Destination (Join-Path $stageDir "README.md")
Copy-Item -Recurse -Force -Path (Join-Path $repoRoot "examples\*") -Destination (Join-Path $stageDir "examples")
Copy-Item -Recurse -Force -Path (Join-Path $repoRoot "prompts\*") -Destination (Join-Path $stageDir "prompts")
Copy-Item -Recurse -Force -Path (Join-Path $repoRoot "scripts\*") -Destination (Join-Path $stageDir "scripts")

$licensePath = Join-Path $repoRoot "LICENSE"
if (Test-Path $licensePath) {
    Copy-Item -Path $licensePath -Destination (Join-Path $stageDir "LICENSE")
}

Compress-Archive -Path $stageDir -DestinationPath $zipPath -CompressionLevel Optimal

$hash = Get-FileHash -Path $zipPath -Algorithm SHA256
"{0}  {1}" -f $hash.Hash.ToLowerInvariant(), [System.IO.Path]::GetFileName($zipPath) | Set-Content -Path $hashPath -Encoding ascii

Write-Host "Created package: $zipPath"
Write-Host "Created checksum: $hashPath"

if ($env:GITHUB_OUTPUT) {
    "package_zip=$zipPath" | Out-File -FilePath $env:GITHUB_OUTPUT -Encoding utf8 -Append
    "package_sha256=$hashPath" | Out-File -FilePath $env:GITHUB_OUTPUT -Encoding utf8 -Append
    "package_name=$packageName" | Out-File -FilePath $env:GITHUB_OUTPUT -Encoding utf8 -Append
}
