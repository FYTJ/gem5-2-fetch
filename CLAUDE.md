# CLAUDE.md

## 项目边界

本仓用于围绕“降级 TAGE 为 base predictor”的模块删除实验进行代码理解、测试方案设计和后续实现验证。当前主要对象是仓内的 `GEM5/`、`XiangShan/`、`XiangShan-doc/` 以及 `dev-docs/` spec 工作流文档。

默认工作规则：

- 用户使用中文沟通；回复和项目文档默认使用中文。
- 未经明确要求，不修改 `GEM5/`、`XiangShan/`、`XiangShan-doc/` 源码。
- `dev-docs/` 是独立 spec 文档仓，不能被主仓跟踪。
- 需要运行构建、仿真、测试时，默认在远程 Linux 上执行。

## TAGE baseline 共识

后续“将 TAGE/BTBTAGE 降级为 base predictor，并比较模拟器与 RTL 性能下降”的实验，默认从以下两个完整 commit 出发：

```text
GEM5:      e6172e6a550b1767c8ca4dbb6851e6efdb674c00
XiangShan: 5123974942833f8d63672f0c132ec9787e8a650a
```

该 pair 的含义是功能 / 设计语义 baseline：两侧都包含当前已识别的 TAGE 对齐锚点和后续必要工程修复，足以作为 base-only TAGE 改造的实验起点。它不是性能已对齐 baseline；当前尚未证明两侧在同 workload、同统计口径下的 IPC、误预测率或性能 delta 已经一致。

功能对齐锚点：

- GEM5：`b813b5770a5357ad9309e4d8cb8552e572a37049`（BTBTAGE/MGSC behavior align）、`2cf3e3895f015372314a9d6f5bfe3a9c75774ec2`（useful/allocation semantics align）。
- XiangShan：`5dbef689a8d274e312aa276674ca2bd42086f949`（参数对齐）、`64e7bff7f8a7c85948d8cc26a02e2cb7d6716e83`（prediction selection 对齐）。

后续实验和文档中必须优先引用完整 hash，而不是只引用 `xs-dev`、`kunminghu-v3` 等会移动的分支名。若需要评估性能下降是否一致，必须在上述 baseline 上分别记录 baseline 与 base-only 后的同 workload、同统计项、同运行口径 delta。

## 仿真方式与命令共识

后续修改 TAGE/BTBTAGE 或比较 base-only 前后行为时，默认先使用本节命令。若命令失败，需要记录完整命令、运行节点、commit、返回码和日志路径；不要把较弱验证写成较强结论。

### RV64 77 项轻量全系统 suite

后续轻量全系统正确性验证默认使用 `supported-cross-77` suite。该 suite 以当前 `build/validation/l3-gem5/isa/results.json` 中 `status=pass` 的 79 项 GEM5 RV64 ISA L3 结果为来源，再排除已经在 CORE-V Wally 和 XiangShan Chisel/Verilator 完整 direct-run 中证明不通过的 2 项，形成当前跨 GEM5、CORE-V Wally、XiangShan 三条路径都适合作为默认 gate 的 77 项轻量功能测试集。

历史 `supported-gem5-79` 仍可作为 GEM5-supported 来源证据理解：GEM5 当前为 `79 pass / 5 unsupported / 0 fail / 0 timeout`。但后续默认项目共识不再要求 Wally/XiangShan 默认执行已知不通过的 `rv64mi-p-scall` 和 `rv64mi-p-pmpaddr`。

机器可读入口：

```bash
tools/validation/select-rv64-supported-suite --suite supported-cross-77 --format json --summary
tools/validation/select-rv64-supported-suite --format ids
tools/validation/run-l3-gem5 --stage isa --suite supported-cross-77
tools/validation/run-l3-wally --stage isa --suite supported-cross-77
tools/validation/run-l3-xiangshan --stage isa --suite supported-cross-77 --dry-run
```

当前 suite 分布：

| suite | count |
| --- | ---: |
| `rv64ui` | 52 |
| `rv64um` | 13 |
| `rv64mi` | 12 |
| total | 77 |

默认排除以下 5 项 GEM5 known unsupported 测试，不得把它们纳入 `supported-cross-77`：

- `rv64ui-p-fence_i`：当前 GEM5 raw-cpt ISA gate 不验证 `fence.i` / self-modifying-code 的 I-cache coherence。
- `rv64ui-p-ma_data`：该测试依赖 misaligned data access 完整行为，不属于当前轻量 gate 的稳定契约。
- `rv64mi-p-csr`：privileged CSR side effects 不属于当前轻量兼容性契约。
- `rv64mi-p-illegal`：privileged illegal-instruction trap flow 在当前 runner 下不能自然终止。
- `rv64mi-p-instret_overflow`：`minstret` 写入/溢出语义与当前实现不匹配。

在 GEM5 pass 的 79 项中，额外排除以下 2 项跨目标非 pass 测试：

- `rv64mi-p-scall`：CORE-V Wally 完整 suite 中 instruction limit 前未到达预期 `tohost` pass 路径；当前归类为 ecall/trap-sensitive 测试与 Wally runner/处理器行为契约不匹配的非 pass，证据为 `build/validation/l3-wally/isa/logs/rv64mi-p-scall.log`。
- `rv64mi-p-pmpaddr`：XiangShan emu/difftest 中出现 PMP 相关寄存器差异；当前归类为 XiangShan/NEMU difftest 或 PMP 语义匹配问题的非 pass，证据为 `build/validation/l3-xiangshan/isa/logs/rv64mi-p-pmpaddr.log`。

77 项测试 ID：

```text
rv64ui-p-add
rv64ui-p-addi
rv64ui-p-addiw
rv64ui-p-addw
rv64ui-p-and
rv64ui-p-andi
rv64ui-p-auipc
rv64ui-p-beq
rv64ui-p-bge
rv64ui-p-bgeu
rv64ui-p-blt
rv64ui-p-bltu
rv64ui-p-bne
rv64ui-p-jal
rv64ui-p-jalr
rv64ui-p-lb
rv64ui-p-lbu
rv64ui-p-ld
rv64ui-p-ld_st
rv64ui-p-lh
rv64ui-p-lhu
rv64ui-p-lui
rv64ui-p-lw
rv64ui-p-lwu
rv64ui-p-or
rv64ui-p-ori
rv64ui-p-sb
rv64ui-p-sd
rv64ui-p-sh
rv64ui-p-simple
rv64ui-p-sll
rv64ui-p-slli
rv64ui-p-slliw
rv64ui-p-sllw
rv64ui-p-slt
rv64ui-p-slti
rv64ui-p-sltiu
rv64ui-p-sltu
rv64ui-p-sra
rv64ui-p-srai
rv64ui-p-sraiw
rv64ui-p-sraw
rv64ui-p-srl
rv64ui-p-srli
rv64ui-p-srliw
rv64ui-p-srlw
rv64ui-p-st_ld
rv64ui-p-sub
rv64ui-p-subw
rv64ui-p-sw
rv64ui-p-xor
rv64ui-p-xori
rv64um-p-div
rv64um-p-divu
rv64um-p-divuw
rv64um-p-divw
rv64um-p-mul
rv64um-p-mulh
rv64um-p-mulhsu
rv64um-p-mulhu
rv64um-p-mulw
rv64um-p-rem
rv64um-p-remu
rv64um-p-remuw
rv64um-p-remw
rv64mi-p-breakpoint
rv64mi-p-ld-misaligned
rv64mi-p-lh-misaligned
rv64mi-p-lw-misaligned
rv64mi-p-ma_addr
rv64mi-p-ma_fetch
rv64mi-p-mcsr
rv64mi-p-sbreak
rv64mi-p-sd-misaligned
rv64mi-p-sh-misaligned
rv64mi-p-sw-misaligned
rv64mi-p-zicntr
```

推荐验证边界：

- GEM5：使用 `supported-cross-77` 证明当前 GEM5 raw-cpt L3 ISA gate 能跑通这 77 项轻量 RV64 程序；历史 79 项只作为 GEM5-supported 来源证据。
- CORE-V Wally：后续 runner 必须消费同一 77 项 ID 列表并产生 direct run 结构化结果；不能用 Wally matrix 分类替代 direct run，也不能把被排除的 `rv64mi-p-scall` 写成通过。
- XiangShan Chisel/RTL：后续 emu/difftest runner 必须在 q2 `eda-00` 消费同一 77 项 ID 列表；不能用旧的 5 项 smoke 结果替代完整 suite，也不能把被排除的 `rv64mi-p-pmpaddr` 写成通过。
- 该 suite 只证明轻量功能正确性门槛，不证明 SPEC2006、medium workload、性能 delta、IPC 对齐、TAGE/base-only 行为差异或 BPU 替换已经完成。

### SPEC2006 checkpoint canary

SPEC2006 checkpoint输入必须放在主仓ignored cache中，默认路径为`.cache/spec2006-ckpts/chp/`，并由`tools/validation/spec2006-checkpoint-manifest`生成`build/validation/spec2006-checkpoints/manifest.json`。checkpoint本体和日志不得进入git历史。

GEM5侧的最小checkpoint canary使用`tools/validation/run-l3-gem5 --stage spec2006 --spec2006-canary --difftest-mode enabled`，必须显式提供与宿主架构匹配的`--difftest-ref-so`，并通过`--gcpt-restorer <path|auto|none>`明确restorer策略。runner会先检查checkpoint布局：旧固定payload布局才允许标准NEMU`gcpt_restore`；当前`chp`批次是内嵌restorer的`embedded-rvh`布局，`auto`会解析为`none`，并生成`--restore-rvh-cpt --enable-h-gcpt --gcpt-restorer=None`。强制给这类checkpoint传标准外部restorer应被记录为启动前blocked，而不是进入长仿真。`requires_h_aware_ref=true`时，未显式提供H-aware ref且未设置`GCBH_REF_SO`也应在dry-run阶段blocked。非RVH旧布局的`auto`会优先使用manifest或`GCB_RESTORER`，也会识别`NEMU/resource/gcpt_restore/build/gcpt.bin`和本仓NEMU外部副本中的`resource/gcpt_restore/build/gcpt.bin`；传入`gcpt_restore`目录时会解析到目录下的`build/gcpt.bin`。该路径使用GCPT恢复，不使用`--raw-cpt`。默认bounded canary结果路径是`build/validation/l3-gem5/spec2006/results.json`，collector中的target为`gem5-spec2006-checkpoint`。

GEM5完整checkpoint运行必须显式传`--spec2006-run-mode complete`。该模式默认不向GEM5命令传`--maxinsts`，结果默认写入`build/validation/l3-gem5/spec2006-full/results.json`，collector中的target为`gem5-spec2006-full-checkpoint`。完整运行只有在checkpoint已恢复并进入仿真循环后，出现`m5_exit instruction encountered`、GOOD TRAP或等价自然完成marker时才算pass；长超时为timeout，指令预算停止、difftest mismatch/panic、commit stuck或未识别退出都不是pass。它用于验证完整ckpt能否自然完成，不能用bounded canary的`maxinsts`受控退出替代。

XiangShan Chisel/RTL侧的最小checkpoint canary使用`tools/validation/run-l3-xiangshan --stage spec2006 --spec2006-canary --diff <ref-so>`。本机和q2登录节点只能dry-run或返回`blocked_env`；真实emu/difftest仍只能在q2 `eda-00`运行。默认结果路径是`build/validation/l3-xiangshan/spec2006/results.json`，collector中的target为`xiangshan-spec2006-checkpoint`。

两端最终canary都必须记录`started_at`、`ended_at`、`wall_time_seconds`、`timeout_seconds`、`stop_reason`、`killed`、checkpoint id、命令行、日志路径和stats路径或等价字段。该耗时只用于记录最小完整ckpt canary的执行成本，不构成GEM5与Chisel/RTL性能比较，也不证明全量SPEC2006跑分。

### GEM5 模拟器

GEM5 的BTB/TAGE模块级单测和禁用difftest的轻量smoke仍可默认在远程 `linux` 主机运行，进入 Linux 映射路径下的 `GEM5/`。为避免误用 Linuxbrew Python 3.14 导致 `libpython3.14.so.1.0` 缺失，GEM5 构建和单测默认先限定系统 PATH：

```bash
cd /mnt/hgfs/zhuyanbo/Desktop/FYTJ/Work/QiMeng/ArchCoder/dev/module-deletion-test/GEM5
export PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin
git rev-parse HEAD
git status --short -uno
scons build/RISCV/cpu/pred/btb/test/tage.test.opt --unit-test -j4
./build/RISCV/cpu/pred/btb/test/tage.test.opt
scons build/RISCV/cpu/pred/btb/test/mgsc.test.opt --unit-test -j4
./build/RISCV/cpu/pred/btb/test/mgsc.test.opt
```

GEM5 当前已验证的 bounded smoke 使用 `kmhv3.py`、RV64UI raw binary、SimpleMemory、禁用 difftest，并限制最多 10 条指令：

```bash
cd /mnt/hgfs/zhuyanbo/Desktop/FYTJ/Work/QiMeng/ArchCoder/dev/module-deletion-test/GEM5
export PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin
./build/RISCV/gem5.opt ./configs/example/kmhv3.py \
  --raw-cpt \
  --generic-rv-cpt=../baremetal/riscv-tests/isa/build/rv64ui/rv64ui-p-add/rv64ui-p-add.bin \
  --mem-type=SimpleMemory \
  --disable-difftest \
  --maxinsts=10
```

这组 GEM5 命令的证明范围：

- `tage.test.opt` / `mgsc.test.opt` 是 BTBTAGE/MGSC 模块级快速验证，适合每次修改后先跑。
- `kmhv3.py` bounded smoke 只证明 GEM5、配置脚本、raw binary 输入和受控退出链路可用。
- `--disable-difftest` 与 `--maxinsts=10` 明确说明它不是完整程序级 pass、不是 GEM5 difftest，也不是性能对齐或性能下降 delta 证据。

### GEM5 enable-difftest

GEM5 enable-difftest必须区分宿主架构和ref so来源。默认远程`linux`是`aarch64`，不能直接复用仓内`XiangShan/ready-to-run/riscv64-nemu-interpreter-so`或`riscv64-spike-so`，因为这两个ready-to-run产物是`x86-64` shared object。若在`linux aarch64`上验证GEM5+NEMU或GEM5+Spike difftest，必须先在该宿主上从源码构建aarch64 ref so；若使用ready-to-run x86-64 NEMU so，仍只能放到x86-64宿主，例如`eda-00`，且当前历史结果并不通过。

#### linux aarch64 NEMU difftest

当前可作为后续轻量正确性gate的GEM5+NEMU路径限定为远程`linux aarch64`上的源码构建NEMU so和已适配NEMU ABI的GEM5二进制：

```text
GEM5 HEAD = 0faac096744d1787f1d5a6c3563d9ea66908df5c
NEMU HEAD = ea34daa7 difftest: complete gem5 rv64mi csr alignment
NEMU base = afcc5cdc9b3b6f4be0b7be264b0acbe307f27d44
```

关键修复边界：

- GEM5侧在`GEM5/src/cpu/difftest.{cc,hh}`和`GEM5/src/cpu/base.cc`中增加NEMU legacy regfile和guided-exec适配，不再把GEM5内部`riscv64_CPU_regfile`直接传给NEMU。
- NEMU侧在`build/external/nemu-gem5-ref-linux-aarch64/`中修复share模式SDL轮询、RV64MI所需CSR、`ebreak`/`mtval`、`jalr` bit0清除和GEM5暴露的`misa/mvendorid/marchid/mimpid`语义。
- NEMU构建必须使用保守并行度，例如`env NEMU_HOME=$NEMU make -j2`；本轮fix5构建峰值RSS约`603976 KB`，`Swaps: 0`。

最终ref so：

```text
build/validation/linux-aarch64-nemu-difftest/nemu-build-final-fix5/riscv64-nemu-interpreter-so
```

全量命令：

```bash
cd /mnt/hgfs/zhuyanbo/Desktop/FYTJ/Work/QiMeng/ArchCoder/dev/module-deletion-test
/usr/bin/time -v -o build/validation/linux-aarch64-nemu-difftest/supported-cross-77/final-nemu-fix5/supported-cross-77.time \
  /usr/bin/python3 tools/validation/run-l3-gem5 \
    --stage isa \
    --suite supported-cross-77 \
    --difftest-mode enabled \
    --difftest-ref-so build/validation/linux-aarch64-nemu-difftest/nemu-build-final-fix5/riscv64-nemu-interpreter-so \
    --per-test-timeout 60 \
    --output build/validation/linux-aarch64-nemu-difftest/supported-cross-77/final-nemu-fix5/results.json
```

最终结果和汇总口径：

- 77项结果：`build/validation/linux-aarch64-nemu-difftest/supported-cross-77/final-nemu-fix5/results.json`。
- 逐项时间：`build/validation/linux-aarch64-nemu-difftest/supported-cross-77/final-nemu-fix5/timing.json`和`timing.md`。
- `collect-validation-results`中对应独立target：`gem5-isa-difftest-linux-aarch64-nemu`。
- 结果为`77 pass / 0 fail / 0 timeout / 0 blocked / 0 unsupported`，逐项`host_time`总和`216.264s`，外层墙钟`3:36.57`，`host_time min/median/average/max = 2.548 / 2.7 / 2.809 / 4.052s`。
- 该target不会覆盖非difftest `gem5-isa-first-layer`，也不会覆盖历史`gem5-isa-difftest-eda00`或Spike对照`gem5-isa-difftest-linux-aarch64-spike`。
- 证明边界仍是`supported-cross-77`轻量功能difftest，不包含`supported-cross-77`外的`rv64mi-p-scall`、`rv64mi-p-pmpaddr`，也不证明SPEC2006、medium workload、性能delta或TAGE/base-only修改正确。

#### linux aarch64 Spike difftest

默认项目路径：

```bash
/mnt/hgfs/zhuyanbo/Desktop/FYTJ/Work/QiMeng/ArchCoder/dev/module-deletion-test
```

复查Spike历史结果时必须确认`GEM5/`处于当轮Spike路径使用的官方checkpoint干净树：

```bash
cd /mnt/hgfs/zhuyanbo/Desktop/FYTJ/Work/QiMeng/ArchCoder/dev/module-deletion-test
git -C GEM5 rev-parse HEAD
git -C GEM5 status --short
```

Spike历史路径要求：

```text
GEM5 baseline = e6172e6a550b1767c8ca4dbb6851e6efdb674c00
GEM5 current HEAD = 243e335baf4e71e56fb072ec724f83f9c6c062a2
GEM5 status = clean
```

上一轮为`eda-00` GCC9/CentOS7构建临时删除过`GEM5/src/mem/cache/prefetch/cdp.hh`中`CDP::~CDP()`对`Queued::~Queued()`的显式调用；当前项目共识不保留该源码改动。Spike历史路径只允许在官方checkpoint之上包含`243e335baf fix(difftest): initialize Spike memcpy_init`这一项修复。后续如果再次为`eda-00`构建需要兼容补丁，必须另行记录diff和证明边界，不能把补丁混入默认baseline。

Spike difftest源码和构建命令：

```bash
cd /mnt/hgfs/zhuyanbo/Desktop/FYTJ/Work/QiMeng/ArchCoder/dev/module-deletion-test
mkdir -p build/external
git clone --branch gem5-ref https://github.com/OpenXiangShan/riscv-isa-sim.git build/external/riscv-isa-sim-gem5-ref
cd build/external/riscv-isa-sim-gem5-ref/difftest
/usr/bin/time -v make -j2 CPU=XIANGSHAN CC=/usr/bin/gcc CXX=/usr/bin/g++
```

共享目录可能触发Git的`dubious ownership`保护；读取该源码仓元数据时优先使用一次性`git -c safe.directory=<abs-path>`，不要改远程全局git配置。

当前Spike so证明：

- 源码：`OpenXiangShan/riscv-isa-sim`，branch `gem5-ref`，commit `d179549fad11fc6a5f33d85803501fbf29c325ab`。
- 构建日志：`build/validation/linux-aarch64-spike-difftest/build.log`。
- 构建命令：`make -j2 CPU=XIANGSHAN CC=/usr/bin/gcc CXX=/usr/bin/g++`。
- 构建时间：`19:58.27`，峰值RSS约`512216 KB`，`Swaps: 0`。
- 本轮ref so：`build/validation/linux-aarch64-spike-difftest/riscv64-spike-so`。
- `file`：`ELF 64-bit LSB shared object, ARM aarch64`。
- `ldd`：无`not found`，无`wrong ELF class`，无旧`GLIBC_2.29 not found`类阻塞。

GEM5二进制证明：

- 二进制：`GEM5/build/RISCV/gem5.opt`。
- `file`：`ELF 64-bit LSB pie executable, ARM aarch64`。
- `ldd`：依赖解析到`/lib/aarch64-linux-gnu/`，无`not found`。
- `gem5.opt --help`：返回码`0`。
- 当前GEM5二进制已包含`SpikeProxy::memcpy_init` fallback修复。重建时应固定系统`PYTHON_CONFIG=/usr/bin/python3-config`，并注意默认`g++-12`最终链接可能因`libprotobuf.so`需要更新的`GLIBCXX/CXXABI`符号失败；本轮使用`g++-15`完成最终链接。
- 构建日志：`build/validation/linux-aarch64-gem5-spikeproxy-fix/build-system-python.log`。
- 重链接日志：`build/validation/linux-aarch64-gem5-spikeproxy-fix/relink-gxx15.log`。

canary命令：

```bash
cd /mnt/hgfs/zhuyanbo/Desktop/FYTJ/Work/QiMeng/ArchCoder/dev/module-deletion-test
tools/validation/run-l3-gem5 \
  --stage isa \
  --suite supported-cross-77 \
  --test rv64ui-p-add \
  --difftest-mode enabled \
  --difftest-ref-so build/validation/linux-aarch64-spike-difftest/riscv64-spike-so \
  --per-test-timeout 120 \
  --output build/validation/l3-gem5-spikeproxy-fix-linux-aarch64/canary/results.json
```

77项全量命令：

```bash
cd /mnt/hgfs/zhuyanbo/Desktop/FYTJ/Work/QiMeng/ArchCoder/dev/module-deletion-test
tools/validation/run-l3-gem5 \
  --stage isa \
  --suite supported-cross-77 \
  --difftest-mode enabled \
  --difftest-ref-so build/validation/linux-aarch64-spike-difftest/riscv64-spike-so \
  --per-test-timeout 120 \
  --output build/validation/l3-gem5-spikeproxy-fix-linux-aarch64/supported-cross-77/results.json
```

结果路径和汇总口径：

- canary：`build/validation/l3-gem5-spikeproxy-fix-linux-aarch64/canary/results.json`。
- 77项：`build/validation/l3-gem5-spikeproxy-fix-linux-aarch64/supported-cross-77/results.json`。
- runner日志：`build/validation/l3-gem5-spikeproxy-fix-linux-aarch64/supported-cross-77/runner.log`。
- `collect-validation-results`中对应独立target：`gem5-isa-difftest-linux-aarch64-spike`。
- 该target不会覆盖非difftest `gem5-isa-first-layer`，也不会覆盖历史`gem5-isa-difftest-eda00`。

当前linux aarch64 Spike difftest证明边界：

- canary和77项都显示`Difftest is enabled with ref so`，并且`ref_so`为新构建aarch64 Spike so。
- 未出现`cannot open shared object file`、`wrong ELF class`、`Exec format error`或`GLIBC_2.29 not found`。
- canary日志显示`spike difftest_memcpy_init not found, using difftest_memcpy instead`，随后出现`Start memcpy to NEMU`、`Start regcpy to NEMU`并以`m5_exit`通过；原始`Start memcpy`后的SIGSEGV已经修复。
- canary结果：`rv64ui-p-add`为`pass`，`return_code=0`，`host_time=1.738s`。
- 77项结果：`72 pass / 5 fail / 0 timeout / 0 blocked`，总墙钟约`128s`，`host_time sum=128.461s`，`min=1.251s`，`median=1.689s`，`average=1.668s`，`max=1.818s`。
- 5个失败项均属于`rv64mi`：`rv64mi-p-breakpoint`、`rv64mi-p-ma_fetch`、`rv64mi-p-mcsr`、`rv64mi-p-sbreak`、`rv64mi-p-zicntr`，失败模式为`difftest panic`，主要体现CSR/异常状态差异。
- 因此当前结论是：linux aarch64已经打通GEM5加载Spike ref so、首次内存/寄存器同步和77项调度链路；但全77项尚未通过，不能作为“supported-cross-77全pass”的正确性gate。若后续实验只接受全量difftest通过，还需要继续修复`rv64mi`异常/CSR状态差异。

并行度和资源规则：

- Spike构建默认使用`-j2`，除非先确认内存余量并记录证据。
- GEM5构建若需要重跑，默认使用保守并行度；若出现SSH断开、OOM、swap异常、远程重启或日志中的`Killed`，立即降到`-j1`或`-j2`并记录。
- 本轮Spike构建、GEM5重建/重链接和77项运行未观察到内存压力、swap使用、SSH断开或系统重启；GEM5重建使用`-j2`，最终链接峰值RSS约4.7GB。

#### q2 / eda-00 历史结果

历史 q2/`eda-00` 结果只用于解释旧验证边界；远程连接、清理范围和环境变量配置已迁移到 `/Users/zhuyanbo/.codex/AGENTS.md`。

eda-00 历史证明边界：

- GEM5 x86-64二进制曾在`eda-00`构建并`--help`通过。
- NEMU enable-difftest canary进入真实仿真后在`Start regcpy to NEMU`之后SIGSEGV；77项为`0 pass / 77 fail / 0 timeout`，所有项`return_code=-11`，总`host_time=1556.37s`，墙钟约`25分57秒`。
- Spike ready-to-run so在`eda-00`上不是可用fallback，日志包含`/lib64/libm.so.6: version 'GLIBC_2.29' not found`。
- `collect-validation-results`中历史eda-00结果的独立target是`gem5-isa-difftest-eda00`。

### Chisel / RTL

XiangShan Chisel 编译、Verilog 生成、Verilator emu 和 difftest 的远端连接与工具链环境配置已迁移到 `/Users/zhuyanbo/.codex/AGENTS.md`。本节仅保留本仓验证入口和证明边界；q2 登录节点不得运行编译、测试、仿真或长任务。

Chisel 轻量编译和基础 ScalaTest：

```bash
mill -i xiangshan.compile
mill -i xiangshan.test.compile
mill -i xiangshan.test.testOnly xiangshan.frontend.bpu.SaturateCounterTest
mill -i xiangshan.test.testOnly xiangshan.frontend.bpu.SignedSaturateCounterTest
```

完整 RTL/difftest smoke 使用 `TLMinimalConfig`、单核、2 个 emu 线程和 `ready-to-run/coremark-2-iteration.bin`：

```bash
test -s ready-to-run/coremark-2-iteration.bin
test -s ready-to-run/riscv64-nemu-interpreter-so
make verilog CONFIG=TLMinimalConfig NUM_CORES=1 -j8
make emu CONFIG=TLMinimalConfig NUM_CORES=1 EMU_THREADS=2 PGO_CFLAGS=-DVerilatedTraceBaseC=VerilatedVcdC -j8
timeout 7200 ./build/emu -b 0 -e 0 -i ./ready-to-run/coremark-2-iteration.bin --diff ./ready-to-run/riscv64-nemu-interpreter-so
```

这组 Chisel / RTL 命令的证明范围：

- `mill -i xiangshan.compile` 和 `xiangshan.test.compile` 证明 Chisel 编译链路可用。
- `SaturateCounterTest` / `SignedSaturateCounterTest` 是 BPU 基础计数器 smoke，不是完整 TAGE predictor 行为测试。
- `make verilog` / `make emu` / coremark difftest 证明当前 RTL 工程入口、Verilog 生成、Verilator emu 和 NEMU difftest 路径可运行；已有成功结果为 `HIT GOOD TRAP`、`instrCnt = 663687`、`cycleCnt = 483463`、`IPC = 1.372777`。
- 该 RTL/difftest 证据仍不是与 GEM5 同 workload、同统计口径的性能对齐证明；性能下降一致性必须另行固定两侧 workload、运行长度、统计项和采样口径。

## 远程执行配置

通用远程服务器连接、`linux` 路径映射、macstudio、qimeng2/q2、RockyOS 和 `eda-00` 访问配置已迁移到 `/Users/zhuyanbo/.codex/AGENTS.md`。本文件不再记录 SSH 地址、凭据文件、ControlMaster、远端目录配额或 helper 用法。

当前仓远程执行边界：

- 常规 GEM5 构建、模块级测试和禁用 difftest 的轻量 smoke 默认使用全局 `linux` 环境。
- q2/`eda-00` 只用于用户明确要求的 XiangShan Chisel/RTL、真实 emu/difftest 或长任务；q2 登录节点只做连接、同步和轻量检查。
- 任何远端运行结论必须记录实际节点、命令、返回码和关键日志；不要把 dry-run、smoke 或 bounded timeout 写成完整通过。
