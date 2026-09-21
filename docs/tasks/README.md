# 串行实施与新 agent 交接

## 使用方式与范围

本目录是后续工作的任务书，适用于具备本地文件、命令行和必要人工协作能力的任意 agent。
不要求 Codex、GPT-5.6 Luna、子 agent、MCP 或原私有仓库的 skill。没有 GUI 自动化能力时，准备精确操作步骤，由所有者完成 GUI 操作并交回原始产物。
全部工作在当前 LumaBough 中完成；out 下的冻结副本、解压测试目录不是另建开发仓库。
不要修改 MiniEngine、WorldTreeEngine，不导入旧 Git 历史。不要并行开发或同时进行计时、录屏、Capture、构建。

执行顺序：[P 性能](P-performance.md) → [D Capture/视频](D-capture-video.md) → [E 候选终审](E-candidate-review.md) → [F 发布](F-publication.md)。
当前仅 P 是下一项。所有者逐阶段指派；完成后交付结果和下一阶段启动说明，不自动开始未指派阶段。

## 当前事实与复核入口

截至此次交接修订：
- B 已完成；C 六次连续 Demo 彩排通过，历史 25 次性能数值复算通过，但历史实际源码绑定 BLOCKED。
- 新性能 001/002 分别在第 18/22 次正式运行因前台比例失败，整组 INVALID。源码、构建、失败 raw 均封存，未启动 003。
- 采样前连续前台 1 秒、最长等待 60 秒的修复已加入；无法保证后续不失焦，原因尚未查明。
- 本地 Git 无 HEAD、无 remote；未提交、推送或公开。新 agent 必须重新检查当前状态，不把本段当实时查询。
- MIT 已选定；源码许可检查不覆盖未来实际二进制包、Capture 或视频。
- PUBLICATION-FILES 是逐文件边界；out、本机日志、机器路径、PDB、Capture 和录屏不自动获准公开。

先读根 AGENTS.md、[路线图](../ROADMAP.md)、[开发指南](../DEVELOPMENT.md)、[C 记录](../evidence/BATCH-C.md)、
[新性能记录](../evidence/NEW-PERFORMANCE.md)、[来源](../evidence/PROVENANCE.md)；随后只读当前阶段要求的文件。
具体事实优先取当前代码和可复核产物；旧记录保留原时间/身份，不倒写。

## 通用执行和交付规则

每阶段先核对依赖、工作树、可执行文件、脚本 --help、工具版本及记录路径；不要照抄不存在的 CLI。
命令行以 Windows PowerShell 5.1/Python/CMake 为基础；不要求 pwsh。
没有 GPU、PIX、RenderDoc、录屏或第二台机器时，明确具体缺口及最小人工操作，不伪造成功。
普通可逆的本地操作按阶段授权实施；本地 commit、remote 创建、push、变更可见性和发布按所有者具体授权执行。已有明确授权不重复询问。
申请批准前先备好可审阅的文件集合、diff、验证结果与将执行的具体动作。

每阶段必须交付：实际改动、实际命令及退出码、工具/环境版本、源码/EXE/输入/产物 SHA-256、成功与失败记录、未完成项、下一阶段前置条件。
记录时间和来源；NULL/缺失保持诚实，不能用导入来源提交替代当前运行提交。
原始产物放 out 的新目录；公开摘要去除机器私人信息；不得覆盖旧失败集合。
新增/修改公开候选内容逐文件审阅并更新 PUBLICATION-FILES；先列具体提案，不能直接把全盘扫描结果批准为公开集合。
读取当前 schema 和校验器后再修改 Ledger/manifest：现有 Ledger 校验固定七个 C-M9 ID，新增独立当前声明须同步 schema、检查器和契约测试，不偷换历史行。

常用本地检查（从根执行；环境缺失先按开发指南配置）：

~~~powershell
python -B tools/portfolio/validate_publication.py --stage entry
out/python/Scripts/python.exe -B -m unittest discover -s tests/tools -p "test_*.py"
out/python/Scripts/python.exe -B -m unittest discover -s tests/tools/contracts -p "test_*.py"
python -B tools/validation/check_rhi_boundary.py
~~~

文档改动只做相关检查；C++、运行路径或打包改动做相应构建/测试。测试数量以本次输出为准，不照搬旧 84/1003。
validate_publication 当前只有 entry，且输出 publication=BLOCKED；它不验证发布 readiness。
E/F 必须实现并测试对应验收机制或提供逐项可复核手工记录，不能调用不存在的 --stage candidate/published。

## 身份冻结与返工顺序

P 可用源码快照和 EXE 哈希进行本地实验，不要求先取得 commit 授权。
E 在最终包冻结前解决运行源码提交身份：先准备精确本地提交范围，取得授权后提交、重新 configure/build，记录实际运行提交。
证据记录提交、运行提交和最终发布提交可能不同；逐一绑定，不要求互相循环引用。
若 EXE 或相关运行输入变化，旧性能/Capture 只能保留为原快照证据；评估适用性，必要时先返 P、再返 D，然后继续 E。
只改文档不自动要求 GPU 重跑。不同 EXE 不得仅改 JSON 哈希冒充重新运行。

## 可复制的首次启动提示词

> 请在当前 LumaBough 仓库执行 docs/tasks/P-performance.md，先读 AGENTS.md、docs/tasks/README.md 和指定证据。
> 本任务只授权 P 的本地诊断、修复、实验、测试与文档整理。不要并行开发、另建开发仓库或启动 D/E/F。
> 不要求 Codex、任何指定模型或原仓库 skill。无法操作 GUI 时，请给我具体手动步骤和产物要求。
> 保留 lb-current-001/002 的全部失败证据；先定位失焦，不直接重启长采样；新的正式组使用新 ID、快照和预注册。
> 长时间占用前台前，先完成所有准备并与我确认时间。不要以缺省回复认定电脑空闲。
> 不提交、不推送、不公开，除非我随后明确授权具体动作。
> 完成后同步路线图、阶段记录与逐文件候选清单，明确结果、剩余阻断以及进入 D 的条件。

后续分配 D/E/F 时，把上述任务路径和授权范围替换为对应阶段；F 仍需对实际仓库、ref、文件和可见性给出具体授权。
