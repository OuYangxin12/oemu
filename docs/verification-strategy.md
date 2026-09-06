# 验证与规范总纲（M2c 收尾 → M5）

本文规定"怎么验证、按什么规范写、错了怎么快速定位"，适用于 M2c 收尾、
M3、M4、M5 的全部开发任务。它的动机是：后续任务的复杂度远超 M1/M2，
任何"先写代码、后补测试"的做法都会把错误埋进数千行新代码里；本文把
每个任务的验收命令、测试清单与独立参照物**前置到动工之前**，让错误在
最小的反馈环内暴露并被纠正。

## 文档分工

| 文档 | 回答的问题 |
| --- | --- |
| `docs/roadmap-full-system.md` | 做什么：范围、锁定决策 D1–D8、每阶段 Accepts |
| `docs/roadmap-linux-boot.md` | 验收门：实证修订后的 M3a–M5 门与 QEMU 基线 |
| `docs/linux-minimal-qemu.md` | oracle：QEMU 上的最小 Linux 基线与三标记 |
| `tests/README.md` | harness 怎么用：三种测试风格、支撑库、五步流程 |
| `AGENTS.md` + `.dsh/skills/` | 硬性规则与任务级操作手册 |
| **本文** | 验证纪律 + 每里程碑测试矩阵 + 快速纠错机制 |

本文只引用上述文档的结论，不重复其内容；冲突时以实证路线图
（`roadmap-linux-boot.md`）为准。

## 六条验证纪律（反 vibecoding 原则）

每一条都对应一类 AI 辅助开发的高发错误。开工前逐条自查。

- **P1 验收先于实现。** 每个任务动工前先填任务卡
  （`docs/templates/milestone-task-card.md`），写下可运行的验收命令与
  通过标准、计划新增的测试清单，然后才写代码。"先实现后想怎么测"在本
  项目视为返工。
- **P2 Oracle-first：期望值只能来自实现之外。** 合法来源：llvm-mc /
  clang 汇编产出的金标准语料、QEMU 行为对照、ARM ARM 引用、经 QEMU
  与 Linux 核对过的常量、独立于被测实现的构造器（`elf_builder` 模式）。
  禁止"自己和自己一致"：用被测代码自己的定义生成期望值，什么也证明
  不了。
- **P3 最小反馈环。** 迭代时只跑相关子集：`ctest --test-dir build/debug
  -R <名字>` 或 `-L <标签>`，失败后用 `make retest` 只重跑失败用例；
  子集绿了再跑全量。禁止每改一行就跑全量再等十分钟。
- **P4 不变量逐 PR 复核。** 每个 PR 合入前对照 roadmap 的四条不变量：
  纯 C11 且步进路径零分配、精确异常（交付异常时不越过触发指令）、
  一切分配经 `oemu_allocator` seam、`make test` / `make asan` /
  `make format-check` / `make tidy` 全绿。新模块沿用既有 ≥97% 行覆盖
  门槛（`make coverage-summary`）。
- **P5 失败不留痕迹 + 状态二分。** 被拒绝的操作不提交任何状态；
  `OEMU_ERR_DECODE`（镜像坏了）与 `OEMU_ERR_UNSUPPORTED`（oemu 没实现）
  的文案必须可区分——纠错的第一步走对方向，比任何调试都快。
- **P6 不声称未运行的验证。** 报告里只写实际跑过的配置与真实数字；
  "应该能过"不算数（AGENTS.md 既定规则）。

## 测试分层体系

五层各有明确的发现错误类型；新测试先归入正确的层，再选风格。

- **L0 单元测试**——默认层。黑盒走 `include/oemu/` 公共 API；纯内部
  决策逻辑走 `src/<module>/<module>_internal.h` 白盒；`OEMU_REQUIRE`
  的 abort 契约走死亡测试。风格选择、支撑库与注册流程见
  `tests/README.md`。
- **L1 金标准与穷举**——用于"期望值容易被实现带偏"的逻辑。每向解码器
  加一条指令，指令字连同 llvm-mc 打印的汇编文本进金标准语料；纯函数
  输入空间够小就穷举（沿用条件码真值表、DecodeBitMasks 全空间扫描的
  既有技法），不抽样。
- **L2 guest 裸机测试**——freestanding AArch64 汇编用例，住在
  `tests/guest/`，交叉编译；沿用 `bench/guest/build.sh` 的模式：宿主无
  AArch64 工具链时干净跳过（exit 3 语义），CTest 标签 `guest`。guest
  用例经 magic byte / 退出码对外报告结果，驱动测试（如
  `test_boot_smoke.cpp`）断言这些外部信号，不窥探内部。
- **L3 启动阶段门**——把"启动 Linux"拆成可独立调试的四门：**earlycon
  有输出 → 内核日志持续推进 → `BOOT OK` → `SHELL_ALIVE`**。每门独立
  命名、独立可跑；镜像缺失时在 CI 干净跳过（roadmap 既定假设）。过哪
  门、卡哪门，是 M4/M5 期间汇报进度的标准语言。
- **L4 差分对照**——QEMU 是 oracle：同一 Image + initramfs 先在
  `qemu-system-aarch64 -machine virt` 验证，再在 oemu 上跑；QEMU 能、
  oemu 不能 ⇒ oemu bug（实证路线图升格为纪律的规则）。同层还有两类
  parity：TLB 服务结果必须等于 walk 服务结果（TLB 只许更快、不许不
  同）；seam 改造后双路 parity。M5 调试允许用串口日志 diff 与指令
  trace diff 做定位。

## 每里程碑测试矩阵

每里程碑固定栏目：改动面 / 新增测试 / 测试支撑 / oracle / 验收门 /
CI 影响 / 高发陷阱。验收门与 `roadmap-linux-boot.md` 第 2 节一一对应。

### M2c 收尾 — `oemu boot` CLI + guest 冒烟

- **改动面**：`src/main.c` 增加 `boot` 子命令（`-kernel <raw-bin>
  [--entry ADDR]`，映射到 `0x40080000`，EL1 + identity 进入）；新建
  `tests/guest/`。
- **新增测试**：`el1_smoke.S`（guest：arm VBAR → SVC 陷入并返回 →
  fake UART 打 magic byte → BRK 退出）；`test_boot_smoke.cpp`
  （标签 `guest`，无工具链 `GTEST_SKIP`）；`test_cli` 增加 `boot`
  子命令的 fork 式用例（沿用现有 CLI 测试模式）。
- **oracle**：SVC 往返的期望状态来自 M2b exc 模块既定的进入/返回语义；
  `oemu run` 的既有 fork 测试充当行为回归基线。
- **验收门**：`ctest -L guest` 通过（或无工具链干净跳过）；`make test`
  全绿且用例只增不减；`oemu run` 行为字节级不变。
- **陷阱**：boot 与 run 共用 exec 路径——任何为 boot 改的 exec 行为都要
  先过 `oemu run` 全回归；UART 是 M4 的 PL011 之前的 fake 设备，别把
  临时实现写成既定行为。

### M3a — Stage-1 页表翻译（无 TLB）

- **改动面**：新 `src/mmu/`（EL1&0、4 KiB granule、通用 TCR.TnSZ、
  TTBR0/1、block/page 描述符、AF 写回、AP/UXN/PXN 逐级累积、各级
  Translation fault、Address Size fault、`SCTLR.M=0` 时 identity）；
  memops 接翻译层；DC ZVA 实现为真实写零。
- **新增测试**：`test_mmu.cpp`（黑盒：经 vcpu/aspace 的数据与取指访问、
  ESR DFSC + FAR 精确、触发指令零副作用）；`test_mmu_internal.cpp`
  （白盒：walk 判定表——fault-class × level × AP × UXN/PXN ×
  block/page × TnSZ，**39 位与 48 位 VA 两种配置都覆盖**；VA[55] 的
  TTBR0/1 banding；AF 写回含"描述符只读 → Permission fault"）；
  `mmu_smoke.S`（guest：identity 页表 → 置 SCTLR.M → 跨页访问 →
  UART magic → poweroff）。DC ZVA 写零、DC CVAC/CVAU 与 IC IVAU 按
  no-op 接受、barrier 按 no-op 接受，各配显式用例。
- **测试支撑**：`tests/support/page_table_builder.h`——按
  `elf_builder` 模式逐字段独立构造页表，不复用 walker 的任何定义。
- **oracle**：ARM ARM 的 walk 语义；可 dump 内核 idmap/swapper 页表做
  黄金样本（实证路线图建议）；fault 的 DFSC 值与 QEMU/Linux 常量核对。
- **验收门**：白盒判定表全绿；guest `mmu_smoke.S` 通过；user-mode
  facade 完全绕过翻译、既有测试零改动。
- **陷阱**：翻译在步进路径上——**零分配不变量**最紧，OOM 注入只针对
  init；TLBI/DC/IC 编码必须在解码器被"接受"（UNSUPPORTED 会把 musl
  memset 变成崩溃）；39 位只测 48 位是实证踩过的坑（tinyconfig 用 39
  位）。

### M3b — TLB 与维护指令

- **改动面**：`src/mmu/` 加 direct-mapped flush-all TLB（D8：先完全
  正确的 walk，再加只许更快的 TLB）；TLBI 全套编码接线；TTBR/TCR/MAIR
  写与 SCTLR.M 翻转 flush all。
- **新增测试**：parity 扫描——固定种子的伪随机访问序列，TLB 服务结果
  == walk 服务结果（沿用 seam parity 技法）；映射 → 改表 → 访问三步
  失效测试；各 flush 触发点逐一断言。
- **oracle**：无 TLB 路径本身就是 oracle——这是 D8 决策的直接产物。
- **验收门**：parity 全绿；改表后旧映射立即失效。
- **陷阱**：任何"TLB 与 walk 结果不同但性能更好"的优化都是 bug，不是
  权衡。

### M4a — PL011 + DTB + Image loader + boot 协议

- **改动面**：`src/dev/pl011.c`（DR/FR/CR/IBRD/FBRD/LCR_H/IMSC/RIS/
  MIS/ICR；16 项预分配 TX 环；RX 注入钩子）；`src/fdt/fdt.c`（纯函数
  FDT builder）；`src/kernel/image.c`（AArch64 Image 头解析）；
  `oemu boot` 完整参数面。
- **新增测试**：`test_pl011.cpp`（寄存器行为、TX 环满/空、RX 注入；
  经 aspace MMIO 驱动）；`test_fdt.cpp`（结构块/字符串块正确性）；
  `test_image.cpp`（字节级构造 Image 头：magic `ARM\x64`、text_offset、
  IPA-size 标志诚实拒绝——`elf_builder` 模式）；`test_cli` 扩展。
- **测试支撑**：fdt 期望值用独立 reader 或 `dtc` 对照（缺失则跳过）；
  PL011 早期字节策略测试与 QEMU 行为对齐。
- **oracle**：QEMU virt 的 PL011/DTB 行为；**早到字节是缓冲还是丢弃，
  与 QEMU 对齐后写成明文决策**（实证观察 4：QEMU 在内核使能 UART 前
  丢弃管道字节，测试脚本据此 sleep 后喂入）。
- **验收门（L3 第一门）**：真实内核 **earlycon 输出 "Booting Linux"**
  ——boot 协议、UART、早期异常向量全部正确的联合证据；镜像缺失 CI
  跳过。
- **ID 寄存器一致性审计（本里程碑内）**：`ID_AA64PFR0/ISAR0/...` 宣传
  的每个特性位要么已实现、要么内核不会走到；FP/SIMD=0 与无 FP 路径
  闭环。审计产出核对清单，内核日志 `CPU features` 行做旁证。
- **陷阱**：设备回调零分配、缓冲全部 init 预分配（D 系列决策）；DTB
  与 ID 寄存器、机器布局三者必须自洽——Linux 只信我们说的话。

### M4b — GICv2 + arch timer + PSCI

- **改动面**：`src/dev/gicv2.c`（distributor + CPU interface，
  `ICC_*` sysreg 状态住 vCPU）；`src/timer/gtimer.c`（CNTFRQ/CNTPCT/
  CVAL，驱动 IRQ pin）；`src/fw/psci.c`（SMC #0 拦截）。
- **新增测试**：`test_gicv2.cpp`（使能/优先级/pending/EOI 流程，固定
  表优先级扫描）；`test_gtimer.cpp`（**可注入 fake clock**——比较
  器触发、IRQ pin 置位，全程确定性，不依赖宿主时序）；`test_psci.cpp`
  （VERSION=0x00020000、单核 CPU_ON 语义、SYSTEM_OFF → machine
  poweroff → CLI exit 0）；`psci_off.S`（guest：EL1 打印字符串 →
  `smc #0` SYSTEM_OFF → 退出码 0）。
- **oracle**：PSCI 常量与行为对齐 QEMU in-model PSCI（D4 既定）；timer
  频率 62.5 MHz 对齐 QEMU virt 基线。
- **验收门**：guest `psci_off.S` 通过；L3 第二门——内核日志出现计时
  器校准与 jiffies 走动（无 initrd 时 "panic 但时钟在走" 本身计为
  M4b 的中期门）。
- **CI 影响**：按 M4 既定计划新增 `guest` job（安装
  `gcc-aarch64-linux-gnu`）；本地无工具链时一切 guest 用例干净跳过。
- **陷阱**：用真实宿主时钟写 timer 测试 = 间歇性失败之源，必须 fake
  clock；WFI 是 D2 的 yield 点不是 NOP——语义别写反。

### M5 — initrd + busybox shell（终点门）

- **改动面**：`-initrd` 加载与 `/chosen` 注册；stdin raw mode 喂 PL011
  RX；中断接线（timer PPI、PL011 SPI 33）；`docs/booting-linux.md`
  （defconfig diff + initrd 配方）；`--smp N` 殿后（可选）。
- **新增测试**：boot 冒烟脚本复刻 QEMU 基线（`oemu boot -kernel Image
  -initrd initramfs.cpio -serial stdio`，断言 `BOOT OK` +
  `MINIMAL-BOOT-CHECK-PASSED` + `SHELL_ALIVE` + exit 0）；stdin 喂入
  时序与 QEMU 对齐（guest 起来后再喂）。
- **oracle**：`docs/linux-minimal-qemu.md` 的全部验收输出；**QEMU 能、
  oemu 不能 ⇒ oemu bug** 是本里程碑的最高调试纪律。
- **验收门（L3 第三、四门）**：串口应答 `BOOT OK` → 交互 shell 应答
  `SHELL_ALIVE`；guest `poweroff` → PSCI SYSTEM_OFF → oemu exit 0。
  CI 以镜像存在为前提、缺失即跳过。
- **调试工具（成文备查）**：串口日志落盘比对 QEMU 日志；`--max-insns`
  类预算防挂死；必要时 QEMU 指令 trace 与 oemu 步进 trace 做 diff
  定位分歧点。
- **陷阱**：性能预期管理——纯解释器启动按分钟计，"能跑"是门、"跑得
  快"是 M6；`--smp 4` 在单核 shell 达成之后才碰。

## 规范汇编（逐条可检查）

模块、错误处理与测试的既定规范分别由 `oemu-add-c-module` skill、
AGENTS.md 与 `tests/README.md` 持有，此处只补充 M3–M5 新增场景的条目。

**设备模型（M4 起适用）**
1. 设备回调（read/write）永不分配；一切缓冲在 device init 时经 seam
   预分配。
2. 未实现的 MMIO 寄存器必须有成文的明确行为（读返 0 / 写忽略 / 触发
   Data Abort），不允许"碰巧返回了什么"。
3. 设备对外可见行为（如 UART 早到字节策略）与 QEMU 对齐，并把决策写进
   模块头注释。

**架构语义（M3 起适用）**
4. ESR/ISS 编码、DFSC 值、ID 寄存器取值必须在代码注释里注明出处
   （ARM ARM 条目或 QEMU/Linux 核对记录）——没有出处的常量视为可疑。
5. 精确异常：交付异常时架构状态不越过触发指令（含 writeback 与 PC）。
6. 解码器对 M3/M4 必需的维护类指令（DC/IC/TLBI/barrier）必须"接受"，
   no-op 语义也要显式实现并配测试，不允许落回 UNSUPPORTED。

**PR 验收清单（每 PR 合入前逐项打勾，报告真实输出）**
- [ ] `make test` 100% 通过，用例只增不减
- [ ] `make asan` 100% 通过（触及分配/生命周期/算术时不可豁免）
- [ ] `make format-check` 与 `make tidy` 干净
- [ ] 新模块 `make coverage-summary` 行覆盖 ≥97%（或成文的豁免理由）
- [ ] 任务卡（见下）已填写并随 PR 更新
- [ ] 里程碑验收门该跑的跑了、该跳的干净跳了

## 快速纠错机制

**标签地图**（`ctest -L`/`-R` 选择最小子集）：

| 选择器 | 选中 |
| --- | --- |
| `-L unit` | 全部用例 |
| `-L whitebox` | 触达 internal 头的用例 |
| `-L death` / `-LE death` | 只要 / 排除死亡测试（`make test-death` / `make test-fast`） |
| `-L guest` | guest 裸机用例（M2c 起新增） |
| `-R <Module>` | 按模块名正则（`ctest -N` 列全量） |

**迭代循环**：改代码 → `-R`/`-L` 跑相关子集 → 失败则 `make retest`
只重跑失败项 → 绿了之后 `make test` + `make asan` 全量 → 里程碑任务
再跑对应验收门。

**确定性三件套**——间歇性失败是纠错效率的第一杀手：
1. 伪随机扫描固定种子，失败可复现；
2. timer 等时钟逻辑一律经可注入 clock，不读宿主时间；
3. guest 输入喂入时序固定（与 QEMU 基线同一脚本约定）。

**错误信息质量**——让"看错方向"不可能发生：DECODE 与 UNSUPPORTED
文案二分；启动阶段门各自独立命名、各自可跑；guest 用例的
`GTEST_SKIP` 条件写明缺什么、怎么补。

**CI 失败本地复现**：CI 跑的就是本地预设（`cmake --preset ... &&
ctest --preset ...`）；复现流程与环境坑（conda 抢 GTest、TSan ASLR、
gcov 版本配对、clang-tidy 遇 GCC flags）见 `oemu-ci-workflow` 与
`oemu-build-configs` 两个 skill，不要重新"发明"修复。

## 任务卡流程

里程碑任务开工前：复制 `docs/templates/milestone-task-card.md`，填写
范围、验收命令、测试清单与 oracle 来源，随 PR 维护。完工时对照 PR
验收清单逐项打勾并记录真实输出。任务卡是"验收先于实现"（P1）的载体
——没填卡的任务不动工。

## 维护规则

与 `tests/README.md` 同一约定：本文只写不随用例增减变化的内容（分层、
纪律、矩阵、机制）；用例级细节维护在各测试文件头部注释里；验收门本
身的变化改 `roadmap-linux-boot.md`，本文跟随引用。用例实时数目以
`ctest --test-dir build/debug -N` 为准，本文不写具体数字。
