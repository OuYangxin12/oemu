# 任务卡：M3a — Stage-1 页表翻译（无 TLB）

> 状态：已实现并合入 master（PR #19）；本卡保留为资产索引与实证记录。
> 依据：`docs/roadmap-full-system.md` M3 节、`docs/roadmap-linux-boot.md`
> M3a 验收门、`docs/verification-strategy.md` 测试矩阵。

## 范围

- 做什么：新 `src/mmu/`——EL1&0 regime、4 KiB granule、通用 TCR.TnSZ
  （16/25/34/43 阈值 → 根级别 L0/L1/L2/L3）、TTBR0/1 按 VA[55] 选择、
  block/page 描述符、AP/UXN/PXN 逐级累积、AF 写回（只读描述符触发
  Permission fault）、各级 Translation fault、Address Size fault、
  `SCTLR.M=0` 时 identity；memops 接翻译层；DC ZVA 真实写零；对齐
  fault（LDP/STP 8 字节、`SCTLR.A` 全访问）。
- 不做什么：TLB（M3b）；ASID/VMID（无 EL2/ASID 语义，nG 位存储但不
  行为化）；stage-2；39 位以外 granule。

## 验收命令与通过标准（动工前已定）

```sh
ctest --test-dir build/debug -R Mmu          # 单元全绿
ctest --test-dir build/debug -L guest        # guest 用例（需 tests/guest 骨架）
make test && make asan                       # 四绿底线
```

- 通过标准：白盒判定表全绿；`mmu_smoke` guest 在 oemu 与 QEMU 上
  输出相同 magic；user-mode facade 零改动。

## 已就位资产（本卡的前置，已先行落地并验证）

- `tests/support/page_table_builder.h`——独立页表构造器（elf_builder
  模式，与 `src/mmu/` 零共享），含 TnSZ→根级别数学、MAIR 编码、
  table/block/page 描述符与 table-descriptor 继承位（APTable 等）。
  18 项编码钉测试全绿。
- `tests/unit/test_page_table_builder.cpp`——构造器自身编码钉测试。
- `tests/guest/mmu_smoke.S`——**已实测通过**：
  `scripts/build-guest.sh tests/guest/mmu_smoke.S` 后经
  `scripts/qemu-oracle.sh build/guest/mmu_smoke.bin EL1 MMU-OK`
  （QEMU 8.2.2 virt/cortex-a53/-nographic）输出
  `EL1` → `MMU-OK`，含跨页非对齐读写。
- 实证常量（写死在 guest 注释里，与实现共享的是**值**不是代码）：
  TCR_EL1=0x81903519（T0SZ=25/TG0=4K/T1SZ=25/TG1=4K，WB/inner）、
  MAIR_EL1=0x04FF（attr0 WB，attr1 Device-nGnRE）、页表根 0x40100000、
  L2/L3 表 0x40101000/0x40102000。
- **血泪教训（QEMU ESR=0x96000007 抓出）**：stage-1 **L3 的
  bits[1:0]=0b01 是 BLOCK，属保留编码 → level-3 translation fault；
  页描述符必须 0b11**（builder 测试钉的是 0b11，手写汇编写错才会被
  双 oracle 抓住——这正是"guest 独立推导"的价值所在）。

## 新增测试清单（已由 PR #19 落地；本卡所列即其对应关系）

| 层/风格 | 文件 | 钉住的行为 |
| --- | --- | --- |
| 黑盒 | `tests/unit/test_mmu.cpp` | 经 vcpu/aspace 的数据与取指访问；ESR DFSC+FAR 精确；触发指令零副作用；SCTLR.M=0 identity；DC ZVA 写零 |
| 白盒 | `tests/unit/test_mmu_internal.cpp` | walk 判定表：fault-class × level × AP × UXN/PXN × block/page × TnSZ（39/48 位 VA）；VA[55] banding；AF 写回含只读描述符→Permission fault；对齐 fault 矩阵 |
| 死亡 | `tests/unit/test_mmu_check.cpp` | NULL 状态、非法参数的 `OEMU_REQUIRE` 契约 |
| guest | `tests/guest/mmu_smoke.S`（已就位） | 置 SCTLR.M 后跨页访问、magic 输出；oemu 与 QEMU 行为一致 |

- 注册：`oemu_add_test(...)` + `check` 依赖列表；`INTERNAL` 头来自
  `src/mmu/mmu_internal.h`。
- 测试支撑：页表经 `page_table_builder` 构建进宿主镜像，用
  `oemu_aspace_map_ram_alias` 挂上总线（零拷贝，walker 经 memops 读到
  的就是构造器写的字节）。

## Oracle 来源

- [x] Linux 6.6 本地树 `arch/arm64/include/asm/pgtable-hwdef.h`（描述符位、
      TCR；构造器注释已引用）
- [x] ARM ARM DDI 0487（walk 语义、DFSC 值；实现时逐条引用到注释）
- [ ] 可选黄金样本：dump 内核 idmap/swapper 页表
- [x] QEMU：`mmu_smoke` 已实测；fault 类的 DFSC 可与 QEMU 行为对照

## 不变量复核（实现时逐项打勾）

- [ ] 纯 C11；翻译在步进路径上——**零分配**（OOM 注入只针对 init）
- [ ] 精确异常：fault 交付时不越过触发指令（含 writeback 与 PC）
- [ ] 一切分配经 seam；`make test`/`make asan`/`format-check`/`tidy` 全绿
- [ ] 新模块行覆盖 ≥97%

## 风险与回退

- TnSZ→根级别边界算错：构造器测试已钉住阈值表（16/25/34/43），
  实现直接对照。
- TLBI/DC/IC 编码落回 UNSUPPORTED：musl memset 用 DC ZVA——M3a 必须
  接受并实现；解码器回归语料随实现扩展。
