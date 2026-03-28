param(
    [string]$ContextFile,
    [string]$Reason
)

$context = ""
if ($ContextFile -and (Test-Path $ContextFile)) {
    $context = Get-Content -Raw -Path $ContextFile
}

# 这里故意只做最小示例。
# 你后面可以把这一段换成对 OpenAI / Claude / 任何兼容接口的调用，
# 然后把模型返回的“下一条要发给 agent 的文本”直接 Write-Output 出去。

if ($Reason -match "exited with code") {
    Write-Output "请根据最近上下文继续上一个中断的任务，并先说明恢复点。"
    exit 0
}

if ($context -match "approval" -or $Reason -match "wait pattern") {
    Write-Output "继续"
    exit 0
}

Write-Output "继续"
