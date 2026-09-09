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

## 当前状态：红（以下只记实测，不记解释）

门禁不通过。原因不是脚本、判据或夹具，而是 oemu 侧一处尚未定位的机器级偏差。串口日志固定停在
`CPU: All CPU(s) started at EL1`，`Run /init` 不出现；已确认非控制台丢字节（`tx_dropped` 恒 0，
门禁也会因丢弃而 exit 2）。

用 #28 里那组探针量到的、彼此独立的事实：

1. 载入后首次一级翻译失败：`ESR=0x96000005`（同 EL 数据中止、`DFSC=0x05`、WnR=0 ⇒ **读**），
   `FAR=_text+0x226260`，PC 在 `el1h_64_irq_handler + 0xc`。4 亿条指令内**只有一次**，不嵌套。
2. 遍历本身是对的：失败处读到的原始 8 字节是 `0xffffff800022e950`，`bits[1:0]=0b00` ⇒ 按架构确实无效；
   `idx` 是**字节偏移**（`0x810/8 = 0x102` 号项，在 512 项之内）。⇒ 中止是真的，不是遍历算错。
3. 那一槽先被 `pc=0xffffffc08001be54` 写成合法表项 `0x100000004ffff003`；随后被
   **`el1h_64_irq_handler` 的第一条指令**写成 `0xffffff800022e950`。
4. 那次写**不是页表写**，而是入口存根保存 `pt_regs::sp`：写入值恰为 `x21 − 0x20`，`x22` 是 `ELR`
   （`el1_interrupt+0x1c`）、`x23` 是 `SPSR`（`0x100005`）、`0x20` 是栈帧大小。
5. 于是关键量是 `x21 = 0xffffff800022e970`：在本内核线性映射下（`PAGE_OFFSET=0xffffff8000000000`、
   `PHYS_OFFSET=0x40000000`）它指 **PA `0x4022e970`**，恰在**承载 `swapper_pg_dir` 的那一页**、且在镜像
   之内（`_text.._end = 0x40000000..0x40350000`）。**即中断发生时钟指针位于内核自身镜像内**，压帧便写坏
   了 PGD 第 `0x102` 项，随后要走的正是这张表。

已用实测排除（不要再走）：镜像装载与入口（`text_offset=0`，入口 `0x40000000`，与 guest 的
`__pa(_text)` 自洽；`BOOT_IMAGE_BASE=0x40080000` 是无代码使用的死宏）；`--initrd` 告知（`virt_dtb.c` 注入
`linux,initrd-start/end`，initrd 在 RAM 四分之三处）；DTB 内存描述（与 QEMU 的 `dumpdtb` 逐字段比过，
`memory@40000000` 一致，仅差 `rng-seed`/`kaslr-seed`）；GIC 两个使能位（`gicv2.c:396` 门住）、逐线
ISENABLER、ACTIVE 抑制；DAIF 屏蔽（前 400 次投递无一例在 I 置位时投递）；向量槽次序（原实现
`kind << 7` 一直正确，被我用 `296b7c7`/`042aec1` 错改两次，已由 `5e901ec`/`cfa1de0` 撤销）；
`TTBR0/TTBR1_EL1` 写不落地或被掩码（`SysregTest.TtbrWritesLandWholeAndDoNotDisturbEachOther` 钉住）；
一级块解码（`test_mmu` 的 `l1_block` 用例）；KPTI（`CONFIG_UNMAP_KERNEL_AT_EL0 is not set`）；PSCI 挂起
（`FEATURES=0` ⇒ 内核不注册 `cpu_suspend`）；`ICC_*` 系统寄存器；WFI 唤醒次序；控制台丢字节。

本 issue 的排查方式本身是一条教训：线程里贴出过 5–6 个被下一次实测否证的结论，每次都是"测量正确、
框定测量的假设出自我记忆"。因此本节只列事实。下一件该拿的东西是**装载后写进 RAM 的那份 FDT 的转储**
（夹具源文件不是地面真值，它会被补丁），以及"guest 为何会把首个用栈分到自身镜像内"的正面答案。

## 现在卡在哪（issue #28 第三轮复核）

上面那节"中断发生时钟指针落在内核镜像内"不再是当前阻塞点。补上 AMBA 组件号之后
（CID 在 `region_end-0x10`，即 `0xFF0..0xFFC` = `d f0 05 b1`；早先一次实测把
`0xFD0..0xFDC` 读成"全零"并当成事实记下了，那是错的地址），控制台正常注册，
`Run /init as init process` 与 `BOOT OK` 都打得出来，panic 消失。补充一条：**之前所有"只有 debug
构建才 panic"的现象，出自一个 6 小时没重建的 `build/debug/bin/oemu`**——门禁默认用的正是它，
而当时只重建了 `build/release`。

当前唯一可复现的偏差：**PID 1 的第二次 `write(1, ...)` 永不返回**，oracle 上同一夹具三次 write
全部按长度立即返回。

* 探针：`tests/guest/probe.c` + `scripts/mkcpio.py`（本机没有 cross binutils，用
  `clang --target=aarch64-none-elf -fuse-ld=lld -nostdlib -static -fno-builtin` 造）。
  oracle 打出 `PROBE-A` 后每条 write 都回 `0x1a`/`0x40`；oemu 只打出 `PROBE-A`。
* 串口无罪：退出诊断行 `oemu: uart: tx_emitted=... tx_dropped=0` 里 `tx_emitted` 恰等于日志
  字节数——不是丢字节，是 guest 根本没再写。
* 设备状态与 oracle 逐位相同：`CR=0x0f01 IMSC=0x0050 RIS=0x0020`（`scripts/oracle-uart-regs.py`
  从 QEMU monitor 读）；FR 曾差在 0x97 vs 0x90，即我们无端把 CTS/DSR/DCD 报成有效，已改成低。
* oracle 完全不给输入（`-serial file:` 且 stdin 接 `/dev/null`）时三个标记齐全 ⇒
  `MINIMAL-BOOT-CHECK-PASSED` 缺失与敲键无关。

本轮另以实测排除/修复：`/chosen/rng-seed` 缺失（已补，`random: crng init done` 现与 oracle 同行）；
接收中断路径（收到字节该置 **RXIS = 1<<4**，此前置的是 bit0 = RIMIS，driver 走 modem 分支把字节丢了，
且 `ris & imsc` 恒低）；`FR.TXFF` 由 host 侧未排空的 TX 队列触发（会让 `pl011_tx_room()` 返回 0，
`n_tty_write` 睡死在 `tty->write_wait`，而那一次排空不伴随任何中断）；CRC32 一族（`crc32_be` 走
`arch/arm64/lib/crc32.S`，错了就让内核判 devicetree CRC 失败）。

下一件该拿的东西：第二次 write 期间驱动是否**真的**又往 UART DR 写过字节——`OEMU_TRACE_VA` 窗口观察
两次返回零记录，而"零记录"在证明探针会响之前不算答案；可用 `OEMU_DUMP_MEM=<pa>:<len>:<file>`
（两者皆十六进制）从 guest RAM 里把 `uart_port`/`uart_state` 抠出来看 `suspended`/`x_char`/xmit 头尾。

## `--smp 4` → `nproc == 4`

这一项**目前无法达成**，且与门禁脚本无关：`oemu boot --smp N`（N>1）现在被 CLI 直接拒绝
（`--smp is not supported yet; one vCPU boots alone (M4b+)`），多核与 IPI 属于排期里的 M4b+ 后续
（`GICD_SGIR` 记录但不产生 SGI、`GICD_SGIR_NS` 未实现等，见 #26 的卫生清单）。脚本因此只在
`--smp > 1` 时才把该选项传下去，避免把门禁变成一条用法错误。

## 相关

* `docs/linux-minimal-qemu.md` —— oracle 侧的最小启动配方与标记来源。
* `docs/verification-strategy.md` —— L4 差分纪律：先在 QEMU 上量，再在同一判据上回放 oemu。
* `docs/roadmap-linux-boot.md` 与 `docs/task-cards/` —— M5 的任务卡与排期。
