# LoopCode

`LoopCode` 是一个 Windows 下的 C++ wrapper + supervisor，用来托管 `codex`、`claude` 这类 agent CLI。它会持续读取输出或窗口可见文本，识别“等待你确认”“网络抖动/服务异常”“长时间无输出”这几类信号，然后自动发送一条恢复消息。

当前默认路径是把自己当成 `codex` wrapper 使用：

- `loopcode --yolo` 等价于 `codex --yolo`
- 遇到可恢复异常时，先补发继续消息；再次命中后会自动发 `/new` 并带上 transcript 恢复提示
- 如果 agent 进程真的退出，仍然走已有的 restart + resume 链路

为了兼容旧用法，这个仓库暂时仍保留：

- `loopguard.exe` 作为兼容别名
- `examples/loopguard*.ini` 作为兼容配置文件名
- `LOOPGUARD_LLM_*` 作为兼容环境变量名

这版有两种模式：

- `spawn`：由 `LoopCode` 启动 agent 子进程，基于 stdin/stdout pipe 监控
- `attach`：你先手动打开 agent 窗口，`LoopCode` 再按进程树附着到对应终端窗口，用 UI Automation 读可见文本，并用 `SendInput` 回发输入

## 现在已经有的能力

- 监督一个子进程命令，例如 `codex` 或 `claude`
- 默认把 `loopcode [args...]` 透传为 `codex [args...]`
- 附着到已打开的窗口，例如已经打开的 `codex` / `claude` 所在 Windows Terminal
- 实时采集 `stdout/stderr`，同时落 `logs/loopcode.log`
- 在 attach 模式下轮询可见窗口文本
- attach 模式默认按 `codex.exe/claude.exe -> 终端进程 -> 顶层窗口` 自动找目标，不再强依赖窗口标题
- attach 模式会尽量读取已附着 agent 进程的当前工作目录，并保存成可批量恢复的目录清单
- 基于关键字匹配触发自动输入
- 长时间无输出时自动补发 `继续`
- 子进程退出后自动重启
- 重启后把最近 transcript 拼进 resume prompt
- 自动保存最近一次可恢复会话，下次可一键 `resume`
- 预留“外部决策器”接口，你可以接 PowerShell、Python 或任何大模型 API

## 目录

- `src/main.cpp`：主程序，包含 wrapper 入口、supervisor、pipe reader、INI 解析、自动触发逻辑
- `src/loopguard_attach.inl`：attach 模式和窗口检测逻辑
- `examples/loopcode.ini`：默认示例配置
- `examples/loopcode-attach.ini`：附着已打开窗口的示例配置
- `examples/loopcode-llm.ini`：通过大模型做恢复决策的示例配置
- `examples/loopguard*.ini`：兼容旧命名的同内容配置
- `prompts/decision-strategy.md`：策略模板，可按你的恢复策略直接改
- `prompts/decision-user-template.txt`：用户侧 prompt 模板，可按需改占位符结构
- `scripts/decider-example.ps1`：外部决策器示例
- `scripts/llm-decider.ps1`：通过 OpenAI 兼容接口调用大模型的决策器

## 配置说明

核心字段在 `examples/loopcode.ini`：

- `[agent].command_line`：要启动的 agent 命令
- `[agent].mode`：`spawn` 或 `attach`
- `[agent].attach_strategy`：attach 模式下的定位策略，支持 `auto`、`process_tree`、`title_match`
- `[agent].attach_window_scope`：attach 模式下接管一个还是多个窗口，支持 `single`、`all`
- `[agent].target_process_names`：attach 模式下要追踪的 agent 进程名，使用 `||` 分隔
- `[agent].terminal_process_names`：attach 模式下可作为终端祖先进程的进程名，使用 `||` 分隔
- `[agent].attach_visible_text_contains`：attach 模式下可见文本确认关键字，使用 `||` 分隔
- `[agent].window_title_contains`：旧版标题匹配附着的窗口标题片段；`attach_strategy=title_match` 或 `auto` fallback 时使用
- `[agent].window_class_contains`：旧版标题匹配附着的窗口类名片段
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

attach 模式下如果 `[session].enabled=true`，还会额外在 `sessions/attach/<name>/inventory.tsv` 保存一份“已附着窗口目录清单”。其中每条记录至少包含：

- agent 类型，例如 `codex` 或 `claude`
- 解析到的执行目录

## 外部决策器

如果你想让工具根据上下文自动决策下一条输入，而不是只发固定的 `继续`，把：

```ini
[decision]
mode = external
external_command = powershell -ExecutionPolicy Bypass -File scripts\decider-example.ps1 -ContextFile "{context_file}" -Reason "{reason}"
```

打开即可。`LoopCode` 会把最近 transcript 写到一个上下文文件，再调用这个命令。外部脚本只需要把“要发回 agent 的文本”打印到 stdout。

这意味着你后面可以非常容易地接：

- OpenAI 兼容接口
- Claude API
- 自己本地部署的模型
- 任意脚本规则引擎

## 大模型策略决策

如果你想让 `LoopCode` 真的调用大模型来判断“现在该不该继续、该发什么 prompt”，直接用 [loopcode-llm.ini](D:/Repos/LoopCode/examples/loopcode-llm.ini)。

它默认会调用 [llm-decider.ps1](D:/Repos/LoopCode/scripts/llm-decider.ps1)，并读取两份你可以直接修改的模板：

- [decision-strategy.md](D:/Repos/LoopCode/prompts/decision-strategy.md)
- [decision-user-template.txt](D:/Repos/LoopCode/prompts/decision-user-template.txt)

默认通过 OpenAI 兼容接口的 `Responses API` 风格请求发送决策，环境变量最少要配：

```powershell
$env:OPENAI_API_KEY = "your-key"
$env:LOOPCODE_LLM_MODEL = "your-model-id"
```

如果不是官方 OpenAI 地址，再额外配：

```powershell
$env:LOOPCODE_LLM_BASE_URL = "https://your-endpoint/v1"
```

然后运行：

```powershell
.\loopcode.exe --config .\examples\loopcode-llm.ini
```

`llm-decider.ps1` 会把这些上下文传给模型：

- `reason`
- `workdir`
- `session_name`
- 最近 transcript

并要求模型返回严格 JSON，再由脚本提取出最终要发给 agent 的文本。

注意：脚本现在优先使用 `LOOPCODE_LLM_*`，同时继续兼容旧的 `LOOPGUARD_LLM_*`。

如果你只是想本地调模板、不想真的打 API，也可以给脚本传 `-MockResponseFile` 做离线测试。

## 构建

如果你本机有 CMake：

```powershell
cmake -S . -B build
cmake --build build --config Release
```

如果你使用 Visual Studio Developer Command Prompt，也可以直接编：

```powershell
cl /utf-8 /std:c++17 /EHsc /W4 src\main.cpp /Fe:loopcode.exe
```

## 打包

本地打包脚本在 `scripts/package.ps1`。它会把 `loopcode.exe`、兼容别名 `loopguard.exe`、示例配置、脚本和 `README.md` 打成 zip，并额外生成一个 SHA256 文件：

```powershell
.\scripts\package.ps1 -BuildDir build -Configuration Release -Version dev
```

默认输出到 `dist/`。

仓库里也已经带了 GitHub Actions workflow：

- push 到 `main`、PR 到 `main`、手动触发时，会在 `windows-latest` 上编译并上传打包产物为 workflow artifact
- 如果配置了 Azure Trusted Signing，非 PR 流水线会先对 `loopcode.exe` / `loopguard.exe` 做签名，再上传 artifact 和 release 资产
- push `v*` tag 时，会额外把 zip、`.sha256` 和裸 exe 上传到 GitHub Release；如果 tag release 没配签名参数，workflow 会直接失败，避免发布未签名资产

如果你要启用 GitHub Actions 里的签名，需要先在仓库里配置：

- repository secrets: `AZURE_TENANT_ID`、`AZURE_CLIENT_ID`
- repository variables: `TRUSTED_SIGNING_ENDPOINT`、`TRUSTED_SIGNING_ACCOUNT_NAME`、`TRUSTED_SIGNING_CERTIFICATE_PROFILE_NAME`

这套 workflow 按微软官方推荐方式走 OIDC：先用 `azure/login` 登录，再让 `azure/trusted-signing-action` 通过 `AzureCliCredential` 完成签名，不在 GitHub 里保存 client secret。

Azure 侧至少要准备好：

- 一个启用了 federated credential 的 Microsoft Entra app registration，并允许当前仓库的 GitHub Actions 通过 OIDC 登录
- 给这个 app registration 授予目标 Trusted Signing account 上对应 certificate profile 的 `Trusted Signing Certificate Profile Signer` 角色

## 测试

仓库里有一组 PowerShell smoke tests：

```powershell
.\tests\run-smoke-tests.ps1 -BuildDir build -Configuration Release
```

当前覆盖三条主链路：

- 打包脚本能生成 zip 和 sha256，并且包内容完整
- `llm-decider.ps1` 能用 mock 响应正确提取模型决策
- attach 进程树识别和可见文本确认有一条内置 self-test
- session 保存与 `--resume-session` 恢复链路可用

GitHub Actions 也会在打包前先跑这组 smoke tests，并上传 `build/test-output/` 作为测试工件。

## 运行

默认直接运行 `loopcode.exe` 会像 `codex` 一样工作：

```powershell
.\loopcode.exe --yolo
```

如果你想进入 attach 交互菜单，需要显式带 `--menu`：

```powershell
.\loopcode.exe --menu
```

菜单里支持：

- 附加全部
- 附加指定
- 恢复全部
- 恢复指定

如果你还是想走原来的参数方式，也可以显式指定配置：

```powershell
.\loopcode.exe --config .\examples\loopcode.ini
```

```powershell
.\loopcode.exe --menu --config .\examples\loopcode-attach.ini
```

如果你想直接恢复上一次保存的工作目录和上下文：

```powershell
.\loopcode.exe --resume-last
```

如果你配置了自定义会话名，也可以按名字恢复：

```powershell
.\loopcode.exe --resume-session my-project
```

保存的会话默认在 `sessions/` 下，里面会记住：

- 上次工作的目录
- 启动 agent 用的命令
- 原始配置文件路径
- 最近一段 transcript

这样下次恢复时，`LoopCode` 会在原目录重新启动 agent，并自动发送一段 resume prompt。

如果你是“先自己打开 `codex/claude` 再让工具接管”，用 attach 示例：

1. 先手动打开 `codex` 或 `claude`
2. 如果你同时开了多个终端窗口，先把你想接管的那个切到前台
3. 确认目标窗口里当前可见内容确实是 agent 对话，而不是别的 tab / shell
4. 按需修改 `examples\loopcode-attach.ini` 里的 `target_process_names`、`terminal_process_names` 或 `attach_visible_text_contains`
5. 运行：

```powershell
.\loopcode.exe --config .\examples\loopcode-attach.ini
```

默认 attach 示例会优先按进程树找已经运行的 `codex.exe` / `claude.exe`，再映射到对应的终端窗口，并且会同时监督所有匹配窗口。

如果你想把这些已打开的窗口目录记下来，留到下次一键全部恢复，保持：

```ini
[session]
enabled = true
```

即可。`LoopCode` 会在 attach 轮询时持续更新目录清单。

下次直接批量恢复全部已记录窗口：

```powershell
.\loopcode.exe --config .\examples\loopcode-attach.ini --resume-all-attached
```

这条命令会：

1. 读取上次保存的 attach 目录清单
2. 对 `codex` 执行 `codex resume --last`
3. 对 `claude` 执行 `claude --continue`
4. 用 Windows Terminal 按对应目录重新拉起
5. 然后继续进入 attach 监控

如果你想兼容旧方式，也可以改成：

```ini
[agent]
attach_window_scope = single
attach_strategy = title_match
window_title_contains = codex
```

## 适合你的下一步

如果你真要长期拿它盯 `codex/claude`，建议下一步继续做三件事：

1. 把关键字匹配升级成状态机，区分“正常等待用户确认”和“真的卡死”
2. 给外部决策器接一个正式的大模型 API，把 transcript 摘要后再判断要不要 `继续`
3. 把 attach 模式再升级成 ConPTY 托管模式，这样对交互式终端工具更稳

## 当前限制

- `spawn` 模式当前是 pipe 方案，不是 PTY 方案
- `attach` 模式现在优先依赖进程树、UI Automation 和前台输入焦点；旧版标题匹配仍可作为 fallback
- 如果同一个 Windows Terminal 窗口里有多个 tab，`LoopCode` 只能看到当前可见 tab 的文本，所以 idle 自动恢复会先做一次可见文本确认
- `attach` 模式把文本发到当前光标位置，所以如果你把焦点切到别的输入框，发送位置也会偏
- attach 模式的批量 resume 依赖能成功读取目标进程的执行目录；如果系统权限或进程状态阻止读取，就不会写入该条记录
- 关键字匹配是 substring，不是正则
- 没有做 GUI，只是命令行工具
