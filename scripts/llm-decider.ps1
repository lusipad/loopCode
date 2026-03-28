param(
    [Parameter(Mandatory = $true)]
    [string]$ContextFile,

    [Parameter(Mandatory = $true)]
    [string]$Reason,

    [string]$Workdir = ".",
    [string]$SessionName = "",
    [string]$StrategyFile = "prompts/decision-strategy.md",
    [string]$UserTemplateFile = "prompts/decision-user-template.txt",
    [string]$BaseUrl = "",
    [string]$Model = "",
    [string]$ApiKey = "",
    [string]$ApiKeyEnv = "OPENAI_API_KEY",
    [ValidateSet("responses")]
    [string]$ApiStyle = "responses",
    [int]$TimeoutSec = 60,
    [string]$MockResponseFile = ""
)

$ErrorActionPreference = "Stop"

function Resolve-RepoPath {
    param([string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return $Path
    }

    if ([System.IO.Path]::IsPathRooted($Path)) {
        return $Path
    }

    $repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
    return (Join-Path $repoRoot $Path)
}

function Expand-Template {
    param(
        [string]$Template,
        [hashtable]$Variables
    )

    $result = $Template
    foreach ($entry in $Variables.GetEnumerator()) {
        $result = $result.Replace("{$($entry.Key)}", [string]$entry.Value)
    }
    return $result
}

function Get-OutputText {
    param($ResponseObject)

    if ($null -ne $ResponseObject.output_text -and -not [string]::IsNullOrWhiteSpace([string]$ResponseObject.output_text)) {
        return [string]$ResponseObject.output_text
    }

    $parts = New-Object System.Collections.Generic.List[string]
    foreach ($item in @($ResponseObject.output)) {
        foreach ($content in @($item.content)) {
            if ($null -ne $content.text -and -not [string]::IsNullOrWhiteSpace([string]$content.text)) {
                $parts.Add([string]$content.text)
            } elseif ($null -ne $content.output_text -and -not [string]::IsNullOrWhiteSpace([string]$content.output_text)) {
                $parts.Add([string]$content.output_text)
            }
        }
    }

    return ($parts -join "`n").Trim()
}

function Normalize-Decision {
    param([string]$RawText)

    if ([string]::IsNullOrWhiteSpace($RawText)) {
        throw "Model returned empty output."
    }

    $decision = $RawText | ConvertFrom-Json
    if ($decision.action -eq "send_input") {
        return [string]$decision.message
    }

    return ""
}

$resolvedContextFile = Resolve-RepoPath $ContextFile
$resolvedStrategyFile = Resolve-RepoPath $StrategyFile
$resolvedUserTemplateFile = Resolve-RepoPath $UserTemplateFile

$context = ""
if (Test-Path $resolvedContextFile) {
    $context = Get-Content -Raw -Path $resolvedContextFile
}

$strategy = Get-Content -Raw -Path $resolvedStrategyFile
$userTemplate = Get-Content -Raw -Path $resolvedUserTemplateFile
$userInput = Expand-Template -Template $userTemplate -Variables @{
    reason = $Reason
    workdir = $Workdir
    session_name = $SessionName
    transcript = $context
}

if (-not [string]::IsNullOrWhiteSpace($MockResponseFile)) {
    $mockPath = Resolve-RepoPath $MockResponseFile
    $mockResponse = Get-Content -Raw -Path $mockPath | ConvertFrom-Json
    $rawOutput = Get-OutputText -ResponseObject $mockResponse
    $message = Normalize-Decision -RawText $rawOutput
    if (-not [string]::IsNullOrWhiteSpace($message)) {
        Write-Output $message
    }
    exit 0
}

if ([string]::IsNullOrWhiteSpace($BaseUrl)) {
    $BaseUrl = $env:LOOPGUARD_LLM_BASE_URL
}
if ([string]::IsNullOrWhiteSpace($BaseUrl)) {
    $BaseUrl = "https://api.openai.com/v1"
}

if ([string]::IsNullOrWhiteSpace($Model)) {
    $Model = $env:LOOPGUARD_LLM_MODEL
}
if ([string]::IsNullOrWhiteSpace($Model)) {
    throw "Missing model. Set -Model or LOOPGUARD_LLM_MODEL."
}

if ([string]::IsNullOrWhiteSpace($ApiKey)) {
    $ApiKey = [Environment]::GetEnvironmentVariable($ApiKeyEnv)
}
if ([string]::IsNullOrWhiteSpace($ApiKey)) {
    throw "Missing API key. Set -ApiKey or environment variable $ApiKeyEnv."
}

if ($ApiStyle -ne "responses") {
    throw "Unsupported ApiStyle: $ApiStyle"
}

$schema = @{
    type = "object"
    additionalProperties = $false
    properties = @{
        action = @{
            type = "string"
            enum = @("send_input", "skip")
        }
        message = @{
            type = "string"
        }
        rationale = @{
            type = "string"
        }
    }
    required = @("action", "message", "rationale")
}

$body = @{
    model = $Model
    input = @(
        @{
            role = "system"
            content = @(
                @{
                    type = "input_text"
                    text = $strategy
                }
            )
        },
        @{
            role = "user"
            content = @(
                @{
                    type = "input_text"
                    text = $userInput
                }
            )
        }
    )
    text = @{
        format = @{
            type = "json_schema"
            name = "loopguard_decision"
            strict = $true
            schema = $schema
        }
    }
}

$headers = @{
    Authorization = "Bearer $ApiKey"
    "Content-Type" = "application/json"
}

$uri = ($BaseUrl.TrimEnd('/') + "/responses")
$response = Invoke-RestMethod -Method Post -Uri $uri -Headers $headers -Body ($body | ConvertTo-Json -Depth 20) -TimeoutSec $TimeoutSec
$raw = Get-OutputText -ResponseObject $response
$message = Normalize-Decision -RawText $raw

if (-not [string]::IsNullOrWhiteSpace($message)) {
    Write-Output $message
}
