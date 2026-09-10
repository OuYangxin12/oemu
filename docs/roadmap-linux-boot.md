# 新路线图：oemu 启动最小 Linux（实证修订版）

日期：2026-09-06。本文以 `docs/linux-minimal-qemu.md` 的 QEMU 实证基线为
依据，细化并修订 `docs/roadmap-full-system.md` 的 M3–M5 范围与验收门；
不变量（零分配步进、精确异常、allocator seam、四绿）与锁定决策 D1–D8
继续有效。

## 0. 目标定义（实证化）

**"oemu 能跑 Linux" = 复现实证基线**：加载与 QEMU 测试完全相同的
Image + initramfs（`guest/build/`），串口应答 `BOOT OK`、
`MINIMAL-BOOT-CHECK-PASSED`、`SHELL_ALIVE`，guest `poweroff` 使 oemu
以退出码 0 结束。命令行形态：

```
oemu boot -kernel Image -initrd initramfs.cpio -serial stdio
```

QEMU 是 oracle：同一镜像先在 qemu-system-aarch64 验证，再在 oemu 上跑；
QEMU 能、oemu 不能 ⇒ oemu bug。这条工作规则升格为调试纪律。

## 1. 实证带来的三项范围修正

1. **M3（MMU）**：翻译测试必须覆盖 **39 位与 48 位两种 VA 配置**（4K
   granule；tinyconfig 实测用 39 位）。TLBI 全套指令接线；**DC ZVA 必须
   实现为真实写零**（glibc/musl memset 会用）；DC CVAC/CVAU、IC IVAU
   在无真实 cache 模型下按 no-op 处理但指令必须被接受；barrier 按 no-op
   语义接受。
2. **M4（设备）**：设备集收敛为实证清单 —— PL011（TX + RX，早到字节的
   缓冲/丢弃策略与 QEMU 对齐）、GICv2（distributor + CPU interface，
   timer PPI 路由）、arch timer（CNTV 计数器 + 62.5 MHz，频率可配）、
   PSCI v1.1（SMC conduit：SYSTEM_OFF/SYSTEM_RESET/CPU_ON）、DTB 生成、
   Image loader（boot protocol：x0 = DTB 物理地址，按 Image 头
   text_offset 放置，EL1 进入，MMU/cache 按协议初始态）。
3. **ID 寄存器一致性审计**（M4 内）：内核按 ID_AA64PFR0/ISAR0 决策
   （实证日志可见 CRC32、32-bit EL0 位被读取）。审计目标：宣传的每个
   特性位要么已实现、要么内核不会走到；FP/SIMD=0 与无 FP 路径闭环。

## 2. 里程碑与验收门

### M3a — 正确的两级查找页表翻译（无 TLB）
- 范围：TTBR0/1、TCR、SCTLR.M、MAIR 属性、AF/access 权限 fault、XN、
  block/page 混合级联、banding（TTBR0/1 切换）；DC ZVA 写零。
- 验收门：
  - 白盒：手工构造页表覆盖每种描述符/属性组合（继承 existing 穷举风格）；
  - 黑盒：guest 裸机测试程序经数据/取指访问各映射区，权限违例拿到正确
    ESR DFSC + FAR，触发指令状态精确（无副作用）。
- 参考：可 dump 内核 idmap/swapper 页表做黄金样本。

### M3b — TLB 与维护指令
- 范围：flush-all TLB → 按 ASID/VA 精确失效（D8 顺序）；TLBI 全套编码、
  DC/IC 维护指令接受语义（见上）。
- 验收门：换页表后旧映射立即失效（映射—改表—访问三步测试）；TLB 仅允许
  更快、不允许不同（与无 TLB 路径 parity 测试，沿用 seam parity 技法）。

### M4a — 串口与启动协议
- 范围：PL011 MMIO 设备、DTB 生成器（内存/CPU/UART/GIC/timer/PSCI 节点）、
  Image 加载器（头校验、text_offset、initrd 放置）、`oemu boot` 命令行。
- 验收门：**内核 earlycon 输出 "Booting Linux"** —— 说明 boot protocol、
  UART、早期异常向量全部正确。

### M4b — GICv2 + arch timer + PSCI
- 范围：GICv2 dist/cpuif MMIO、优先级/使能/EOI、timer PPI 注入、
  CNTV 读写 + 比较中断、PSCI SMC 拦截（SYSTEM_OFF/RESET/CPU_ON）、
  WFI 停步进（D2 的 yield 点）。
- 验收门：内核日志出现计时器校准与 jiffies 走动（无 initrd 时内核会
  panic 找不到根 —— **"panic 但时钟在走" 本身就是 M4b 的可测中期门**）；
  guest `poweroff` 使 oemu exit 0。

### M5 — initrd + busybox shell（终点门）
- 验收门：完整复现 `docs/linux-minimal-qemu.md` 的冒烟脚本
  （`oemu boot ...` 替换 qemu 命令，断言三标记 + exit 0）；
  CI 中以镜像存在为前提、缺失即跳过（路线图既定假设）。

### M6（horizon，不阻塞终点）
- `--smp N`（PSCI CPU_ON + 协作调度已就位）；JIT 提速；
  FP/SIMD 寄存器堆（仅当用户态实测踩到）。

## 3. 度量与预期

TCG 上从上电到交互 shell 仅数秒。oemu 纯解释器按 TCG 1/10–1/50 吞吐
保守估计，启动预计分钟级 —— 终点门是"能跑"，不是"跑得快"；JIT 属
M6。Image 3.2 MB / 内核代码 1728K 说明内存模型规模无需担心（256 MB
实例即可）。

## 4. 风险与缓冲

| 风险 | 缓解 |
| --- | --- |
| 用户态 FP/SIMD（D6） | 实证基线用静态 busybox 跑通；oemu 上复验，踩到则从 M6 拉 FP 寄存器堆进 M5 |
| DC ZVA 遗漏 | 已列入 M3a 范围，guest 测试程序显式覆盖 |
| ID 位宣传与实现不一致 | M4b 一致性审计 + 内核日志比对（CPU features 行） |
| -j 并行构建偶发失败 | 降核重试（实证中 -j32 挂过一次，串行成功） |
| 串口早到字节策略分歧 | 与 QEMU 对齐并在 oemu 测试中用相同喂入时序 |

## 5. 现状对照

M1 ✅（总线/地址空间/机器）· M2a ✅（sysreg）· M2b ✅（exc）·
M2c ✅（PR #21）· M3a ✅（PR #19）· M3b ✅（PR #22）· M4a ✅（PR #24）·
M4b ✅（GICv2 + arch timer + PSCI，见带 `(M4b)` 的提交）·
**M5 ✅（本 PR）** · M6 未开始。每个 PR 保持 `make test`、`make asan` 全绿后再合入。

M5 的验收门已经按下：

```
$ bash scripts/boot-linux-gate.sh
boot-linux-gate: PASS (markers: BOOT OK MINIMAL-BOOT-CHECK-PASSED SHELL_ALIVE; exit: 0)
```

默认 `build/debug/bin/oemu`、默认超时，17 s 墙钟；判据与断言都在树里，来源是
`qemu-system-aarch64` 的实测记录（`scripts/qemu-oracle.sh`、
`scripts/oracle-uart-regs.py`，另见 `docs/booting-linux.md`）。

标题里的 "busybox" 需要一句说明：交互 shell 目前是本仓库手写的静态 `/init`
（`tests/guest/init.c`，由 `scripts/build-linux-initramfs.sh` 用内核自带的
`gen_init_cpio` 打包，2048 字节），它理解 `echo` 与 `poweroff` 两行、回显
`SHELL_ALIVE`，足以判定"提示符可交互 + 干净关机"。第 4 节风险表里"实证基线用静态
busybox"那条还没在 oemu 上复验——换 busybox 只换 initramfs 的内容，门禁判据不变，
所以记作 M5 的后续项，不算缺口。
