# 任务卡：M3b — TLB 与维护指令

> 状态：设计就绪，待 M3a 合入后实现。依据：roadmap M3b 节 + D8 决策
> （先完全正确的 walk，再加只许更快的 TLB）、验证策略 L4 parity 层。

## 范围

- 做什么：direct-mapped、4096 项 TLB（tag 含 VMID/nG/VA/size）；TLBI
  全套编码接线（`VMALLE1IS`/`ALLE1IS` 等）；TTBR/TCR/MAIR 写与
  `SCTLR.M` 翻转 flush-all（范围失效明确不做，写进注释）。
- 不做什么：按 ASID/VA 的精确失效（D8 第二步之后再说）；TLB 性能
  调优（正确性优先，"更快"不是 M3b 的验收项）。

## 验收命令与通过标准

```sh
ctest --test-dir build/debug -R Mmu          # 含 M3a 用例，全量回归
ctest --test-dir build/debug -R Tlb          # 新增 parity 用例
make test && make asan
```

- 通过标准：**TLB 服务结果 == walk 服务结果**（随机扫描 parity）；
  映射→改表→访问三步测试证明旧映射立即失效。

## 新增测试清单

| 层/风格 | 文件 | 钉住的行为 |
| --- | --- | --- |
| 黑盒 | `tests/unit/test_mmu.cpp`（扩展） | 三步失效；各 flush 触发点（TTBR0/1、TCR、MAIR、SCTLR.M 写）逐一断言 |
| 白盒 | `tests/unit/test_mmu_internal.cpp`（扩展） | TLBI 编码→失效动作判定表；tag 构造（含 nG） |
| parity | `tests/unit/test_mmu_parity.cpp` | 固定种子伪随机访问序列，逐次比较 TLB 路径与强制 walk 路径的翻译结果与 fault 类别 |

- parity 的"强制 walk"通道：mmu 模块需在 internal.h 暴露
  `mmu_internal_walk_only` 一类的入口（M3a 时预留），或测试经 seam
  旁路 TLB——实现时二选一，写进 `mmu_internal.h` 注释。

## Oracle 来源

- [x] 无 TLB 路径本身就是 oracle（D8 决策的直接产物）
- [x] TLBI 编码：ARM ARM + llvm-mc 语料（随实现扩展，进金标准）

## 不变量复核

- [ ] 零分配步进不变量扩展到 TLB 查询路径
- [ ] flush 是全有或全无：被拒绝/未触发的维护操作不留残余状态
- [ ] 随机扫描固定种子，失败可复现

## 风险与回退

- "TLB 与 walk 结果不同但更快"的任何优化都是 bug 不是权衡——parity
  测试在 PR 里必须全绿后才能谈性能。
- flush-all 实现太窄（漏 TTBR1 侧）：三步测试对 TTBR0/1 各跑一遍。
