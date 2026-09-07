# 任务卡：M3b — TLB 与维护指令

> 状态：**已填开工**（分支 `feature/m3b-tlb`，P1：本卡更新先于任何代码）。
> 依据：roadmap M3b 节 + D8 决策（先完全正确的 walk，再加只许更快的
> TLB）、验证策略 L4 parity 层。原设计稿的范围/验收/风险三节原样保留。

## 范围

- 做什么：direct-mapped、4096 项 TLB（tag 含 VMID/nG/VA/size）；TLBI
  全套编码接线（`VMALLE1IS`/`ALLE1IS` 等）；TTBR/TCR/MAIR 写与
  `SCTLR.M` 翻转 flush-all（范围失效明确不做，写进注释）。
- 不做什么：按 ASID/VA 的精确失效（D8 第二步之后再说）；TLB 性能
  调优（正确性优先，"更快"不是 M3b 的验收项）。

## 实现决策（动工前定死，防实现期腐化）

1. **挂点在 `translate()`，不在 bus wrapper**：所有翻译入口（read/write/
   fetch/validate/公开 translate）都汇聚到 translate，TLB 查询/填充/epoch
   检查全在 translate 内完成——单点接线，bus 层零改动。
2. **walk 成功路径必须带出填充材料**：M3a 的 `walk()` 成功只回 PA。TLB
   要存 (PA, level, ap, xn, pxn, nG)——hit 时权限裁决必须调用与 walk
   同一个 `oemu_mmu_internal_permits()`，DFSC 的 level 取自存下的叶子
   层级，这样 hit 与 miss 的 fault 记录按构造相等（parity 不是测出来
   的，是结构保证的，测试只是戳穿违反）。
3. **只缓存成功翻译**：一切 fault（translation/access-flag/permission/
   address-size）不进缓存，每次重新 walk。M3a 无 HAFDBS → 不存在 AF
   write-back 与缓存的竞态。
4. **flush-all 触发用控制寄存器 epoch**：translate 里比对快照
   (SCTLR,TTBR0,TTBR1,TCR,MAIR) 五元组，任一变化 → 先 flush 再查询。
   这精确实现卡面触发点（M 翻转隐含在 SCTLR 值变化里），且不需要给
   sysreg 模块开回调接缝——跨模块耦合为零。同值重写不 flush：架构上
   同值重写本就无失效语义，Linux 的正确序列总跟一条 TLBI，而 TLBI =
   我们的 flush-all。
5. **tag 语义诚实**：direct-mapped，组号 = VA[23:12]（页偏移之上的
   12 位）。tag 字段 (VMID, nG, asid, level, VA-tag)：
   - asid 取 TTBR[63:48]（存在 tag 里，epoch 已覆盖 TTBR 变化）；
   - **nG** 取叶子描述符 bit[11]，M3b 无 ASID 语义，字段进 tag 是给
     未来 TLBI-by-ASID 预留的布局钉桩，本阶段行为不受其影响——注释
     明说，不做假比较；
   - **VMID** 恒 0（无 stage-2，D3），字段保留，理由同上。
6. **EL3 直通与 M=0 直通不进缓存**：这两条是平凡恒等路径，进缓存只会
   制造 epoch 盲区（EL3 不读 SCTLR_EL1.M 的语义、掉 EL 序列），排除
   它们让不变量简单。
7. **TLBI 编码接线**：`exec_internal` 新增动作 `OEMU_EXEC_SYS_TLBI`；
   分类窗口 = M3a 里 CRn∈{8,9} ∧ op1∈{0,1,2,4} 的 NOP 窗口（CRn 9 的
   stage-2 TLBI 用 flush-all 超集兑现，比假装区分诚实）；`do_sys` 调
   **公开** API `oemu_mmu_flush_all(mmu)`（mmu 为 NULL——用户态总线——
   则 no-op）。guest 可执行性与 M3a 一致，本卡不动。
8. **观测面**：hit/miss/flush 计数进 `oemu_mmu` 结构（公共头，与
   fault 记录同列），parity 测试靠它证明"缓存真的在"（只 miss 的
   parity 是 vacuous）；`mmu_internal.h` 暴露 `oemu_mmu_internal_walk`
   （旁路 TLB 的完整翻译，parity 的 oracle 通道）与
   `oemu_mmu_internal_tlb_peek(mmu, index, entry_out)`（判定表白盒）。
9. **尺寸与栈**：entry 24 B × 4096 = 96 KiB 嵌进 `oemu_mmu`（by value，
   零分配——mmu 的既有契约"no allocation, no dispose"不因缓存破例）。
   vcpu 内嵌 mmu → `main.c`/`test_vcpu` 的栈上对象 +96 KiB，Linux 8 MiB
   栈无虞；头注释明说。

## 验收命令与通过标准

```sh
ctest --test-dir build/debug -R Tlb          # 新增：parity + 失效 + 判定表
ctest --test-dir build/debug -R Mmu          # M3a 全量回归，零变化
make test && make asan && make format-check && make tidy
```

- 通过标准：**TLB 服务结果 == walk 服务结果**（固定种子随机扫描逐次
  比较，含 status/PA/ESR/FAR 四要素）；
  映射→改表→访问三步测试证明旧映射立即失效；
  hit 计数 > 0（parity 不许靠"从没命中"蒙混）。

## 新增测试清单

| 层/风格 | 文件 | 钉住的行为 |
| --- | --- | --- |
| parity | `tests/unit/test_mmu_parity.cpp`（新建，suite `TlbParity`） | 固定种子 LCG 伪随机访问序列，逐次比较 TLB 路径与 `mmu_internal_walk`：status/PA/ESR/FAR 全等；命中发生断言 |
| 黑盒 | `tests/unit/test_mmu.cpp`（扩展） | 三步失效（TTBR0/TTBR1 两侧各一遍）；五个 flush 触发点逐一断言（epoch 后首查必 miss） |
| 白盒 | `tests/unit/test_mmu_internal.cpp`（扩展） | tag 构造与冲突驱逐（同组 4097 个 VA → 全 miss）、peek 字段、hit/miss/flush 计数判定表 |
| 白盒 | `tests/unit/test_exec_internal.cpp`（扩展） | TLBI 编码→`SYS_TLBI` 判定表（原 NOP 期望按新语义改写——刻意的行为变更，在 PR 描述里点名） |
| 端到端 | `tests/unit/test_vcpu.cpp`（扩展） | 真指令 `TLBI VMALLE1IS`（0xD5087BF0）过 `vcpu_step`：执行后旧映射立失效、flush 计数 +1；M=0 路径不缓存 |

- 拆分说明：设计稿把判定表写在 `test_mmu_internal.cpp`；实施时按模块
  纪律拆开——SYS 分类表的归属是 exec（`oemu_exec_internal_sys_action`
  所在处），TLB 内部观测归 mmu。表的内容一项不少。

## Oracle 来源

- [x] 无 TLB 路径本身就是 oracle（D8 决策的直接产物）
- [x] TLBI 编码：`test_exec_internal.cpp` 既有 `Sel()` 构造器 + ARM ARM
  D12/SYS 编码；`VMALLE1IS` = op0=1,op1=0,CRn=8,CRm=7,op2=6 → 合成字
  0xD5087BF0（`oemu_exec_internal_reencode_sys` 现场重算，不手抄）
- [x] tag/驱逐行为：由 direct-mapped 定义直接推出（组号=VA[23:12]），
  断言即规格

## 不变量复核（P4，合入前打勾）

- [x] 纯 C11；TLB 存储嵌值零分配；查询/命中路径不碰 allocator seam
- [x] 零分配步进不变量扩展到 TLB 查询路径（test_vcpu 的 tracking-allocator
  TearDown 在 TLBI e2e 用例下仍断零分配）
- [x] flush 是全有或全无：被拒绝/未触发的维护操作不留残余状态
- [x] 随机扫描固定种子，失败可复现（LCG 显式编码，不用 rand()）
- [x] `make test` / `make asan` / `make format-check` / `make tidy` 全绿
      （format-check 仅剩 k_addsub.c 的 LLVM-18/21 既有漂移，与本卡无关）
- [x] M3a 用例零改动零失败——**有记录偏差**：断言与期望值一字未改，但
  三处原地改表用例（UserPageGatesEl0ByAp / XnAndPxnGateOnly... /
  TableConstraints...）在描述符改写后补了 `tlbi()`——带 TLB 后改写活映
  射必须 break-before-make，这是架构要求的合法序列；判定表用例
  TlbiSpaceIsNoop → TlbiSpaceInvalidatesSinceM3b 是本卡的刻意行为变更。

## 风险与回退

- "TLB 与 walk 结果不同但更快"的任何优化都是 bug 不是权衡——parity
  测试在 PR 里必须全绿后才能谈性能。
- flush-all 实现太窄（漏 TTBR1 侧）：三步测试对 TTBR0/1 各跑一遍。
- epoch 比对漏寄存器 → 加字段先过 M3a 回归兜底。
- 回退方案：TLB 全部改动集中在 `mmu.c` 的 translate 前端 + exec 分类
  表一行动作；`walk()` 与 bus 层逐字不动，回退即删前端。

## 完工记录（P6：只写实际跑过的）

- 实现：`mmu.c` translate 前端（epoch 5 元组值比较 → flush-all；TBI 剥
  标签后按页基址键/值入表；命中用存储输入重跑 `permits`，parity 结构性
  成立）；`exec.c` TLBI 窗口（CRn 8/9，op1∈{0,1,2,4}）→ SYS_TLBI →
  `oemu_mmu_flush_all`；`mmu_internal.h` 增 `oemu_mmu_internal_walk` 纯
  旁路与 `oemu_mmu_internal_tlb_peek`。
- 调试记录：split-page 用例揪出真 bug——fill 存页对齐 key 却存带偏移
  的完整 pa，命中回吐他字节的偏移；修为键值同粒度、命中重加当前偏移。
- `make test`：743/743，100%（735 + parity 4 + MmuTest 3 + vcpu e2e 1）。
- `make asan`：743/743，100%。
- `ctest -R Tlb`：9/9；`ctest -R Mmu`：70/70（均 100%）。
- `make format-check`：本卡文件全部干净；仅 `bench/corpus/k_addsub.c`
  存在与本卡无关的本地 LLVM-21 vs CI LLVM-18 既有漂移（不触碰）。
- `make tidy`：0 errors（20130 条 suppression 计数为 gtest 噪音既有口径）。
- parity 扫描实跑命中 409 次/miss 数千——非空洞验证成立。
- 已知边界：TLBI 全编码 → flush-all（超集语义）；nG/asid/vmid 存为标
  签位但暂不参与判定（无 ASID 精确失效、无 stage-2）；块叶按 4K 页粒度
  缓存（合法，只是密度低）。
