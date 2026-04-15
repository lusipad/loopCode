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

function Get-LoopCodeExe {
    param(
        [string]$BuildRoot,
        [string]$Configuration
    )

    $candidates = @(
        (Join-Path (Join-Path $BuildRoot $Configuration) "loopcode.exe"),
        (Join-Path $BuildRoot "loopcode.exe"),
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

    $zipPath = Join-Path $outputDir "loopcode-smoketest-windows-x64.zip"
    $hashPath = Join-Path $outputDir "loopcode-smoketest-windows-x64.zip.sha256"

    Assert-True (Test-Path $zipPath) "package zip should exist"
    Assert-True (Test-Path $hashPath) "package checksum should exist"

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($zipPath)
    try {
        $entries = $zip.Entries | Select-Object -ExpandProperty FullName
        Assert-True ($entries -contains "loopcode-smoketest-windows-x64\loopcode.exe") "package should contain loopcode.exe"
        Assert-True ($entries -contains "loopcode-smoketest-windows-x64\loopguard.exe") "package should contain loopguard.exe alias"
        Assert-True ($entries -contains "loopcode-smoketest-windows-x64\examples\loopguard-llm.ini") "package should contain llm example config"
        Assert-True ($entries -contains "loopcode-smoketest-windows-x64\prompts\decision-strategy.md") "package should contain prompt strategy"
        Assert-True ($entries -contains "loopcode-smoketest-windows-x64\scripts\llm-decider.ps1") "package should contain llm decider script"
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
          "text": "{\"action\":\"send_input\",\"message\":\"Resume from the last checkpoint and explain where you are picking up.\",\"rationale\":\"restart detected\"}"
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

    Assert-True ($result.Trim() -eq "Resume from the last checkpoint and explain where you are picking up.") "mocked llm decider should return the expected prompt"
}

function Test-AttachDetectionSelfTest {
    param(
        [string]$ExePath
    )

    $result = & $ExePath --self-test attach-detection 2>&1 | Out-String
    Assert-Contains $result "attach-detection self-test passed." "attach detection self-test should pass"
}

function Test-WrapperPassthrough {
    param(
        [string]$ExePath,
        [string]$RepoRoot,
        [string]$TestRoot
    )

    $mockCodexScript = Join-Path $TestRoot "mock-codex.ps1"
    @'
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Args
)

Write-Output ("args=" + ($Args -join "|"))
'@ | Set-Content -Path $mockCodexScript -Encoding UTF8

    $configPath = Join-Path $TestRoot "wrapper-smoke.ini"
    $logDir = Join-Path $TestRoot "logs-wrapper"

    @"
[agent]
mode = spawn
command_line = codex
workdir = $RepoRoot
restart_on_exit = false
max_restarts = 0
restart_delay_seconds = 0

[watchdog]
idle_seconds = 0
attach_poll_millis = 1000
action_cooldown_seconds = 1
initial_resume_delay_seconds = 0
transcript_keep_lines = 40
decision_timeout_seconds = 5
echo_output = true
log_dir = $logDir

[actions]
continue_message = continue
resume_template = resume

[patterns]
wait = should-not-match
recoverable_error = should-not-match

[decision]
mode = fixed
external_command =

[session]
enabled = false
name =
storage_dir = $TestRoot\sessions-wrapper
"@ | Set-Content -Path $configPath -Encoding UTF8

    $originalPath = $env:PATH
    $originalCodexCommand = $env:LOOPCODE_CODEX_COMMAND
    try {
        $env:LOOPCODE_CODEX_COMMAND = "powershell -NoProfile -ExecutionPolicy Bypass -File `"$mockCodexScript`""
        $result = & $ExePath --config $configPath --yolo "project-x" 2>&1 | Out-String
    } finally {
        $env:PATH = $originalPath
        $env:LOOPCODE_CODEX_COMMAND = $originalCodexCommand
    }

    Assert-Contains $result "args=--yolo|project-x" "wrapper mode should pass unknown args through to codex"
}

function Test-RecoverableErrorEscalation {
    param(
        [string]$ExePath,
        [string]$RepoRoot,
        [string]$TestRoot
    )

    $scriptPath = Join-Path $TestRoot "recoverable-escalation.ps1"
    @'
Write-Output "network"
$line1 = [Console]::In.ReadLine()
Write-Output ("received1=" + $line1)
Write-Output "network"
$line2 = [Console]::In.ReadLine()
Write-Output ("received2=" + $line2)
$line3 = [Console]::In.ReadLine()
Write-Output ("received3=" + $line3)
'@ | Set-Content -Path $scriptPath -Encoding UTF8

    $configPath = Join-Path $TestRoot "recoverable-escalation.ini"
    $logDir = Join-Path $TestRoot "logs-recoverable-escalation"

    @"
[agent]
mode = spawn
command_line = powershell -NoProfile -ExecutionPolicy Bypass -File "$scriptPath"
workdir = $RepoRoot
restart_on_exit = false
max_restarts = 0
restart_delay_seconds = 0

[watchdog]
idle_seconds = 0
attach_poll_millis = 1000
action_cooldown_seconds = 1
initial_resume_delay_seconds = 0
transcript_keep_lines = 80
decision_timeout_seconds = 5
echo_output = true
log_dir = $logDir

[actions]
continue_message = continue
resume_template = Resume interrupted task. Reason: {reason}
new_session_message = /new
new_session_resume_template = Fresh session resume. Reason: {reason}

[patterns]
wait = should-not-match
recoverable_error = network

[recovery]
recoverable_error_new_session_threshold = 2

[decision]
mode = fixed
external_command =

[session]
enabled = false
name =
storage_dir = $TestRoot\sessions-recoverable-escalation
"@ | Set-Content -Path $configPath -Encoding UTF8

    $result = & $ExePath --config $configPath 2>&1 | Out-String

    Assert-Contains $result "received1=continue" "first recoverable error should send continue"
    Assert-Contains $result "received2=/new" "second recoverable error should send /new"
    Assert-Contains $result "received3=Fresh session resume. Reason: matched recoverable_error pattern: network" "second recoverable error should resume in a fresh session"
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
continue_message = continue
resume_template = Resume interrupted task. Reason: {reason}\nRecent transcript:\n{transcript}

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
    Assert-Contains $resumeRun 'received=Resume interrupted task. Reason: manual resume of saved session "smoke-resume"' "resume run should inject a resume prompt into the child process"
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$buildRoot = Resolve-RepoPath -Path $BuildDir
$exePath = Get-LoopCodeExe -BuildRoot $buildRoot -Configuration $Configuration

Assert-True (-not [string]::IsNullOrWhiteSpace($exePath)) "loopcode.exe or loopguard.exe should exist before smoke tests run"

$testRoot = Join-Path $buildRoot "test-output"
if (Test-Path $testRoot) {
    Remove-Item -Recurse -Force $testRoot
}
New-Item -ItemType Directory -Force -Path $testRoot | Out-Null

Test-Package -RepoRoot $repoRoot -BuildRoot $buildRoot -Configuration $Configuration -TestRoot $testRoot
Test-LlmDeciderMock -RepoRoot $repoRoot -TestRoot $testRoot
Test-AttachDetectionSelfTest -ExePath $exePath
Test-WrapperPassthrough -ExePath $exePath -RepoRoot $repoRoot -TestRoot $testRoot
Test-RecoverableErrorEscalation -ExePath $exePath -RepoRoot $repoRoot -TestRoot $testRoot
Test-SessionResume -RepoRoot $repoRoot -ExePath $exePath -TestRoot $testRoot

Write-Host "Smoke tests passed."
