# LoopGuard

`LoopGuard` 是一个 Windows 下的 C++ supervisor，用来盯 `codex`、`claude` 这类 agent CLI。它会持续读取输出或窗口可见文本，识别“等待你确认”“网络抖动/服务异常”“长时间无输出”这几类信号，然后自动发送一条恢复消息。

这版有两种模式：

- `spawn`：由 `LoopGuard` 启动 agent 子进程，基于 stdin/stdout pipe 监控
- `attach`：你先手动打开 agent 窗口，`LoopGuard` 再按窗口标题附着，用 UI Automation 读可见文本，并用 `SendInput` 回发输入

## 现在已经有的能力

- 监督一个子进程命令，例如 `codex` 或 `claude`
- 附着到已打开的窗口，例如标题里带 `codex` 的 Windows Terminal
- 实时采集 `stdout/stderr`，同时落 `logs/loopguard.log`
- 在 attach 模式下轮询可见窗口文本
- 基于关键字匹配触发自动输入
- 长时间无输出时自动补发 `继续`
- 子进程退出后自动重启
- 重启后把最近 transcript 拼进 resume prompt
- 自动保存最近一次可恢复会话，下次可一键 `resume`
- 预留“外部决策器”接口，你可以接 PowerShell、Python 或任何大模型 API

## 目录

- `src/main.cpp`：主程序，包含 supervisor、pipe reader、INI 解析、自动触发逻辑
- `examples/loopguard.ini`：示例配置
- `examples/loopguard-attach.ini`：附着已打开窗口的示例配置
- `examples/loopguard-llm.ini`：通过大模型做恢复决策的示例配置
- `prompts/decision-strategy.md`：策略模板，可按你的恢复策略直接改
- `prompts/decision-user-template.txt`：用户侧 prompt 模板，可按需改占位符结构
- `scripts/decider-example.ps1`：外部决策器示例
- `scripts/llm-decider.ps1`：通过 OpenAI 兼容接口调用大模型的决策器

## 配置说明

核心字段在 `examples/loopguard.ini`：

- `[agent].command_line`：要启动的 agent 命令
- `[agent].mode`：`spawn` 或 `attach`
- `[agent].window_title_contains`：attach 模式下要匹配的窗口标题片段
- `[agent].window_class_contains`：attach 模式下可选的窗口类名片段
- `[agent].workdir`：子进程工作目录
- `[agent].restart_on_exit`：退出后是否自动重启
- `[watchdog].idle_seconds`：多久没输出就判定为卡住
- `[watchdog].attach_poll_millis`：attach 模式下轮询窗口的间隔
- `[watchdog].action_cooldown_seconds`：两次自动触发之间的冷却时间
- `[actions].continue_message`：普通情况下自动发送的消息
- `[actions].resume_template`：重启后自动发送的恢复模板，支持 `{reason}` 和 `{transcript}`，换行请写成 `\n`
- `[patterns].wait`：等待确认类关键字，使用 `||` 分隔
- `[patterns].recoverable_error`：可恢复异常关键字，使用 `||` 分隔
- `[decision].mode`：`fixed` 或 `external`
- `[decision].external_command`：外部决策器命令，支持 `{context_file}` 和 `{reason}` 占位符
- `[session].enabled`：是否保存可恢复会话
- `[session].name`：可选，自定义会话名；不填时默认取工作目录名
- `[session].storage_dir`：会话元数据和 transcript 的落盘目录

## 外部决策器

如果你想让工具根据上下文自动决策下一条输入，而不是只发固定的 `继续`，把：

```ini
[decision]
mode = external
external_command = powershell -ExecutionPolicy Bypass -File scripts\decider-example.ps1 -ContextFile "{context_file}" -Reason "{reason}"
```

打开即可。`LoopGuard` 会把最近 transcript 写到一个上下文文件，再调用这个命令。外部脚本只需要把“要发回 agent 的文本”打印到 stdout。

这意味着你后面可以非常容易地接：

- OpenAI 兼容接口
- Claude API
- 自己本地部署的模型
- 任意脚本规则引擎

## 大模型策略决策

如果你想让 `LoopGuard` 真的调用大模型来判断“现在该不该继续、该发什么 prompt”，直接用 [loopguard-llm.ini](D:/Repos/LoopCode/examples/loopguard-llm.ini)。

它默认会调用 [llm-decider.ps1](D:/Repos/LoopCode/scripts/llm-decider.ps1)，并读取两份你可以直接修改的模板：

- [decision-strategy.md](D:/Repos/LoopCode/prompts/decision-strategy.md)
- [decision-user-template.txt](D:/Repos/LoopCode/prompts/decision-user-template.txt)

默认通过 OpenAI 兼容接口的 `Responses API` 风格请求发送决策，环境变量最少要配：

```powershell
$env:OPENAI_API_KEY = "your-key"
$env:LOOPGUARD_LLM_MODEL = "your-model-id"
```

如果不是官方 OpenAI 地址，再额外配：

```powershell
$env:LOOPGUARD_LLM_BASE_URL = "https://your-endpoint/v1"
```

然后运行：

```powershell
.\loopguard.exe --config .\examples\loopguard-llm.ini
```

`llm-decider.ps1` 会把这些上下文传给模型：

- `reason`
- `workdir`
- `session_name`
- 最近 transcript

并要求模型返回严格 JSON，再由脚本提取出最终要发给 agent 的文本。

如果你只是想本地调模板、不想真的打 API，也可以给脚本传 `-MockResponseFile` 做离线测试。

## 构建

如果你本机有 CMake：

```powershell
cmake -S . -B build
cmake --build build --config Release
```

如果你使用 Visual Studio Developer Command Prompt，也可以直接编：

```powershell
cl /std:c++17 /EHsc /W4 src\main.cpp /Fe:loopguard.exe
```

## 打包

本地打包脚本在 `scripts/package.ps1`。它会把 `loopguard.exe`、示例配置、脚本和 `README.md` 打成 zip，并额外生成一个 SHA256 文件：

```powershell
.\scripts\package.ps1 -BuildDir build -Configuration Release -Version dev
```

默认输出到 `dist/`。

仓库里也已经带了 GitHub Actions workflow：

- push 到 `main`、PR 到 `main`、手动触发时，会在 `windows-latest` 上编译并上传打包产物为 workflow artifact
- push `v*` tag 时，会额外把 zip 和 `.sha256` 上传到 GitHub Release

## 测试

仓库里有一组 PowerShell smoke tests：

```powershell
.\tests\run-smoke-tests.ps1 -BuildDir build -Configuration Release
```

当前覆盖三条主链路：

- 打包脚本能生成 zip 和 sha256，并且包内容完整
- `llm-decider.ps1` 能用 mock 响应正确提取模型决策
- session 保存与 `--resume-session` 恢复链路可用

GitHub Actions 也会在打包前先跑这组 smoke tests，并上传 `build/test-output/` 作为测试工件。

## 运行

默认读取 `examples/loopguard.ini`：

```powershell
.\loopguard.exe
```

或者指定配置：

```powershell
.\loopguard.exe --config .\examples\loopguard.ini
```

如果你想直接恢复上一次保存的工作目录和上下文：

```powershell
.\loopguard.exe --resume-last
```

如果你配置了自定义会话名，也可以按名字恢复：

```powershell
.\loopguard.exe --resume-session my-project
```

保存的会话默认在 `sessions/` 下，里面会记住：

- 上次工作的目录
- 启动 agent 用的命令
- 原始配置文件路径
- 最近一段 transcript

这样下次恢复时，`LoopGuard` 会在原目录重新启动 agent，并自动发送一段 resume prompt。

如果你是“先自己打开 `codex/claude` 再让工具接管”，用 attach 示例：

1. 先手动打开 `codex` 或 `claude`
2. 确认目标窗口里光标确实停在 agent 的输入位置
3. 修改 `examples\loopguard-attach.ini` 里的 `window_title_contains`
4. 运行：

```powershell
.\loopguard.exe --config .\examples\loopguard-attach.ini
```

## 适合你的下一步

如果你真要长期拿它盯 `codex/claude`，建议下一步继续做三件事：

1. 把关键字匹配升级成状态机，区分“正常等待用户确认”和“真的卡死”
2. 给外部决策器接一个正式的大模型 API，把 transcript 摘要后再判断要不要 `继续`
3. 把 attach 模式再升级成 ConPTY 托管模式，这样对交互式终端工具更稳

## 当前限制

- `spawn` 模式当前是 pipe 方案，不是 PTY 方案
- `attach` 模式依赖窗口标题匹配、UI Automation 和前台输入焦点
- `attach` 模式把文本发到当前光标位置，所以如果你把焦点切到别的输入框，发送位置也会偏
- 一键 resume 当前主要面向 `spawn` 模式，`attach` 模式默认不保存可恢复会话
- 关键字匹配是 substring，不是正则
- 没有做 GUI，只是命令行工具
