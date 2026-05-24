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
