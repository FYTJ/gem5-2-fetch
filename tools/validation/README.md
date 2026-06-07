# 四层验证 Harness

`tools/validation/run-validation` 是统一入口。默认以 dry-run 模式输出四层 gate 的远程命令和结构化结果，不会在本机启动 GEM5、XiangShan 或 Wally。

```bash
tools/validation/run-validation --layer all --dry-run
tools/validation/run-validation --layer l1 --target riscv-tests-build --no-dry-run
tools/validation/collect-validation-results --output build/validation/final-summary.json --markdown build/validation/final-summary.md
```

路径默认映射为：本机 `/Users/zhuyanbo/...` 对应远程 `linux` 的 `/mnt/hgfs/zhuyanbo/...`；`eda-00` 默认使用 `/nfs_global/I/qimeng2/zhuyanbo/module-deletion-test`。如环境变化，可用 `VALIDATION_LINUX_ROOT`、`VALIDATION_EDA_ROOT` 或命令行 `--linux-root`、`--eda-root` 覆盖。

结果字段固定包含 `layer`、`target`、`workload`、`host`、`cwd`、`command`、`return_code`、`status`、`log_path`、`stats_path`、`exit_reason`、`difftest`、`simInsts`、`instrCnt`、`cycles`、`IPC`、`host_time`。状态枚举为 `pass`、`fail`、`timeout`、`unsupported`、`blocked_missing_artifact`、`blocked_env`。

`collect-validation-results` 只读取已有 JSON 结果并生成总表，用于 Task 006 和 Phase C 汇总。它不会重跑仿真，也不会把 L3 timeout、Wally benchmark 环境阻塞或 SPEC checkpoint 缺失改写为通过结论。汇总中会保留 `harness_status`、`detail_status` 和 `effective_status`；总表按 `effective_status` 计数，若顶层状态与细节状态冲突，会写入 `inconsistent_statuses`。当前汇总器优先读取本轮 `run-validation` 输出目录，并会把直接运行的 L3 detail JSON 作为 gate 纳入统计，避免旧 task 产物覆盖当前结果。XiangShan L3 的 direct detail 优先级是 `build/validation/l3-xiangshan/eda00-smoke/results.json` 高于旧的 `local-guard`，用于保存通过 `paper-RAG/tools/ssh-eda00-fast` 实际尝试 `eda-00` 登录后的最新证据。

`supported-cross-77` 是当前默认轻量全系统 RV64 suite。它从 `build/validation/l3-gem5/isa/results.json` 中抽取 GEM5 `status=pass` 的 79 项，默认排除 5 个 GEM5 known unsupported 项，并额外排除 Wally/XiangShan 完整 direct-run 中已知不通过的 `rv64mi-p-scall` 与 `rv64mi-p-pmpaddr`，最终形成 77 项。GEM5 summary 优先读取 `build/validation/l3-gem5/supported-cross-77/results.json`；Wally summary 优先读取 `build/validation/l3-wally/isa/results.json`；XiangShan summary 优先读取 `build/validation/l3-xiangshan/isa/results.json`，只有完整 suite 不存在时才回退到 `eda00-smoke` / `local-guard`。

GEM5 enable-difftest 必须按宿主和 ref so 分开保存。在 `eda-00` 上使用历史 ready-to-run NEMU 的结果路径是 `build/validation/l3-gem5-enable-difftest-eda00/supported-cross-77/results.json`，汇总目标名为 `gem5-isa-difftest-eda00`。在默认远程 `linux aarch64` 上使用本机源码构建的 Spike ref so 时，结果路径为 `build/validation/l3-gem5-spikeproxy-fix-linux-aarch64/supported-cross-77/results.json`，汇总目标名为 `gem5-isa-difftest-linux-aarch64-spike`。在默认远程 `linux aarch64` 上使用本机源码构建并完成 GEM5 适配的 NEMU ref so 时，当前通过结果路径为 `build/validation/linux-aarch64-nemu-difftest/supported-cross-77/final-nemu-fix5/results.json`，汇总目标名为 `gem5-isa-difftest-linux-aarch64-nemu`；逐项时间统计在同目录 `timing.json` / `timing.md`。这些结果不会覆盖旧的非 difftest `gem5-isa-first-layer`，用于明确区分“GEM5 raw-cpt L3可运行”和“GEM5+ref so difftest可运行”这两种证明边界。

SPEC2006 checkpoint 以ignored cache和manifest为入口。`tools/validation/spec2006-checkpoint-manifest`从`.cache/spec2006-ckpts/chp/`生成`build/validation/spec2006-checkpoints/manifest.json`，其中`canary_id`用于选择最小checkpoint，并记录`gcpt_layout`、`gcpt_restorer`、`requires_rvh`和`requires_h_aware_ref`。当前`chp`批次会被识别为`embedded-rvh`，GEM5 runner在`auto`策略下使用checkpoint内嵌restorer，命令中加入`--restore-rvh-cpt --enable-h-gcpt --gcpt-restorer=None`，不会用标准NEMU `gcpt_restore`覆盖镜像头部；如果强制传入外部标准restorer，runner会在启动前阻断。GEM5侧使用`tools/validation/run-l3-gem5 --stage spec2006 --spec2006-canary --difftest-mode enabled --difftest-ref-so <ref-so> --gcpt-restorer <path|auto|none>`，默认`--spec2006-run-mode bounded`会带manifest中的小`--maxinsts`并写入`build/validation/l3-gem5/spec2006/results.json`，汇总目标名为`gem5-spec2006-checkpoint`。显式`--spec2006-run-mode complete`进入GEM5 checkpoint结束运行口径，默认向GEM5传入`--maxinsts=40000000`作为checkpoint样本结束边界；关闭difftest结果写入`build/validation/l3-gem5/spec2006-full-disabled/results.json`，汇总目标名为`gem5-spec2006-full-disabled`，开启difftest结果写入`build/validation/l3-gem5/spec2006-full-difftest/results.json`，汇总目标名为`gem5-spec2006-full-difftest`。只有进入checkpoint仿真后到达checkpoint结束边界才算pass；长超时、未进入checkpoint、difftest错误和OOM都保持独立非pass状态。对`requires_h_aware_ref=true`的条目，未显式提供H-aware ref且未设置`GCBH_REF_SO`时会在dry-run阶段blocked。非RVH旧布局仍可通过`auto`解析manifest、`GCB_RESTORER`和已构建的NEMU gcpt restorer产物；传入`gcpt_restore`目录时会解析到目录下的`build/gcpt.bin`。XiangShan侧使用`tools/validation/run-l3-xiangshan --stage spec2006 --spec2006-canary --diff <ref-so>`，真实执行仍只能在q2`eda-00`，结果默认写入`build/validation/l3-xiangshan/spec2006/results.json`，汇总目标名为`xiangshan-spec2006-checkpoint`。各结果都必须保留`started_at`、`ended_at`、`wall_time_seconds`、`timeout_seconds`、`stop_reason`、`killed`、`log_path`和`stats_path`或等价字段；这些耗时只记录最小ckpt验证成本，不作为GEM5与Chisel/RTL性能比较。

2-fetch 性能评估使用 `tools/validation/run-gem5-dual-metric` 作为统一双口径入口。`--metric end-to-end` 使用 restore 后完整指令预算，例如 40M 全部计入 stats；`--metric measured` 使用 `--warmup-insts-no-switch=<warmup>` 触发 GEM5 dump/reset，然后只把 reset 后最后一个 stats window 作为正式性能口径。runner 会为每次运行创建独立 run directory，保存 `command.txt`、`preflight.json`、`stdout.log`、`stderr.log`、`time.log`、`resource-monitor.jsonl`、strict checker 输出、focused stats 输出和 `results.json`。

`resource-monitor.jsonl` 每轮记录 `/proc/meminfo` 中的 `MemAvailable` / `SwapFree`，根分区与 workspace 磁盘剩余，m5out 目录增长，以及 `/usr/bin/time` / `timeout` / `gem5.opt` 进程树的 RSS/VSZ/PID。SPEC06 默认 `--trace-policy auto`，在 2-fetch on 时会自动传 `--disable-bpu-two-fetch-trace`，避免完整 checkpoint 产生超大 JSONL trace；小 workload 默认保留 trace，并继续运行 `check-bpu-two-ahead-two-taken`。`--checkpoint-kind auto` 对 CoreMark/Dhrystone 选择 raw binary，对 SPEC06 checkpoint 选择 RVH 恢复命令。

```bash
tools/validation/run-gem5-dual-metric --workload dhrystone --mode off --metric measured --warmup-insts 20000000 --measure-insts 20000000
tools/validation/run-gem5-dual-metric --workload dhrystone --mode on --metric measured --warmup-insts 20000000 --measure-insts 20000000
tools/validation/run-gem5-dual-metric --workload spec06 --mode on --metric end-to-end --checkpoint .cache/spec2006-ckpts/chp/<checkpoint> --e2e-insts 40000000
```

```bash
tools/validation/select-rv64-supported-suite --suite supported-cross-77 --format json --summary
tools/validation/run-l3-gem5 --stage isa --suite supported-cross-77 --reuse-existing --output build/validation/l3-gem5/supported-cross-77/results.json
tools/validation/run-l3-gem5 --stage isa --suite supported-cross-77 --difftest-mode enabled --difftest-ref-so build/validation/linux-aarch64-nemu-difftest/nemu-build-final-fix5/riscv64-nemu-interpreter-so --output build/validation/linux-aarch64-nemu-difftest/supported-cross-77/final-nemu-fix5/results.json
tools/validation/run-l3-wally --stage isa --suite supported-cross-77 --output build/validation/l3-wally/isa/results.json
tools/validation/run-l3-xiangshan --stage isa --suite supported-cross-77 --output build/validation/l3-xiangshan/isa/results.json
```

运行耗时和 DUT 性能分开汇总：`host_time` 只进入 `run_time_summary`；`performance_summary.performance_valid` 只有在 correctness-pass workload 同时提供 `simInsts`、`instrCnt`、`cycles`、`IPC` 时才为 true。timeout/fail workload 的 `stats_path` 仅作为 debug artifact。

状态与性能口径回归检查：

```bash
tools/validation/check-validation-summary-consistency
```

BPU 2-ahead + 2-taken 语义 checker 独立于基础 `check-bpu-trace-schema`。基础 checker 只检查 predict/resolve/redirect/flush/train/commit 的事件结构；语义 checker 额外检查每个预测入口的 ahead window 数量、taken slot 上限、window1 派生、taken 顺序和 flush 后旧 metadata 不再训练：

```bash
tools/validation/check-bpu-two-ahead-two-taken --sample
tools/validation/check-bpu-two-ahead-two-taken --input tools/validation/fixtures/bpu-two-ahead-two-taken/valid.json
tools/validation/check-bpu-two-ahead-two-taken --input tools/validation/fixtures/bpu-two-ahead-two-taken/invalid-too-many-taken.json --expect-fail
tools/validation/check-bpu-two-ahead-two-taken --input tools/validation/fixtures/bpu-two-ahead-two-taken/invalid-window-order.json --expect-fail
tools/validation/check-bpu-two-ahead-two-taken --input tools/validation/fixtures/bpu-two-ahead-two-taken/invalid-stale-epoch-train.json --expect-fail
```

RV64 随机抽样验证入口用于从 manifest 中选择同时具备 GEM5 和 XiangShan 后端产物、且不含 trap/CSR/counter/PMP 等高风险需求标签的测试：

```bash
tools/validation/run-rv64-random-sample \
  --manifest baremetal/riscv-tests/isa/build/manifest.json \
  --seed 20260520 \
  --count 5 \
  --dry-run \
  --output build/validation/rv64-random-sample/dry-run.json
```

去掉 `--dry-run` 后会按 `--target gem5|xiangshan|both` 调用对应 L3 runner；在非 `eda-00` 主机上 XiangShan target 会保持 `blocked_env`，不会启动 emu。
