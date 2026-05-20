# 四层验证 Harness

`tools/validation/run-validation` 是统一入口。默认以 dry-run 模式输出四层 gate 的远程命令和结构化结果，不会在本机启动 GEM5、XiangShan 或 Wally。

```bash
tools/validation/run-validation --layer all --dry-run
tools/validation/run-validation --layer l1 --target riscv-tests-build --no-dry-run
tools/validation/collect-validation-results --output build/validation/final-summary.json --markdown build/validation/final-summary.md
```

路径默认映射为：本机 `/Users/zhuyanbo/...` 对应远程 `linux` 的 `/mnt/hgfs/zhuyanbo/...`；`eda-00` 默认使用 `/nfs_global/I/qimeng2/zhuyanbo/module-deletion-test`。如环境变化，可用 `VALIDATION_LINUX_ROOT`、`VALIDATION_EDA_ROOT` 或命令行 `--linux-root`、`--eda-root` 覆盖。

结果字段固定包含 `layer`、`target`、`workload`、`host`、`cwd`、`command`、`return_code`、`status`、`log_path`、`stats_path`、`exit_reason`、`difftest`、`simInsts`、`instrCnt`、`cycles`、`IPC`、`host_time`。状态枚举为 `pass`、`fail`、`timeout`、`unsupported`、`blocked_missing_artifact`、`blocked_env`。

`collect-validation-results` 只读取已有 JSON 结果并生成总表，用于 Task 006 和 Phase C 汇总。它不会重跑仿真，也不会把 L3 timeout、Wally benchmark 环境阻塞或 SPEC checkpoint 缺失改写为通过结论。

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
