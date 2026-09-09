# 在 oemu 下启动 Linux（M5 门禁）

M5 的终点判据只有一句：`docs/linux-minimal-qemu.md` 里在 QEMU 上量到的那组标记与退出契约，在
`oemu boot` 下同样成立。QEMU 是 oracle，判据是它那里先量出来的，本文只负责说清楚怎么跑、怎么判、
以及现在卡在哪。

## 一条命令

```
make boot-linux
```

等价于运行 `scripts/boot-linux-gate.sh`，默认判据就是那三个标记，一个都不能少：

| 标记 | 含义 |
| --- | --- |
| `BOOT OK` | 内核已把控制权交给 initramfs 里的 `/init` |
| `MINIMAL-BOOT-CHECK-PASSED` | `/init` 自带的断言通过（文件系统、设备节点、时钟等） |
| `SHELL_ALIVE` | 我们敲进控制台的一行被交互式 shell 应答回来了 |

外加退出契约：`poweroff` → PSCI `SYSTEM_OFF` → **oemu 退出码 0**。

脚本退出码与 `scripts/qemu-oracle.sh` 保持同一形状，便于 CI 一视同仁：
`0` 全绿；`1` 标记缺失或退出码不符（日志保留并打印路径）；`2` 用法/输入缺失；
`3` 已知阻塞下的显式跳过（见下）。

常用开关：`--kernel/--initrd/--bin`、`--timeout SEC`（默认 420）、`--marker M`（覆盖默认三个）、
`--expect-exit N|any`、`--no-interactive`（只验内核侧标记，不喂 stdin）、`--smp N`、`-m MB`。
喂 stdin 的等待时间由 `OEMU_BOOT_GATE_FEED_DELAY`（默认 90 s）控制，指令预算由
`OEMU_BOOT_GATE_MAX_INSNS`（默认 4e9）控制。

> 交互判据必须用 `-serial stdio`：`--serial file:` 那条路径没有 RX，敲不进去。这也是脚本默认
> 走 stdio 的原因——一个声称验过 `SHELL_ALIVE` 的门禁不能用 `file:`。

手工等价命令（排障时比脚本好用，因为能直接看到 stderr）：

```
( sleep 90; printf 'echo SHELL_ALIVE\npoweroff -f\n' ) | \
  build/debug/bin/oemu boot -kernel guest/build/Image -initrd guest/build/initramfs.cpio \
    -m 256 -append "console=ttyAMA0 earlycon=pl011,0x9000000 panic=-1 rdinit=/init" \
    --max-insns 4000000000 -serial stdio
```

## 当前状态：红，且是诚实的红

门禁现在**不通过**，原因不是脚本、判据或 guest 镜像，而是 oemu 侧一个尚未修掉的缺陷，全部证据与
排查顺序记在 issue **#28**。现象固定：内核一路正常到

```
SMP: Total of 1 processors activated.
CPU features: detected: CRC32 instructions
CPU: All CPU(s) started at EL1
```

之后停住，`Run /init` 再也不出现，`BOOT OK` 因此拿不到。指令预算耗尽时模型报告

```
[abrt1] esr=0x96000005 far=0xffffffc080226260 pc=0xffffffc0801a66c4 sp=0xffffff8000226fc0
[del1..N] kind=1 daif=0 pc=0xffffffc0801a6484 (= el1_interrupt + 0x1c) ttbr1=0x4022e000
```

即 guest 在自己的 EL1h IRQ 入口存根里（`el1h_64_irq_handler + 0xc`，正在 `kernel_entry` 建栈帧）对
`_text + 0x226260` 做一次**读**时撞上 level-1 translation fault；此后中断风暴恢复原状（投递点恒为
`el1_interrupt + 0x1c`、每次 SP 递降 `0x170`，说明栈帧只推一半、处理器从未跑完，电平拉高的定时器线约
40 条指令后再次被取）。决定性的一条是：**每次投递时 `TTBR1_EL1` 都是 `swapper_pg_dir`
（`0x4022e000`），从第一次投递到第一万次都是**，而日志走到 `CPU: All CPU(s) started at EL1` 时
Linux 的活页表必然是 `init_pg_dir`（`0x40341000`；`paging_init` 在 `setup_arch`，打印该行的 `smp_init`
在晚得多的 `rest_init`）。这两条互斥，其一必被读错；且曾见过 `0x40341000` 被写入两次、其后又写入
`reserved`/`swapper`，而 `TTBR0/TTBR1_EL1` 的表行是朴素的（`offset` + 全 `write_mask`，无 CCI 屏蔽），
所以掩码类缺陷不成立。下一记判据：写 `TTBR1_EL1` 后立刻读回比对——若 `0x40341000` 的写读回不是它自己，
就是一个可脱离 guest 单测的 sysreg 缺陷，并能一句话解释风暴。

注意本条曾被写错两次：一次记成 `ESR=0x86000005`（取指中止），一次记成 `ESR=0x96000045` 且 `FAR`
越界——**后一条是我自己引入的向量回归（`296b7c7`、`042aec1`，已由 `5e901ec`、`cfa1de0` 撤销）造成的
伪象**，不是 guest 的行为。教训是：改了异常投递的实现之后，旧二进制/旧签名都会把回归伪装成 guest 缺陷，
所以重测前必须删掉 trace 二进制重建。

已经用实测排除的方向（不要再走一遍）：镜像装载与入口、陈旧 TLB、`stp` 前索引缩放、`task_struct->stack`
被写坏、`ICC_*` 系统寄存器、银行化 `SP_EL1` 陈旧、入口 DAIF 不生效（单测钉住）、**向投递屏蔽中的
guest 投递**（前 400 次投递 DAIF 均可为 0，无一例在 I 置位时投递）、**WFI 唤醒打断上下文恢复**
（`TTBR1` 恒为 swapper，非瞬时状态）、GIC ACTIVE 抑制、DT/GIC 探测失败、嵌套同步数据中止、
向量槽次序（原实现 `kind << 7` 一直是对的，我错改两次并已撤销）、`TTBR0/1_EL1` 的 `write_mask`/CCI 屏蔽。原列出的：Image 装载地址与入口（与 booting.rst 和 oracle 逐字节一致）、
页表索引与描述符解码（`swapper_pg_dir`/`init_pg_dir` 位置由 `nm` 定标）、TLBI 与 `TTBR` 写入的失效
路径（我们是整体失效，保守正确）、`stp` 的偏移定标与写回、`SPSel`/DAIF 是否漏实现、以及
**generic timer 计数速率**（每指令 1e6 个计数比对外宣告的 62.5 MHz 快 1600 万倍，已在 `606e245`
修掉，中断风暴随之消失）。当前最强证据：那次致命写是 `el1h_64_irq_handler` 序言的 `stp`，
`sp` 指向内核自己的页表页，而 `tsk` 寄存器指向一整页全零的内存——异常是在上下文尚未建好时进入的。

CI 若要在修好之前继续跑这条门禁，用 `GATE_ARGS="--allow-blocked"`：它把这一条特定的失败映射成
`3`（跳过），既保留信号，也不假装通过。

```
make boot-linux GATE_ARGS="--allow-blocked"
```

## `--smp 4` → `nproc == 4`

这一项**目前无法达成**，且与门禁脚本无关：`oemu boot --smp N`（N>1）现在被 CLI 直接拒绝
（`--smp is not supported yet; one vCPU boots alone (M4b+)`），多核与 IPI 属于排期里的 M4b+ 后续
（`GICD_SGIR` 记录但不产生 SGI、`GICD_SGIR_NS` 未实现等，见 #26 的卫生清单）。脚本因此只在
`--smp > 1` 时才把该选项传下去，避免把门禁变成一条用法错误。

## 相关

* `docs/linux-minimal-qemu.md` —— oracle 侧的最小启动配方与标记来源。
* `docs/verification-strategy.md` —— L4 差分纪律：先在 QEMU 上量，再在同一判据上回放 oemu。
* `docs/roadmap-linux-boot.md` 与 `docs/task-cards/` —— M5 的任务卡与排期。
