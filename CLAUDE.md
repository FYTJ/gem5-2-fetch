# CLAUDE.md

## 项目边界

本仓用于围绕“降级 TAGE 为 base predictor”的模块删除实验进行代码理解、测试方案设计和后续实现验证。当前主要对象是仓内的 `GEM5/`、`XiangShan/`、`XiangShan-doc/` 以及 `dev-docs/` spec 工作流文档。

默认工作规则：

- 用户使用中文沟通；回复和项目文档默认使用中文。
- 未经明确要求，不修改 `GEM5/`、`XiangShan/`、`XiangShan-doc/` 源码。
- `dev-docs/` 是独立 spec 文档仓，不能被主仓跟踪。
- 需要运行构建、仿真、测试时，默认在远程 Linux 上执行。

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
