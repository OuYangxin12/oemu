# 任务卡：M4b — GICv2 + arch timer + PSCI

> 状态：设计就绪，待 M4a 合入后实现。依据：roadmap M4 节 +
> `roadmap-linux-boot.md` M4b 验收门（timer 校准/jiffies；"panic 但
> 时钟在走"是可测中期门）+ M4 内 ID 寄存器一致性审计。

## 范围

- 做什么：`src/dev/gicv2.c`（distributor + CPU interface，`ICC_*`
  sysreg 状态住 vCPU，固定表优先级扫描）；`src/timer/gtimer.c`
  （CNTFRQ/CNTPCT/CNTV + CVAL 比较 → IRQ pin，**可注入 clock**）；
  `src/fw/psci.c`（SMC/HVC 拦截：VERSION=0x00020000 且带版本协商
  状态机、CPU_ID/COUNT/AFFINITY、SYSTEM_OFF/SYSTEM_RESET、单核
  CPU_ON）；WFI 停步进（D2 yield 点）。
- 不做什么：`--smp 4` 全调度（M5）；vGIC 高级特性（listed/扩展寄存器
  按"未实现寄存器行为成文"规范处理）。

## 验收命令与通过标准

```sh
ctest --test-dir build/debug -R 'Gic|Gtimer|Psci'
ctest --test-dir build/debug -L guest
make test && make asan
# 中期门（无 initrd 内核）：日志出现 timer 校准 + jiffies 走动
# 终门：guest psci_off → oemu exit 0（psci_off.S 已在 QEMU 预验证）
```

## 已就位资产

- `tests/guest/psci_off.S`：QEMU 实测通过（PSCI-OK + exit 0）。oemu
  侧同镜像验收 = M4b 终门。
- `scripts/qemu-oracle.sh`：`--expect-exit 0` 断言 SYSTEM_OFF 语义，
  oemu 侧驱动照抄其断言集。

## 新增测试清单

| 层/风格 | 文件 | 钉住的行为 |
| --- | --- | --- |
| 黑盒 | `tests/unit/test_gicv2.cpp` | 使能/优先级/pending/EOI 流程；固定表扫描次序；SGI/PPI/SPI 注入路径 |
| 黑盒 | `tests/unit/test_gtimer.cpp` | **fake clock 注入**：CVAL 比较→IRQ pin；CNTV/CNTPCT 读数随注入时钟推进；频率可配 |
| 黑盒 | `tests/unit/test_psci.cpp` | SMC/HVC 拦截：VERSION 值、协商前 SYSTEM_OFF→NOT_SUPPORTED(-1)、单核 CPU_ON→INVALID_PARAMETERS、SYSTEM_OFF→machine poweroff→exit 0 |
| 死亡 | `tests/unit/test_psci_check.cpp` | NULL 契约 |
| 白盒 | `tests/unit/test_gicv2_internal.cpp` | 优先级扫描纯函数的判定表（空表/全屏蔽/平优先级） |

- **设计要求（对实现者的硬约束）**：gtimer 必须经一个可注入的 clock
  接口读时间（函数指针或 ops 结构，init 时注入；缺省实现用宿主单调
  时钟）。用真实时钟写 timer 测试 = 间歇性失败之源，此约束先于实现
  写死在卡里。

## Oracle 来源

- [x] QEMU in-model PSCI（D4：拦截 SMC 而非模拟 EL3）；`psci_off` 实测
- [x] timer 频率 62.5 MHz 对齐 QEMU virt 基线（`linux-minimal-qemu.md`）
- [ ] GICv2 寄存器行为：Linux `drivers/irqchip/irq-gic.c`（本地树） +
      QEMU `hw/intc/arm_gic.c` 对照

## 不变量复核

- [ ] IRQ 交付在 quantum 边界检查并尊重 DAIF（M2c 既定语义，回归
      `test_vcpu`）
- [ ] 中断不推进架构状态越过触发指令
- [ ] WFI 是 yield 点不是 NOP

## 风险与回退

- ID 寄存器一致性审计（本里程碑内）：宣传位要么已实现要么内核不可达；
  FP/SIMD=0 与无 FP 路径闭环；产出核对清单，内核日志 `CPU features`
  行旁证。
- PSCI conduit 分歧（SMC vs HVC）：本地 QEMU 8.2.2 实测（M5 基线旗标
  `virtualization=off`）DTB `/psci` method=**hvc**（字节级核对），而
  `linux-minimal-qemu.md` 基线记录内核看到 SMC——两边都可能是真的
  （旗标/版本相关）。因此 `psci_off.S` **运行时探测 DTB 选 conduit**
  而不是硬编码；oemu 侧实现与自产 DTB 的 `/psci` 节点必须一致，
  oracle 断言两边同源。
- **版本协商（QEMU 实测硬行为）**：QEMU 的 PSCI 模型初始按 0.1 处理，
  SYSTEM_OFF 直接返回 -1（NOT_SUPPORTED，x0=0xFFFFFFFFFFFFFFFF）；
  必须先调 PSCI_VERSION(0x84000000) 升级到 0.2+。oemu 的 PSCI 状态机
  照此实现，`psci_off.S` 已按该协议验证（exit 0）。
- FID 构造半字纪律：SYSTEM_OFF=0x84000008 → `movz #0x8; movk #0x8400,
  lsl #16`（低 16 位必须进 movz 半字）。

## 第 1 轮落地：PSCI/SMCCC conduit 修正 + PL011 直通 + ID 诚实化（本提交）

> 背景：PR#24（M4a）合入 master 后，遗留问题是 post-PSCI banner 的
> oops 与被截断的 oops 打印。目标：把内核从"banner 后 oops"推进到
> 深度早期 init。

已实现并全门通过（`make test`/`make asan` 828/828，`tidy` exit 0
新增告警 0，`format-check` 仅 `bench/corpus/k_addsub.c` 预存漂移）：

- **PSCI/SMCCC conduit 重写**（`src/fw/psci.c`、`include/oemu/psci.h`）：
  - FID 常量此前是**猜测值**（旧 SYSTEM_OFF=0x84000002 其实是 CPU_OFF；
    旧 SYSTEM_RESET=0x84000003 是 CPU_ON）。现按客户机自带契约
    `uapi/linux/psci.h` 纠正：VERSION=0x84000000、SYSTEM_OFF=FN(8)、
    SYSTEM_RESET=FN(9)、CPU_SUSPEND/CPU_ON/AFFINITY/FEATURES 齐备。
  - 关键不变量：PSCI/SMCCC-标准/fast 服务空间内的调用**一律被消费**
    （不认识的答 NOT_SUPPORTED），绝不反弹成异常——旧代码把
    FEATURES/fast-0x80000000 弹回 guest，内核 `setup_arch` 即 oops。
  - SMCCC FEATURES(0x80000000/0xC0000000)→v1.0(0x00010000，取自
    oracle dmesg "SMC Calling Convention v1.0")；ARM-Standard owner
    0x47→NOT_SUPPORTED。owner 位域拆成 bits[31:24] class 与
    bits[23:16] owner 两个字节（旧实现把两者混为一谈，误吞
    0x8442xxxx 之类 HyperV owner）。
  - 服务测试 `tests/unit/test_psci.cpp`（11 例）钉住以上全部。
  - 同错还潜伏在测试侧：`test_cli.cpp`/`psci_off.S`/`el1_smoke.S`
    都用错 SYSTEM_OFF 0x84000002（只有"自产自销"的同错测试才过）；
    真内核的 panic-reboot 才暴露。已全部纠正为 0x84000008。
- **PL011 FEN=0 直通**（`src/dev/pl011.c`、`pl011_internal.h`、
  `test_pl011.cpp`）：CR.FEN=0（内核 console driver 从不置 FEN）时
  DR 写**立即**进 sink，FR.TXFE 当场诚实——旧的 64B 环延迟到 slice
  边界才泵，earlycon 在 TXFE 上自旋时环永不排空，panic 打印被截断
  （"Hardwar…"）。FIFO 模式(FEN=1)仍走环，测试按 FEN 分流。
- **ID 诚实化**（`include/oemu/sysreg.h`、`tests/guest/id_probe.S`）：
  `ID_AA64PFR0_EL1` 0x22→**0x10**（EL0=无 AArch32）。oemu 无 AArch32
  解码，旧值 0x22 是谎报，内核据此去读整组 AArch32 ID 寄存器并在
  `smp_prepare_boot_cpu` 的 swapper 上触发同步 Undefined。诚实宣传
  AArch64-only 后内核直接跳过该路径。id_probe.S 扩测 AA32 组，记录
  这处**有意**偏离 oracle（oracle=a53 有 AArch32）。

内核轨迹对比（`boot -kernel guest/build/Image`，同 DTB）：
- 修复前：`Booting Linux` banner → `psci: ...v0.2 function IDs` →
  `Internal error: Oops - Undefined instruction: 2000003`（SMC 弹回）→
  `Kernel panic - not syncing: Attempted to kill the idle task!`，
  console 截断在 "Hardwar"。
- 本提交：一路到 `SLUB: HWalign=64, ...`（含 PSCI 探测全过、
  `Detected VIPT I-cache`、`CPU features`、内存布局、mem auto-init、
  software IO TLB、Memory:、SLUB）。无 oops、无截断。

**下一个墙**（非本提交范围）：SLUB 之后内核在 `free_vmap_area`
红黑树遍历处反复取数据异常（FAR=0x1000027、ESR=0x96000005、
ELR=`free_vmap_area_rb_augment_cb_propagate+0x24`，`ldr x2,[x2,#40]`
命中野指针），且被 printk rate-limit 吞掉、无 oops 外漏。这是一处
**更靠前的执行/MMU 一致性 bug**（破坏了 vmap 堆结构），需要指令级
trace 工具单独立项排查——非 conduit/ID/串口问题。

## 第 2 轮落地：UBFM 回绕即左移修复（本提交）

第 1 轮把内核推到 SLUB 后卡死：`free_vmap_area` 红黑树遍历取到野指针
（FAR=0x1000027、ESR=0x96000005），反复取异常、无 oops 外漏。根因不是
conduit/串口/MMU，而是一条 CPU 指令译码/执行错：

- **`do_bitfield` 把回绕 UBFM（immR>immS）执行成"循环右移"**，正确语义是
  "左移 (regsize-immR) 位、低位移入位强制为 0"。回绕 UBFM 正是 `lsl #n`
  的编码（`lsl #12` = UBFM #52,#51）。旧实现把源的高 n 位旋转回到低 n 位，
  于是 Linux `allocate_slab+0xc8` 的 `lsl x20,x20,#12` 得到 `...fff` 而非
  `...000`——SLUB 对象基址每个都偏 1 字节，freelist 链接指针写歪，第一个
  vmap_area 红黑树遍历就炸。
- 用指令级取证定位：watchpoint 抓到 `allocate_slab` 的 freelist 存指令
  `str x22,[x20,x0]` 落在 `0x..1f/0x..67`（奇数、非 8 对齐）；反汇编 + 寄存器
  trace 精确定位到 `lsl x20,x20,#12` 结果低 12 位应为 0 却是 0xfff。
- 修复只改回绕分支（immS<immR）：`rot = (src << (bits - lsb))`，其后 mask 与
  符号扩展逻辑不变。现有 UBFM/SBFM 回绕测试（len<bits）结果不变（结果掩码
  同样丢弃回绕位），新增 `UbfmWrappedIsShiftNotRotate` 钉住 len==bits 情形。

内核轨迹：修复后一路到 `init_IRQ`（第 35 行 `Root IRQ handler: gic_handle_irq`），
在 `gic_of_init` 的 `readl_relaxed` 处取异常 → oops（这次能完整打印）→
`Kernel panic - not syncing: Attempted to kill the idle task!`。

**下一个墙**：内核走到 `init_IRQ → irqchip_init → gic_of_init`，读 GIC
distributor 寄存器（VA 0xffffffc080360004，oemu 该处无设备）触发数据异常。
这已是 M4b 明确列出的 **GICv2** 子系统范围（`src/dev/gicv2.c`），非 CPU 保真度
问题。下一步实现最小 GICv2（distributor 让 gic_of_init 读到合理 ID、支持
初始化期寄存器读写），使内核越过 init_IRQ 逼近 timer/ init 阶段。

门：make test 829/829、make asan 829/829、make tidy exit 0、format-check 仅
`bench/corpus/k_addsub.c` 预存漂移。

## 第 3 轮落地：GICv2 + arch timer + CRC32 → 抵达 oracle 终态（本提交）

第 2 轮把内核推到 `gic_of_init`，需要 GICv2。本轮补齐 GICv2 + arch timer +
CRC32，内核一路跑到 **init 任务**并复现 QEMU oracle 的**终态**。三个独立提交
（`048689a` gicv2、`1267618` sysreg、`521da15` crc）：

- **GICv2 设备**（`src/dev/gicv2.c`、`include/oemu/gicv2.h`、
  `src/dev/gicv2_internal.h`，`boot_run` 接线）：照抄 qemu `arm_gic`。
  GICD_PIDR2=0x2B 身份、固定 64 线（两组）TYPER、banked
  enable/pending/active/priority/target/config 窗口、优先级扫描 CPU interface
  （IAR 应答最高优先级、EOI 清 active）。未建模偏移读 0/写丢弃，**绝不取异常**
  （正是 `gic_of_init` 里数据异常杀死启动的那处）。SGI/PPI 目标固定本 CPU，
  SPI 目标可写。`oemu_gicv2_internal_scan` 抽成纯判定表（白盒）。
  一处真 bug：`lines_of` 早先返回"组数"(2) 而非"线数"(64)，扫描只会越过 id 1。
- **arch timer 选择子纠正**（`include/oemu/sysreg.h`、`src/sysreg/sysreg.c`）：
  客户机按**它真正发射的字**命中寄存器，不能靠别名表。四个 `_EL1` 选择子此前
  全错（0x07xx 基底匹配不到任何项），每次访问漏表→ hyp-trap oops 死在
  `time_init`。直接从 `vmlinux` 反推：`CNTKCTL_EL1` 真是 **0x0708**（别名会让人
  误以为 0x1f08），而 `CNTVOFF` 与 `CNTP_*` 特权视图折进 **0x1f__** 段。补
  `CNTVCTSS/CNTPCTSS`（自同步计数）与虚拟时钟对 `CNTV_CTL_EL0/CNTV_CVAL_EL0`，
  并把表按选择子**重排升序**（EL1 组移出 0x07xx 槽）。`TPIDRRO_EL0` 曾被标
  RO，但它只是 EL0 只读：`__switch_to` 每次上下文切换清零它，故必须 EL1 可写
  （否则 `msr tpidrro_el0, xzr` oops）。
- **CRC32B/H/W/X**（`src/decode/decode.c`、`src/exec/exec.c`、
  `exec_internal.h`、`include/oemu/decode.h`）：A53 置 `ID_AA64ISAR0_EL1.CRC`，
  内核 crc32 库跑硬件指令，不实现就撞未定义陷阱。照架构 CRC() 伪码
  （反射 CRC-32，poly 0x04C11DB7）实现；数据宽度 = 1<<opcode 低两位，结果 32 位、
  种子 Rn。越过内核自带 crc32 自检即 oracle 确认。

测试：`test_gicv2.cpp`(12 黑盒)、`test_gicv2_internal.cpp`(10 白盒扫描判定表)、
`test_exec_internal.cpp` 增 CRC32 表(12)、`test_decode.cpp` 增 CRC 译码(1)。

**终态达成**（`boot -kernel guest/build/Image`，同 DTB）——oemu 与
`qemu-system-aarch64 -cpu cortex-a53` oracle 逐字一致：
```
Run /sbin/init as init process
Run /etc/init as init process
Run /bin/init as init process
Run /bin/sh as init process
Kernel panic - not syncing: No working init found.
```
内核轨迹：banner → psci → GIC 探测过 → `time_init`/arch_timer 过 →
`Freeing unused kernel memory` → `Switched to clocksource arch_sys_counter` →
`rest_init → schedule → __switch_to`（上下文切换过）→ **init(pid1) 起来**跑
`kernel_init`（crc32 自检过）→ 找不到 init 程序 → 终态 panic。唯一残差是
`CPU features:` 位图行（oracle 多报若干能力位），属**有意**诚实偏离，非终态。

门：make test 865/865、make asan 865/865（较上轮 +36）、make tidy exit 0、
format-check 仅 `bench/corpus/k_addsub.c` 预存漂移。

**下一里程碑（M5）**：busybox initramfs 让 init 真跑起来（当前无 initrd 故
init-not-found 即正确终态）；`--smp 4` 全调度；WFI 停步进的 yield 语义。
timer IRQ 交付（`CNTV_CVAL`→PPI→IAR）本轮未接——内核靠调度器选中 init_task
即达终态，无需 tick；若后续 idle 等 tick 再补 gtimer（可注入 clock 硬约束）。
