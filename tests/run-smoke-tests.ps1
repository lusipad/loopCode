param(
    [string]$BuildDir = "build",
    [string]$Configuration = "Release"
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

function Assert-True {
    param(
        [bool]$Condition,
        [string]$Message
    )

    if (-not $Condition) {
        throw "Assertion failed: $Message"
    }
}

function Assert-Contains {
    param(
        [string]$Text,
        [string]$Needle,
        [string]$Message
    )

    if ($Text -notlike "*$Needle*") {
        throw "Assertion failed: $Message`nExpected to find: $Needle`nActual text:`n$Text"
    }
}

function Get-LoopGuardExe {
    param(
        [string]$BuildRoot,
        [string]$Configuration
    )

    $candidates = @(
        (Join-Path (Join-Path $BuildRoot $Configuration) "loopguard.exe"),
        (Join-Path $BuildRoot "loopguard.exe")
    )

    return $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
}

function Test-Package {
    param(
        [string]$RepoRoot,
        [string]$BuildRoot,
        [string]$Configuration,
        [string]$TestRoot
    )

    $outputDir = Join-Path $TestRoot "package"
    & (Join-Path $RepoRoot "scripts\package.ps1") -BuildDir $BuildRoot -Configuration $Configuration -Version "smoketest" -OutputDir $outputDir | Out-Host

    $zipPath = Join-Path $outputDir "loopguard-smoketest-windows-x64.zip"
    $hashPath = Join-Path $outputDir "loopguard-smoketest-windows-x64.zip.sha256"

    Assert-True (Test-Path $zipPath) "package zip should exist"
    Assert-True (Test-Path $hashPath) "package checksum should exist"

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($zipPath)
    try {
        $entries = $zip.Entries | Select-Object -ExpandProperty FullName
        Assert-True ($entries -contains "loopguard-smoketest-windows-x64/loopguard.exe") "package should contain loopguard.exe"
        Assert-True ($entries -contains "loopguard-smoketest-windows-x64/examples/loopguard-llm.ini") "package should contain llm example config"
        Assert-True ($entries -contains "loopguard-smoketest-windows-x64/prompts/decision-strategy.md") "package should contain prompt strategy"
        Assert-True ($entries -contains "loopguard-smoketest-windows-x64/scripts/llm-decider.ps1") "package should contain llm decider script"
    } finally {
        $zip.Dispose()
    }
}

function Test-LlmDeciderMock {
    param(
        [string]$RepoRoot,
        [string]$TestRoot
    )

    $contextPath = Join-Path $TestRoot "llm-context.txt"
    $mockResponsePath = Join-Path $TestRoot "llm-mock-response.json"

    @'
reason: manual resume

[stdout] phase1
[stdout] phase2
'@ | Set-Content -Path $contextPath -Encoding UTF8

    @'
{
  "output": [
    {
      "content": [
        {
          "text": "{\"action\":\"send_input\",\"message\":\"请基于刚才的上下文继续，并先说明恢复点。\",\"rationale\":\"restart detected\"}"
        }
      ]
    }
  ]
}
'@ | Set-Content -Path $mockResponsePath -Encoding UTF8

    $result = & (Join-Path $RepoRoot "scripts\llm-decider.ps1") `
        -ContextFile $contextPath `
        -Reason "manual resume" `
        -Workdir $RepoRoot `
        -SessionName "smoke-test" `
        -StrategyFile (Join-Path $RepoRoot "prompts\decision-strategy.md") `
        -UserTemplateFile (Join-Path $RepoRoot "prompts\decision-user-template.txt") `
        -MockResponseFile $mockResponsePath

    Assert-True ($result.Trim() -eq "请基于刚才的上下文继续，并先说明恢复点。") "mocked llm decider should return the expected prompt"
}

function Test-AttachDetectionSelfTest {
    param(
        [string]$ExePath
    )

    $result = & $ExePath --self-test attach-detection 2>&1 | Out-String
    Assert-Contains $result "attach-detection self-test passed." "attach detection self-test should pass"
}

function Test-SessionResume {
    param(
        [string]$RepoRoot,
        [string]$ExePath,
        [string]$TestRoot
    )

    $sessionRoot = Join-Path $TestRoot "sessions"
    $logDir = Join-Path $TestRoot "logs-session"
    $configPath = Join-Path $TestRoot "session-smoke.ini"

    @"
[agent]
mode = spawn
command_line = powershell -NoProfile -Command `"Write-Output 'phase1'; Write-Output 'phase2'`"
workdir = $RepoRoot
restart_on_exit = false
max_restarts = 0
restart_delay_seconds = 0

[watchdog]
idle_seconds = 0
attach_poll_millis = 1000
action_cooldown_seconds = 1
initial_resume_delay_seconds = 1
transcript_keep_lines = 80
decision_timeout_seconds = 5
echo_output = true
log_dir = $logDir

[actions]
continue_message = 继续
resume_template = 恢复刚才中断的任务。原因：{reason}\n最近 transcript：\n{transcript}

[patterns]
wait = should-not-match
recoverable_error = timeout

[decision]
mode = fixed
external_command =

[session]
enabled = true
name = smoke-resume
storage_dir = $sessionRoot
"@ | Set-Content -Path $configPath -Encoding UTF8

    $firstRun = & $ExePath --config $configPath 2>&1 | Out-String
    Assert-Contains $firstRun 'saved session "smoke-resume"' "first run should save a session"

    $sessionIniPath = Join-Path $sessionRoot "smoke-resume\session.ini"
    $transcriptPath = Join-Path $sessionRoot "smoke-resume\transcript.txt"
    Assert-True (Test-Path $sessionIniPath) "session.ini should exist after first run"
    Assert-True (Test-Path $transcriptPath) "transcript.txt should exist after first run"

    $resumeCommand = "powershell -NoProfile -Command `"Write-Output 'resume-ready'; `$line = [Console]::In.ReadLine(); Write-Output ('received=' + `$line)`""
    $sessionContent = Get-Content -Raw -Path $sessionIniPath
    $sessionContent = [regex]::Replace($sessionContent, '(?m)^command_line = .*$',"command_line = $resumeCommand")
    Set-Content -Path $sessionIniPath -Value $sessionContent -Encoding UTF8

    $resumeRun = & $ExePath --config $configPath --resume-session smoke-resume 2>&1 | Out-String
    Assert-Contains $resumeRun "resuming saved session in workdir=$RepoRoot" "resume run should announce the saved workdir"
    Assert-Contains $resumeRun 'received=恢复刚才中断的任务。原因：manual resume of saved session "smoke-resume"' "resume run should inject a resume prompt into the child process"
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$buildRoot = Resolve-RepoPath -Path $BuildDir
$exePath = Get-LoopGuardExe -BuildRoot $buildRoot -Configuration $Configuration

Assert-True (-not [string]::IsNullOrWhiteSpace($exePath)) "loopguard.exe should exist before smoke tests run"

$testRoot = Join-Path $buildRoot "test-output"
if (Test-Path $testRoot) {
    Remove-Item -Recurse -Force $testRoot
}
New-Item -ItemType Directory -Force -Path $testRoot | Out-Null

Test-Package -RepoRoot $repoRoot -BuildRoot $buildRoot -Configuration $Configuration -TestRoot $testRoot
Test-LlmDeciderMock -RepoRoot $repoRoot -TestRoot $testRoot
Test-AttachDetectionSelfTest -ExePath $exePath
Test-SessionResume -RepoRoot $repoRoot -ExePath $exePath -TestRoot $testRoot

Write-Host "Smoke tests passed."
