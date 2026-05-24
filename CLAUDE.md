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

### GEM5 模拟器

GEM5 的BTB/TAGE模块级单测和禁用difftest的轻量smoke仍可默认在远程 `linux` 主机运行，进入 Linux 映射路径下的 `GEM5/`。为避免误用 Linuxbrew Python 3.14 导致 `libpython3.14.so.1.0` 缺失，GEM5 构建和单测默认先限定系统 PATH：

```bash
ssh -tt linux 'bash -ic '\''
cd /mnt/hgfs/zhuyanbo/Desktop/FYTJ/Work/QiMeng/ArchCoder/dev/module-deletion-test/GEM5
export PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin
git rev-parse HEAD
git status --short -uno
scons build/RISCV/cpu/pred/btb/test/tage.test.opt --unit-test -j4
./build/RISCV/cpu/pred/btb/test/tage.test.opt
scons build/RISCV/cpu/pred/btb/test/mgsc.test.opt --unit-test -j4
./build/RISCV/cpu/pred/btb/test/mgsc.test.opt
'\'''
```

GEM5 当前已验证的 bounded smoke 使用 `kmhv3.py`、RV64UI raw binary、SimpleMemory、禁用 difftest，并限制最多 10 条指令：

```bash
ssh -tt linux 'bash -ic '\''
cd /mnt/hgfs/zhuyanbo/Desktop/FYTJ/Work/QiMeng/ArchCoder/dev/module-deletion-test/GEM5
export PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin
./build/RISCV/gem5.opt ./configs/example/kmhv3.py \
  --raw-cpt \
  --generic-rv-cpt=../baremetal/riscv-tests/isa/build/rv64ui/rv64ui-p-add/rv64ui-p-add.bin \
  --mem-type=SimpleMemory \
  --disable-difftest \
  --maxinsts=10
'\'''
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

#### q2 / eda-00历史结果

`qimeng2`登录节点只用于建立ControlMaster、同步文件和轻量检查；不要在登录节点构建或仿真。`eda-00`是x86-64运行节点，可用于历史ready-to-run NEMU so路径验证，但当前结果不通过。

q2/eda清理边界：

- 只清理`/nfs_global/I/qimeng2/zhuyanbo/module-deletion-test`下本项目产物和相关tmux/进程。
- 禁止修改`/home/I/qimeng2`、共享shell profile、全局conda、认证配置、其他用户文件和项目外目录。
- 本轮已把旧`build/validation/eda00-manifest`和`build/validation/l3-gem5-enable-difftest-eda00`移动到`build/validation/archive-before-linux-aarch64-spike-20260523-1410/`，避免与linux结果混淆。

历史`eda-00`构建环境和命令只作为旧结果解释，不是当前默认路径：

```bash
cd /nfs_global/I/qimeng2/zhuyanbo/module-deletion-test
source /nfs_global/I/qimeng2/zhuyanbo/envs/module-deletion-test-gem5-py311/bin/activate
export PATH=/nfs_global/I/qimeng2/zhuyanbo/module-deletion-test/.cache/bin:/tools/cluster-software/gcc/gcc-9.3.0/bin:$PATH
export CC=zyb-gcc-gem5
export CXX=zyb-g++-gem5
export BOOST_ROOT=/workspace/I/qimeng2/anaconda3
export CPATH=/workspace/I/qimeng2/anaconda3/include:/nfs_global/I/qimeng2/zhuyanbo/envs/module-deletion-test-verilator/include:${CPATH:-}
export LIBRARY_PATH=/nfs_global/I/qimeng2/zhuyanbo/envs/module-deletion-test-verilator/lib:/workspace/I/qimeng2/anaconda3/lib:${LIBRARY_PATH:-}
export LD_LIBRARY_PATH=/nfs_global/I/qimeng2/zhuyanbo/envs/module-deletion-test-verilator/lib:/workspace/I/qimeng2/anaconda3/lib:${LD_LIBRARY_PATH:-}
cd GEM5
scons build/RISCV/gem5.opt -j8
```

eda-00历史证明边界：

- GEM5 x86-64二进制曾在`eda-00`构建并`--help`通过。
- NEMU enable-difftest canary进入真实仿真后在`Start regcpy to NEMU`之后SIGSEGV；77项为`0 pass / 77 fail / 0 timeout`，所有项`return_code=-11`，总`host_time=1556.37s`，墙钟约`25分57秒`。
- Spike ready-to-run so在`eda-00`上不是可用fallback，日志包含`/lib64/libm.so.6: version 'GLIBC_2.29' not found`。
- `collect-validation-results`中历史eda-00结果的独立target是`gem5-isa-difftest-eda00`。

### Chisel / RTL

XiangShan Chisel 编译、Verilog 生成、Verilator emu 和 difftest 默认在 q2 的 `eda-00` 运行节点执行。`qimeng2` 登录节点只允许建连、同步和轻量检查，禁止运行编译、测试、仿真或长任务。

进入 q2 后先确认在运行节点和项目路径：

```bash
ssh-qimeng2-open
ssh-eda00-fast
cd /nfs_global/I/qimeng2/zhuyanbo/module-deletion-test/XiangShan
test "$(hostname)" = "eda-00"
git rev-parse HEAD
git status --short -uno
```

当前 q2 用户态工具链环境：

```bash
export HOME=/nfs_global/I/qimeng2/zhuyanbo
export XDG_CACHE_HOME=/nfs_global/I/qimeng2/zhuyanbo/.cache
export COURSIER_CACHE=/nfs_global/I/qimeng2/zhuyanbo/.cache/coursier
export MILL_DOWNLOAD_PATH=/nfs_global/I/qimeng2/zhuyanbo/.cache/mill/download
export JAVA_HOME=/nfs_global/I/qimeng2/zhuyanbo/envs/module-deletion-test-chisel/jdk17
export VERILATOR_ENV=/nfs_global/I/qimeng2/zhuyanbo/envs/module-deletion-test-verilator
export FIRTOOL_ENV=/nfs_global/I/qimeng2/zhuyanbo/envs/module-deletion-test-firtool-1.135.0
export CHISEL_FIRTOOL_PATH=$FIRTOOL_ENV/bin
export FIRTOOL=$FIRTOOL_ENV/bin/firtool
export PATH=$JAVA_HOME/bin:/nfs_global/I/qimeng2/zhuyanbo/envs/module-deletion-test-chisel/bin:$VERILATOR_ENV/bin:$FIRTOOL_ENV/bin:$PATH
export CC=$VERILATOR_ENV/bin/x86_64-conda-linux-gnu-gcc
export CXX=$VERILATOR_ENV/bin/x86_64-conda-linux-gnu-g++
export CPATH=$VERILATOR_ENV/include:/workspace/I/qimeng2/anaconda3/include:$CPATH
export LIBRARY_PATH=$VERILATOR_ENV/lib:/workspace/I/qimeng2/anaconda3/lib:$LIBRARY_PATH
export NEMU_HOME=/nfs_global/I/qimeng2/zhuyanbo/module-deletion-test/XiangShan/ready-to-run
export AM_HOME=/nfs_global/I/qimeng2/zhuyanbo/module-deletion-test/XiangShan/ready-to-run
```

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

## 默认远程 Linux 环境

本地机器为 macOS，主要用于编辑代码与 Codex/Claude 交互。构建、仿真、测试命令默认通过 SSH 在远程主机 `linux` 上执行。

默认命令模板：

```bash
ssh -tt linux 'bash -ic '\''<command>'\'''
```

说明：

- `-tt` 强制分配伪终端。
- `bash -ic` 加载远端交互式 bash 配置，使 PATH、alias、module 等环境配置生效。
- 未显式指定其他远端时，一律使用 `linux`。
- qiming2/qimeng2 集群不是当前仓的默认执行环境；只有用户明确要求使用该集群时，才另行引入对应连接和作业规则。

## 共享文件系统路径

Mac home 目录与 `linux` 主机共享同一存储。路径映射为：

```text
Mac:   /Users/zhuyanbo/...
Linux: /mnt/hgfs/zhuyanbo/...
```

当前仓路径映射：

```text
Mac:   /Users/zhuyanbo/Desktop/FYTJ/Work/QiMeng/ArchCoder/dev/module-deletion-test
Linux: /mnt/hgfs/zhuyanbo/Desktop/FYTJ/Work/QiMeng/ArchCoder/dev/module-deletion-test
```

因此，在 Linux 上进入当前仓的标准写法是：

```bash
ssh -tt linux 'bash -ic '\''cd /mnt/hgfs/zhuyanbo/Desktop/FYTJ/Work/QiMeng/ArchCoder/dev/module-deletion-test && pwd'\'''
```

## 连接测试

执行远程任务前，优先用以下命令确认 `linux` 可达且共享路径可见：

```bash
ssh -tt linux 'bash -ic '\''hostname; uname -m; cd /mnt/hgfs/zhuyanbo/Desktop/FYTJ/Work/QiMeng/ArchCoder/dev/module-deletion-test && pwd && test -f CLAUDE.md'\'''
```

如果连接失败，需要记录 SSH 的准确错误，不要把测试写成已通过。

## qimeng2 / RockyOS / eda-00 服务器配置

以下信息从 `paper-RAG/CLAUDE.md` 的服务器配置整合而来，并按当前仓 `module-deletion-test` 的路径和任务边界适配。除非用户明确要求使用这些服务器，否则当前仓仍默认使用上一节的 `linux` 远程主机。

### 服务器总览

| 节点 | 登录脚本 | 角色 | 当前仓用途 |
| --- | --- | --- | --- |
| `qimeng2登录节点` | `ssh-qimeng2-open` / `ssh-qimeng2-fast` / `ssh-qimeng2-close` | 中科院计算所 SCC 集群入口和跳板机；登录节点禁止运行代码 | 建立 ControlMaster、同步文件、查看共享目录、进入运行节点 |
| `rockyos` | `ssh-rockyos8-fast` / `ssh-rockyos-fast` | Rocky Linux 8 登录节点；实际主机名为 `rockyos8-login0` | RockyOS 环境检查、共享目录可见性验证、后续 RockyOS 作业入口 |
| `eda-00` | `ssh-eda00-fast` | 从 q2 登录节点跳转进入的代码运行节点 | 创建 tmux、激活 conda 环境、运行明确允许的测试和长时间任务 |

通用纪律：

- 登录节点只做连接、同步、查看和轻量环境检查；禁止在登录节点运行项目代码、测试、仿真或长时间任务。
- 真实代码运行必须进入 `eda-00` 等运行节点，并在 tmux 中执行。
- 真实 `.env`、storage state、cookie、SQLite taskdb、浏览器 profile 和明文密码不得写入仓库。
- 本人的所有代码、工作文件、缓存和运行产物默认放在 `/nfs_global/I/qimeng2/zhuyanbo/` 下。
- 当前仓在 q2 / RockyOS / eda-00 侧的默认项目目录为 `/nfs_global/I/qimeng2/zhuyanbo/module-deletion-test`。
- 远程认证配置、`authorized_keys`、共享 shell profile 和其他人的文件未经明确批准不得修改。

### qimeng2 登录节点

`qimeng2登录节点` 是 SCC 集群入口，登录账号为共享账号 `qimeng2`。当前仓只在需要远端资源、同步文件或进入运行节点时使用它。

q2 登录节点禁止运行代码；编译、测试、仿真和批处理都必须进入 `eda-00` 等运行节点后再执行。

#### 登录脚本

本机 q2 登录脚本位于 `~/.local/bin/`：

| 脚本 | 用途 | 是否触发 TOTP |
| --- | --- | --- |
| `ssh-qimeng2-open` | 建立 ControlMaster 长连接，一次 TOTP 后 4 小时内复用 | 是 |
| `ssh-qimeng2-fast` | 复用 ControlMaster 执行短命令，推荐日常使用 | 否 |
| `ssh-qimeng2-close` | 主动关闭 ControlMaster 长连接 | 否 |
| `ssh-qimeng2` | 旧版单次认证入口，仅在需要独立交互登录时使用 | 是 |
| `ssh-rockyos8-fast` / `ssh-rockyos-fast` | 复用 q2 ControlMaster 跳转 RockyOS，支持短命令和交互 shell | 否 |
| `ssh-eda00-fast` | 复用 q2 ControlMaster 跳转 `eda-00`，支持短命令和交互 shell | 否 |

推荐用法：

```bash
ssh-qimeng2-open
ssh-qimeng2-fast 'whoami; hostname'
ssh-qimeng2-fast 'cd /nfs_global/I/qimeng2/zhuyanbo && pwd'
ssh-qimeng2-close
```

底层连接信息：

- 主机：`qimeng2@62.234.203.206`，端口 `61020`
- ICT 局域网：`qimeng2@10.208.120.13`，端口 `61020`
- ControlMaster socket：`~/.ssh/cm-qimeng2.sock`
- 凭据文件：`~/.ssh/qimeng_password`、`~/.ssh/totp_secret`

TOTP 限制：每个 6 位验证码 30 秒内只能用一次。优先使用 `ssh-qimeng2-open` + `ssh-qimeng2-fast`，减少重复认证。

RockyOS 与 `eda-00` wrapper 默认复用同一个 q2 密码文件：若 `ROCKYOS8_PASSWORD` / `EDA00_PASSWORD` 未设置，会读取 `~/.ssh/qimeng_password`；只有环境变量和凭据文件都不可用且 stdin 是 TTY 时，才退回交互读取。不要把密码字面量写入仓库文件、脚本文件、日志或远端配置。

#### 文件同步

scp / rsync 走同一 socket：

```bash
SOCK=~/.ssh/cm-qimeng2.sock

rsync -avz \
  -e "ssh -o ControlPath=$SOCK -p 61020" \
  ./ \
  qimeng2@62.234.203.206:/nfs_global/I/qimeng2/zhuyanbo/module-deletion-test/
```

多行轻量命令推荐 base64 包装后通过 `ssh-qimeng2-fast` 执行，避免本地 shell 或 expect 误解释引号和分号：

```bash
B64=$(base64 < /tmp/script.sh | tr -d '\n')
ssh-qimeng2-fast "echo $B64 | base64 -d | bash"
```

登录节点只能做同步、查看和轻量环境确认；不要在 q2 登录节点运行 `pytest`、`compileall`、构建、仿真、浏览器或长时间任务。

#### 存储空间

本文中的 `qimengX` 对应当前账号 `qimeng2`，`姓名全拼` 对应 `zhuyanbo`。

服务器内可用的存储空间如下：

1. `/home/I/qimeng2`：存放配置文件，不需要操作。当前仓禁止在该路径下创建项目文件、缓存、环境或运行产物。
2. `/workspace/I/qimeng2`：存放 anaconda 环境，不需要手动操作。创建 conda 环境时使用规范化环境名，由 conda 自动写入该位置。
3. `/nfs_global/I/qimeng2/zhuyanbo`：本人工作路径，用于存放代码、项目文件、缓存、下载产物和其它工作文件。

当前仓远端项目目录默认使用：

```bash
/nfs_global/I/qimeng2/zhuyanbo/module-deletion-test
```

#### q2 使用规范

1. `/home/I/qimeng2` 下禁止存放任何项目文件，禁止修改该路径下的 `.bashrc`、`.vimrc` 等配置文件。如有需求，先找负责的学长修改。
2. 每个人的工作路径为 `/nfs_global/I/qimeng2/姓名全拼`；当前账号实际路径为 `/nfs_global/I/qimeng2/zhuyanbo`。所有代码、工作文件、缓存和运行产物都应放在该路径下。
3. 服务器没有数据备份，删除文件无法找回；删除前必须确认路径和影响范围。
4. 禁止修改或删除其他人的文件、文件夹和 conda 环境。
5. 默认登录路径为 `/home/I/qimeng2`，每次登录服务器后必须先 `cd /nfs_global/I/qimeng2/zhuyanbo`。
6. 由于多人共享一个账号，git 用户名和邮箱必须使用仓库级 local 配置，不要改全局配置：

```bash
git config --local user.name "zhuyanbo"
git config --local user.email "<your-email>"
```

7. 如果使用 VS Code，请统一使用最新版本 `1.98.0`，避免多人共享账号时不同版本互相冲突。
8. 多次执行远程命令优先复用 ControlMaster；如果必须用 `ssh-qimeng2` 单次模式，多次调用之间至少间隔 30 秒。

### rockyos

`rockyos` 是 Rocky Linux 8 登录节点的文档名称，实际可达目标名为 `rockyos8-login0`；`rockyos-login0` 在 q2 上不可解析。该节点的网络路径仍经过 q2，但日常登录必须直接使用本机 wrapper。

#### 登录脚本

本机 RockyOS wrapper 位于 `~/.local/bin/`：

| 脚本 | 用途 |
| --- | --- |
| `ssh-rockyos8-fast` | 复用 q2 ControlMaster 跳转 `rockyos8-login0`，支持交互 shell 或短命令 |
| `ssh-rockyos-fast` | 通用别名，转发到 `ssh-rockyos8-fast` |

推荐用法：

```bash
ssh-qimeng2-open
ssh-rockyos8-fast 'hostname; whoami; pwd'
ssh-rockyos8-fast
```

RockyOS wrapper 默认在本机读取 `~/.ssh/qimeng_password`，因此短命令通常不需要人工输入密码。如果需要覆盖密码，只能使用本机临时环境变量或交互读取，不落盘、不提交：

```bash
read -rsp 'rockyos8-login0 password: ' ROCKYOS8_PASSWORD; echo
export ROCKYOS8_PASSWORD
ssh-rockyos8-fast 'hostname; whoami; pwd'
unset ROCKYOS8_PASSWORD
```

密码处理规则：

- 不把 RockyOS 密码写入 `CLAUDE.md`、脚本文件、仓库文件、远端文件或日志。
- 不在远端写 `~/.ssh/authorized_keys`，不修改 SSH server 配置，不改共享 shell profile。
- 免密登录或远端认证配置变更属于服务器文件修改，未经明确批准不得执行。

#### 已验证环境事实

`rockyos8-login0` 只读探测结论：

- 主机：`RockyOS8-Login0.future.cn`
- OS：Rocky Linux 8.7，glibc 2.28，kernel `4.18.0-425.10.1.el8_7.x86_64`
- 用户：`qimeng2`
- Python：系统 `python3` 为 3.6.8，默认没有 `python`
- `conda` / `mamba` / `micromamba`：PATH 中不可见
- `uv` / `uvx` / `pip`：PATH 中不可见
- `/nfs_global/I/qimeng2/zhuyanbo` 可访问
- `/workspace/I/qimeng2/anaconda3` 在该节点不可见，因此 CentOS7 登录节点上的 conda base 不能直接照搬为 RockyOS8 默认环境

#### RockyOS 目录与操作边界

RockyOS 侧项目目录继续使用共享路径：

```bash
/nfs_global/I/qimeng2/zhuyanbo/module-deletion-test
```

允许操作：

- 轻量登录验证；
- 只读环境检查；
- 检查 `/nfs_global/I/qimeng2/zhuyanbo` 下的项目文件是否存在；
- 后续明确允许时同步代码文件到项目目录。

禁止操作：

- 不在登录节点上运行项目代码、测试、仿真或长时间任务；
- 不创建或修改 conda env，除非当前任务明确允许；
- 不安装 uv、Python 包、Playwright、Chromium、Node 或系统依赖，除非当前任务明确允许；
- 不运行本仓构建、仿真、测试、`pytest` 或 `compileall`，除非当前任务明确允许；
- 不操作 `/nfs_global/I/qimeng2/zhuyanbo/` 之外的目录。

### eda-00

`eda-00` 是从 `qimeng2登录节点` 进入的代码运行节点。当前仓需要在 q2 集群侧运行测试、构建、仿真或其它长时间任务时，优先进入该节点并使用 tmux。

#### 登录脚本

`eda-00` 一键入口通过本机独立脚本 `~/.local/bin/ssh-eda00-fast` 调用。该脚本不是 `paper-RAG/tools/ssh-eda00-fast` 的 symlink；`paper-RAG` 仓内脚本仅作为源仓留档，不是当前仓默认入口。该入口复用 q2 ControlMaster，在 q2 登录节点上跳转到 `eda-00`，支持交互 shell 和短命令模式。

推荐用法：

```bash
ssh-qimeng2-open
ssh-eda00-fast 'hostname; whoami; pwd'
ssh-eda00-fast
```

`eda-00` wrapper 默认在本机读取 `~/.ssh/qimeng_password`，因此短命令通常不需要人工输入密码。如果需要覆盖密码，只能使用本机临时环境变量或交互读取，不落盘、不提交：

```bash
read -rsp 'eda-00 password: ' EDA00_PASSWORD; echo
export EDA00_PASSWORD
ssh-eda00-fast 'hostname; whoami; cd /nfs_global/I/qimeng2/zhuyanbo && pwd'
unset EDA00_PASSWORD
```

密码处理规则：

- 不把 `eda-00` 密码写入 `CLAUDE.md`、脚本文件、仓库文件、远端文件或日志。
- 不在远端写 `~/.ssh/authorized_keys`，不修改 SSH server 配置，不改共享 shell profile。
- 免密登录或远端认证配置变更属于服务器文件修改，未经明确批准不得执行。

#### 代码运行流程

正常修改代码和运行代码的顺序如下：

1. 通过 VS Code 或其它 SSH 工具连接服务器，或在本机使用 q2 / eda 登录脚本。
2. 在共享工作路径下编写或同步代码：

```bash
cd /nfs_global/I/qimeng2/zhuyanbo
```

3. 使用 `ssh-eda00-fast` 进入 `eda-00`。
4. 在 eda 节点创建新的 tmux。tmux 名称必须带姓名首字母，避免被其他同学误操作，例如：

```bash
tmux new -s zyb-module-deletion
```

5. 激活 conda 代码运行环境。若需要创建新环境，环境名必须采用 `姓名首字母-环境名称` 格式；`zhuyanbo` 的姓名首字母为 `zyb`，例如：

```bash
conda create -n zyb-module-deletion
conda activate zyb-module-deletion
```

该命令会自动将 conda 环境存放在 `/workspace/I/qimeng2` 下，无需手动指定环境安装路径，也不要在 `/nfs_global/I/qimeng2/zhuyanbo` 下手工堆放 conda base。

6. 在 eda 节点进入项目目录并运行测试或任务：

```bash
cd /nfs_global/I/qimeng2/zhuyanbo/module-deletion-test
```

长时间任务必须在 tmux 内运行。短命令 smoke 可以直接通过 wrapper 执行，但不要把完整构建、仿真或长时间测试塞进一次性登录命令。

#### eda-00 使用规范

1. `/workspace/I/qimeng2` 存放 anaconda 环境。conda 环境命名规范为 `姓名首字母-环境名称`。例如承书尧老师创建 eda 环境应使用 `conda create -n csy-eda`；当前仓建议使用 `conda create -n zyb-module-deletion` 这类名称。
2. 长时间运行代码时必须使用 tmux。
3. tmux 名称规范与 conda 环境类似，使用 `姓名首字母-tmux名称`，例如 `zyb-module-deletion`。
4. 运行任务前确认当前路径位于 `/nfs_global/I/qimeng2/zhuyanbo` 下。
5. 禁止修改或删除其他人的文件、文件夹和 conda 环境。
6. 服务器没有数据备份，删除文件无法找回；删除前必须确认路径和影响范围。

### RockyOS Apptainer conda 环境

RockyOS 上需要 Apptainer 时，环境由 `eda-00` 创建，再在 RockyOS 上验证同一 conda prefix。不要在 q2 登录节点上创建环境，也不要把环境写入 `/home/I/qimeng2` 或系统目录。

默认环境位置：

```bash
/nfs_global/I/qimeng2/zhuyanbo/conda-envs/zyb-apptainer
```

使用边界：

- conda 创建和 Apptainer 安装应通过 `eda-00` 执行。
- RockyOS 上只执行 `apptainer --version`、`apptainer help` 和运行前置信号审计。
- conda env prefix、package cache、临时目录和 Apptainer cache/tmp 都必须位于 `/nfs_global/I/qimeng2/zhuyanbo` 下。
- 安装成功不等于容器镜像 smoke 已通过；镜像 pull / exec / 项目构建或测试需要单独验收。
- 本步骤不运行当前仓构建、仿真或测试。
- `paper-RAG` 源仓中的 Apptainer 安装和 crawler smoke 脚本属于该仓专属工具；当前仓只有在脚本被显式迁入且用户明确允许后才可调用。

`paper-RAG` 源仓中与该环境配套的脚本名如下，仅作为服务器配置留档；当前仓未提供这些脚本时不得直接调用：

```bash
tools/install_rockyos8_apptainer_conda.sh
tools/install_rockyos8_apptainer_conda.sh --apply --create-only
tools/install_rockyos8_apptainer_conda.sh --apply --verify-only
```

如果默认 `conda-forge` 连接不稳定，源仓曾使用镜像源和有界超时：

```bash
tools/install_rockyos8_apptainer_conda.sh \
  --apply --create-only \
  --channel https://mirrors.tuna.tsinghua.edu.cn/anaconda/cloud/conda-forge \
  --conda-timeout 120
```

### paper-RAG 专属 RockyOS Apptainer crawler smoke 留档

以下内容属于 `paper-RAG` 的服务器端 crawler 验证配置，整合进当前文件是为了保留完整服务器信息；它不是当前仓 `module-deletion-test` 的默认工作流。当前仓只有在相关脚本被显式迁入、任务目标确实需要、且用户明确允许时，才可参考这些命令。

推荐入口：

```bash
tools/run_rockyos8_apptainer_catalog_smoke.sh
```

默认只打印计划，不执行远端动作。真实执行必须显式加 `--apply`：

```bash
tools/run_rockyos8_apptainer_catalog_smoke.sh --apply --prepare-only
tools/run_rockyos8_apptainer_catalog_smoke.sh --apply --browser-only
tools/run_rockyos8_apptainer_catalog_smoke.sh --apply --auth-only --sync-auth-state
tools/run_rockyos8_apptainer_catalog_smoke.sh --apply --sync-auth-state
```

镜像来源：

- 默认尝试把 `docker://mcr.microsoft.com/playwright/python:v1.59.0-noble` 拉取为 `/nfs_global/I/qimeng2/zhuyanbo/.cache/paper-RAG-apptainer/images/playwright-python-v1.59.0-noble.sif`。
- 如果网络拉取不可用，优先让用户提供已放在 `/nfs_global/I/qimeng2/zhuyanbo` 下的 SIF，然后使用 `--image-path <sif>`。
- `--image-path`、cache、tmp、runtime、download dir 和 `papers/` 都必须位于 `/nfs_global/I/qimeng2/zhuyanbo` 下。

认证边界：

- `.env` 和 `acm/runtime/storage_state.json` 默认不同步。
- 只有显式 `--sync-auth-state` 才同步认证运行态，并在远端设置 `0600`。
- 脚本不得打印 secret，不得把 storage state、cookie、taskdb、下载 ZIP、debug HTML 或日志提交到主仓。

成功标准：

- `apptainer exec <image> python --version` 成功；
- 容器内 `python -m playwright --version` 成功；
- 容器内 `browser.new_page()` 和 `page.goto("about:blank")` 成功；
- ACM session 通过认证 / 机构授权门禁；
- 单个 catalog URL 完成 `--bulk-download --organize`；
- `scripts/verify_run.py --mode bulk-url` 通过；
- 远端 `papers/<venue>/<year>/<paper-title>/metadata.yml` 和 PDF 标题命名正确。

任何门禁失败都应记录为明确 blocker，例如 `blocked: apptainer-image`、`blocked: container-browser` 或 `blocked: auth/acm`；不能把 Apptainer binary 可见、浏览器进程启动或 ZIP 下载单项成功等同于端到端成功。
