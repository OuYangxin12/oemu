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

## 卡点已定位并修复：SVC 的返回地址是下一条指令

第三轮复核记下的"PID 1 的第二次 `write(1, ...)` 永不返回"**不是**阻塞，而是我自己的测量误读，
真因在下面。当时的现象（`tx_emitted` 等于日志字节数、寄存器与 oracle 逐位相同、第二标记仍缺失）
都指向"卡在 tty 里"，但那是症状。

真因：**`svc` 的 preferred exception return address 是陷阱指令的下一条**（DDI 0487 D1.4.5），
`hvc` 同理；而 `oemu_exc_take` 对所有 kind 都记 `ELR = pc`。于是内核的 `eret` 把同一个系统调用
再执行一遍，而 `x0` 此时装着上一次的返回值：

```
write(1, 0x40028c, 8) -> 8        # "BOOT OK\n" 打出去了
write(8, 0x40028c, 8) -> -EBADF   # fd 变成了上一次的返回值 8
write(-9, ...)         -> -EBADF   # 从此每 256 条指令一次，294,542 次
```

内核自己不执行 `svc`，所以到 `/init` 为止一切正常——这正是它藏了这么久的原因。钉住它的测试
原先写的是"同步陷阱会重试其指令，越过它是指针（handler）的职责"，那句话对 abort / undefined
成立（handler 可修复并推进 ELR），对 `svc`/`hvc`/`brk` 不成立；我自己写的测试把错误契约钉住了。

同轮的另外两件事：

* **空闲的 guest 不是死掉的 guest**：`-serial stdio` 下控制台就是 stdin，parked 的 vCPU 是在
  `read(2)` 上等键盘。原先一律 `EXIT_BLOCKED`，于是门禁那行 `echo SHELL_ALIVE` 到达时进程已经退出了
  ——两个标记都拿到了，仍然判 FAILED。现在只在 stdin 真正 EOF 时才判阻塞。
* 探针夹具 `tests/guest/probe.c` 报的"第二个 write 不返回"是夹具自身的错（它的 `say()` 忽略返回值，
  于是上面那串 -EBADF 在日志里完全隐形）。教训：**用项目自己的夹具下结论**，探针只提供问题。

## 门禁通过后补的两环（M5 完成）

标记齐了仍然 `exit code 1 != expected 0`，因为字符进到 RX ring 之后还有两处断链：

**一、`GICD_ITROUTERn` 是"每个中断一个寄存器"，且复位值指向 CPU0。**
它位于 `0x800 + 4n`，只有低字节（目标 CPU 列表）有效。我原先按"一个 32 位字装四条线"
实现——那是上面几百字节那组 enable 寄存器的形状——于是 guest 往 `0x884`（SPI 1 自己的
路由器）写的 "CPU0" 落到了第 132 条线上。而 SPI 路由器**复位值为 `0b00000001`**，我的实现
清零；guest 有权不改一个它没理由动的亲和性（`gic_dist_init()` 只把分发器打开），
于是复位值成了唯一依据。测量形式：`irq 33 en=1 tgt=0x00`，而设备侧 `device=1`——
中断举进了一个不肯路由它的分发器，看起来跟"键盘丢字符"一模一样。
`Gicv2.SpiTargetsThisCpuFromReset` 现在只碰 enable 寄存器就交付一个电平 SPI，
钉住"驱动可以不动路由"这一条。旧的 `SgiPpiTargetFixedSpiProgrammable`
把两个错误信念都钉住了（复位读 0、一字四线），是在**保护**这个 bug。

**二、`/init` 夹具说的是另一个内核的 `reboot(2)` ABI。**
`guest/` 下的内核被厂商改过：`_reboot()` 要**两个** magic
（`LINUX_REBOOT_MAGIC1=0xfee1dead` 与四个 `MAGIC2*` 之一），且
`LINUX_REBOOT_CMD_POWER_OFF=0x4321FEDC`（mainline 是 `0x4321fed5`）。夹具用的是上古的
`RB_POWERDOWN=0x4321FDA1`，在本树的 switch 里没有任何 case 处理它 ⇒ `-EINVAL` ⇒
init 返回 ⇒ `Kernel panic - not syncing: Attempted to kill init`——三个标记都在，
只是"模拟器不会关机"这个结论是错的。常量现在抄自被启动的那棵树的
`include/uapi/linux/reboot.h`：夹具唯一该忠实的 ABI 就是它启动的那个内核。

```
scripts/boot-linux-gate.sh
boot-linux-gate: PASS (markers: BOOT OK MINIMAL-BOOT-CHECK-PASSED SHELL_ALIVE; exit: 0)
```

## 本轮把"沉默的零"变成可查的东西

`OEMU_TRACE_*` 的三个缺陷都是同一个类型：探针不响，却读成"guest 没执行"。

* PC 钩子原先只有一个调用点在总线派发里，`bti`/`wfi`/`eret` 与函数序言根本不到它，
  于是"在函数入口 PC 上装闩"在函数正常跑的时候一言不发。现在钩在 vCPU 每条指令的入口，
  并且**无条件**调用：用解析后的开关去 gate 它，等于开关永远不会被解析。
* 地址开关用 `strtoull(base 0)` 解析，`System.map` 的裸十六进制被当成**十进制**读成 0 ⇒ 闩永不合。
  现在地址一律按 16 进制读，VA 窗口在 `UINT64_MAX` 处截断而不是回绕（回绕会让窗口拒绝所有访问）。
* 新增 `OEMU_TRACE_HIST`（PC 直方图，撞槽时挤掉计数最小的）与 `OEMU_TRACE_SVC`（记录每次
  supervisor call 的参数与返回值）。两者一次启动就定位了上面的缺陷：直方图指向三条指令的循环，
  syscall 轨迹显示 `write()` 对 fd 8 返回 -EBADF。

## `--smp 4` → `nproc == 4`

这一项**目前无法达成**，且与门禁脚本无关：`oemu boot --smp N`（N>1）现在被 CLI 直接拒绝
（`--smp is not supported yet; one vCPU boots alone (M4b+)`），多核与 IPI 属于排期里的 M4b+ 后续
（`GICD_SGIR` 记录但不产生 SGI、`GICD_SGIR_NS` 未实现等，见 #26 的卫生清单）。脚本因此只在
`--smp > 1` 时才把该选项传下去，避免把门禁变成一条用法错误。

## 相关

* `docs/linux-minimal-qemu.md` —— oracle 侧的最小启动配方与标记来源。
* `docs/verification-strategy.md` —— L4 差分纪律：先在 QEMU 上量，再在同一判据上回放 oemu。
* `docs/roadmap-linux-boot.md` 与 `docs/task-cards/` —— M5 的任务卡与排期。
