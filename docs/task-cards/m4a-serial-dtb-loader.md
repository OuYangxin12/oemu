# 任务卡：M4a — PL011 + DTB + Image loader + boot 协议

> 状态：设计就绪，待 M3 合入后实现。依据：roadmap M4 节 +
> `roadmap-linux-boot.md` M4a 验收门（内核 earlycon "Booting Linux"）。

## 范围

- 做什么：`src/dev/pl011.c`（DR/FR/CR/IBRD/FBRD/LCR_H/IMSC/RIS/MIS/ICR；
  16 项预分配 TX 环；RX 注入钩子）；`src/fdt/fdt.c`（纯函数 FDT
  builder）；`src/kernel/image.c`（AArch64 Image 头：magic `ARM\x64`、
  text_offset、image_size、IPA-size 标志诚实拒绝）；`oemu boot` 完整
  参数面（`-kernel/-dtb/-m/-append/--smp/--serial`）。
- 不做什么：GIC/timer/PSCI（M4b）；RX 的宿主 stdin 接入（M5）；SMP
  调度（M5）。

## 验收命令与通过标准

```sh
ctest --test-dir build/debug -R 'Pl011|Fdt|Image|Boot'
make test && make asan
# L3 第一门（镜像存在时）：
scripts/build-guest.sh tests/guest/psci_off.S                                      # oracle 侧
scripts/qemu-oracle.sh build/guest/psci_off.bin PSCI-OK --expect-exit 0
# oemu 侧等价驱动（实现 test_boot_smoke 扩展时落地）
```

- 通过标准：真实内核 earlycon 输出 "Booting Linux"（镜像缺失 CI 干净
  跳过）；`psci_off` guest 在 QEMU 已预验证（见已就位资产）。

## 已就位资产

- `tests/guest/psci_off.S`——完整 AArch64 Image（含 Image 头），已在
  QEMU virt 上验证：EL1 进入、PL011 输出、**运行时探测 x0 传入的
  DTB**（实测 x0=0x48000000，method=hvc）并按内核协议先
  PSCI_VERSION 协商再 SYSTEM_OFF，QEMU 退出码 0。产物路径
  `build/guest/psci_off.bin`（`scripts/build-guest.sh` 生成）。
  它同时是 M4a Image loader 的**现成合法样本**与 M4b PSCI 的验收
  程序——一份资产钉两个里程碑。

## 新增测试清单

| 层/风格 | 文件 | 钉住的行为 |
| --- | --- | --- |
| 黑盒 | `tests/unit/test_pl011.cpp` | 寄存器行为、TX 环满/空、RX 注入、IMSC/MIS 中断位；经 aspace MMIO 驱动 |
| 黑盒 | `tests/unit/test_image.cpp` | 字节级构造 Image 头（新 `image_builder.h`，elf_builder 模式）：magic、text_offset、标志拒绝矩阵 |
| 黑盒 | `tests/unit/test_fdt.cpp` | 结构块/字符串块/根节点正确性；期望值经独立 reader 或 dtc 对照 |
| 死亡 | `tests/unit/test_pl011_check.cpp`、`test_image_check.cpp` | NULL 契约 |
| fork | `tests/unit/test_cli.cpp`（扩展） | `oemu boot` 参数面、退出码契约 |

- 测试支撑新增：`tests/support/image_builder.h`；fdt 对照用的微型
  reader（若 dtc 不可用则跳过对照用例，`GTEST_SKIP` 写明缺什么）。

## Oracle 来源

- [x] QEMU virt PL011 行为；`psci_off` 实测输出
- [ ] 早到字节策略：与 QEMU 对齐后写成模块头注释（实证观察 4：内核
      使能 UART 前的字节被丢弃；测试喂入时序据此对齐）
- [ ] Image 头字段：Linux `Documentation/arch/arm64/booting.rst`
      （本地树有）

## 不变量复核

- [ ] 设备回调零分配；TX 环 init 预分配
- [ ] 未实现 MMIO 寄存器行为成文（读返 0 / 写忽略 / Data Abort，逐个注明）
- [ ] loader 拒绝路径全部返回 `oemu_status`，不留半加载状态

## 风险与回退

- DTB 与 ID 寄存器、机器布局三者不自洽 → Linux 误判特性：fdt 测试
  与 ID 审计（M4b 内）交叉核对 `/cpus`、`/psci` 节点值。
- earlycon 不出字节的排障顺序：boot 协议（x0=DTB PA）→ UART MMIO →
  早期异常向量；L3 门天然把这三者拆成可独立排查的链路。

## 实现决策（动工前定稿，2026-09-07）

D-M4a-1 **模块与头**：三模块 `src/dev/pl011.c`、`src/fdt/fdt.c`、
  `src/kernel/image.c`，公共头 `include/oemu/{pl011,fdt,image}.h`
  （vcpu/boot 都要装配，属公共 API）；纯函数决策进各自
  `<module>_internal.h`。

D-M4a-2 **PL011 寄存器面（oracle 实测修订，tests/guest/pl011_probe.S
  @ QEMU 10.2.1）**：偏移与语义逐字抄 QEMU hw/char/pl011.c 的可观测面：
  DR/FR/ILPR/IBRD/FBRD/LCR_H/CR/FIFLS/INTIM(=IMSC)/ICR/IMSC(ro
  alias)/RIS/MIS/DMACR*；PID@0xFE0.. = 0x11,0x10,0x14,0x00（**实测**
  ——教科书 0x34 是 QEMU 不实现的），PCL@0xFD0.. 全 0；其余偏移读 0
  写忽略（不发 Data Abort）。复位值实测：FR=0、**CR=0x90
  (TXE|LOOPBACK)**、FIFLS=0、RIS=0。TX：写 DR **无条件透传**（实测：
  CR=0x00/UARTEN=0 时写的 'A' 照样出现——QEMU 透传不看使能位；
  LOOPBACK=1 时 TX 字节同时回环 RX 环）。原"TX 使能门控丢弃"决策
  **作废**——实证观察 4 的丢弃只对 host→guest RX 方向成立。RX：
  `oemu_pl011_inject(dev, byte)`：RXE/UARTEN 关闭 → OEMU_ERR_STATE
  （调用方丢字节），16 项环满 → OEMU_ERR_FULL；注入置 RIS.RXIS。
  中断面：RIS/IMSC/MIS(=RIS&IMSC)/ICR(写1清)/INTIM(写1清全 RIS，抄
  QEMU)；irq 电平 `oemu_pl011_irq_level()` 查询，M4b 前无人拉线。
  TX 环 16 项预嵌入结构体（零分配）；泵 `oemu_pl011_pump` 由 boot
  每轮调用排空到 sink（stdout 或测试捕获器）。

D-M4a-3 **FDT builder**：单缓冲 append-only API：`begin_node/prop_u32/
  prop_u64list/prop_str/...` + `finish` 产出 blob 长度；容量 init
  预分配（seam），满返回 OEMU_ERR_NO_MEMORY 对象完好；字符串去重表
  可选（先不去重，正确性优先）。生成器必须可自反读（配套
  `oemu_fdt_internal_get` 微型 reader 供测试对照与 boot 自检）。

D-M4a-4 **DTB 布局与 QEMU virt 对齐**（oracle：10.2.1 dumpdtb 实测，
  存 `build/m4-oracle/virt.dts`，不入库）：root compatible
  "linux,dummy-virt" #a/#s=2；memory@40000000（device_type=memory）；
  cpus/cpu@0（cortex-a53，reg=0）；intc@8000000（arm,cortex-a15-gic，
  reg 双帧，#interrupt-cells=3，interrupt-controller）；timer（arm,
  armv8-timer，4×PPI 13/14/11/10 flags 0x104，always-on）；psci
  （arm,psci-1.0，**method="hvc"** 与 QEMU virt virtualization=off
  一致——roadmap 写 "SMC conduit" 与此冲突，按 P2 实证优先取 hvc，
  psci_off.S 本身探测 DT 选路不受害）；pl011@9000000（interrupts =
  0 1 4 → SPI 33，clocks=<&osc>，配套 fixed-clock 24MHz osc@8000000?
  ——时钟节点用 QEMU 同构 osc24m）；chosen（stdout-path、bootargs=
  -append 值）。phandle 自有小编号（1=intc、2=cpu、3=osc），不抄 QEMU
  编号——自洽即可。

D-M4a-5 **DTB 路线（2026-09-07 用户拍板，方案 B）**：QEMU 的
  `--append dtb` 生成 + `-dtb` 覆盖使 boot 路径**不需要运行时 builder**。
  M4a boot 使用**提交的参考 blob**（tests/fixtures/boot.dtb：由
  build/m4-oracle/virt.dtb 裁剪——memory/psci/chosen/aliases 改写为
  oemu 布局后交 dtc 重编与再校验）；`oemu boot -dtb <file>` 覆盖可用；
  `include/oemu/fdt.h`/`src/fdt` 保留为内部工具（builder 与 reader）。
  ~~互操作差异~~ **谜底已揭**（2026-07，对照 dtc 1.7.2 源码）：v17 头必须
  在 0x20/0x24 写入 `size_dt_strings`/`size_dt_struct`（非零）、
  `last_comp_version`@0x18=16；空 rsvmap 为 16 字节（8 字节终止条目 +
  8 字节对齐 pad，rsv@0x28、struct@0x38）；PROP 体词序 dlen 先、nameoff
  后。builder 按此重写后 dtc/fdtget 全量校验通过，互操作恢复；boot DTB
  仍用 fixture（dtc 编 .dts），builder 保留备用。

D-M4a-6 **Image loader**：头 64B（booting.rst）：magic `ARM\x64`@0x38
  LE、text_offset@0x8、image_size@0x10（LE64）、flags@0x30。拒绝矩阵
  全部 `oemu_status`：bad magic→INVALID_IMAGE、flags 要求的
  PAGE_SIZE(4K=bit0)/PHYS(LE=bit1)/KASLR(bit2→**接受并忽略**，我们
  永不重定位？不：诚实拒绝 KASLR 位?tinyconfig 不带——拒绝并注因)、
  BE(bit3)→拒绝、image_size≤0 或 >ram→REFUSE。text 读入
  `mem_base+0x40080000? 不：**加载 PA = mem_base + text_offset**，
  entry = 该 PA（booting.rst：text_offset 相对加载基址；QEMU 加载
  0x40080000 = base+0x80000 与此一致）。initrd（M5）预留参数位。

D-M4a-7 **oemu boot 参数面**：`-kernel <img>`（必填）、`-append
  "<cmdline>"`（进 chosen/bootargs）、`-m <MiB>`（默认 256）、`-dtb
  <file>`（外部 DTB 覆盖生成器）、`--serial <path>`（TX 落盘；缺省
  stdout）。`--smp` 拒绝并注 M4b+。旧 `--entry` 保留（覆盖 loader
  推导值）。

D-M4a-8 **PSCI 最小拦截（仅够 psci_off 门）**：exec 的 HVC/SMC 分支
  先过 env 的新 seam `fw_call`（有则消费，无则走现有异常）；boot 装
  psci-driven env。功能面：0x84000000 VERSION → **0x00010001（实测
  QEMU 10.2.1 probe：PSCI-VER=00010001，M4b 卡的 0x00020000 作废
  ——oracle 说了算）**；0x84000002 SYSTEM_OFF / 0x84000003 SYSTEM_RESET
  → 置 halted（boot 循环看到 halted → exit 0 / 重启）；CPU_ON(0xC4000003)/
  CPU_OFF(0x84000002)…M4b 扩，本版回 NOT_SUPPORTED(-1)。conduit：
  DTB 写 method="smc" 并接 SMC——QEMU 的 hvc 由它的 EL3 固件实现，
  oemu 无 EL3，直接以 SMC 拦截履行同一契约（guest 探测 DTB 自选，
  两端可观测行为一致：exit 0）；probe 资产冻结不动。住 `src/fw/
  psci.c`。

D-M4a-9 **TX 策略与 EOT 退役**：M2c stopgap UART 与 EOT sentinel 随
  `oemu boot` 改造一并删除——psci_off 门用真 PSCI SYSTEM_OFF 停机，
  不再需要魔法字节；el1_smoke.bin 的 EOT 依赖改由 PL011 + HVC
  SYSTEM_OFF 重编（guest 资产同步改，oracle 重验）。

## 完工记录（2026-09-08，实现落地）

模块与参数面按 D-M4a 全部落地：`src/dev/pl011.c`、`src/fdt/fdt.c`、
`src/kernel/image.c`、`src/fw/psci.c`、`src/main.c` 的 `oemu boot` 重写。

### 验收门实测（本机，debug preset）

```
ctest -R 'Pl011|Fdt|Image|Boot'            100% (95)，仅 FdtTest.DtcAgrees... 跳过（dtc 不在 PATH，属既有跳过语义）
make test                                 100% (811)
make asan                                 100% (811)   ASan+UBSan 全过
make format-check                         我的文件全绿；唯一命中 bench/corpus/k_addsub.c（本机 clang-format 21.1.8 与 CI 版本漂移，非本次改动，依既有约定不动语料）
make tidy                                 exit 0（告警类别与既有 mmu.c/fdt.c 同类，非 -Werror）
```

L3 全链 e2e（本机）：

```
oemu boot -kernel build/guest/psci_off.bin --max-insns 5000000
  → PSCI-OK
  → PSCI-SMC
  → exit 0
```

boot → 装载 fixture DTB（x0=0x60000000，运行时校验 magic/totalsize）→
guest 探测 /psci method → PL011 打出 PSCI-OK/PSCI-SMC → SMC VERSION 协商
→ SMC SYSTEM_OFF(0x84000002) 被 boot_fw_call 消费 → powerdown(0) → exit 0。

### 新增测试（用例只增不减）

- `tests/support/image_builder.h`：逐字节 booting.rst Image 头/文件构造器（elf_builder 模式）。
- `tests/unit/test_image.cpp`（INTERNAL）：parse 接受矩阵（每 LE 页大小、text_offset 0..2MiB-1、size=0）、拒绝矩阵（坏 magic、非分支 code0、每个 BE 位→UNSUPPORTED、未对齐/≥2MiB offset）、load 到真机总线读回、截断→FORMAT、超 RAM→RANGE、NULL→INVALID_ARG、真实 psci_off.bin 装载（缺则跳过）。
- `tests/unit/test_image_check.cpp`（death）：`phys->write==NULL` 触发 OEMU_REQUIRE。
- `tests/unit/test_pl011.cpp`（INTERNAL，经 aspace MMIO 驱动）：复位面（CR=PL011_CR_RESET、FR=0、DR 空读 0）、PID 11/10/14/00、PCL 全 0、未实现偏移静默；TX 无条件透传、按序排空、满环丢最旧计数；BUSY/TXFE 翻转；RIS.TIEM 置位、RIS 写忽略、ICR 清位；irq 电平随 mask；RX 注入未使能→STATE、注入读回退休 RLIS、满环→FULL；loopback 回环；配置寄存器往返。
- `tests/unit/test_pl011_check.cpp`（death）：init/pump/irq_level 的 NULL 契约。
- `tests/unit/test_cli.cpp` Boot 面按真协议重写（真 Image 头 + SYSTEM_OFF/BRK 终止 + 复位态断言）。

### 与设计的偏差（实证驱动）

1. **TX 环 16 → 64**（`include/oemu/pl011.h`）：模型在 run-loop 切片边界才排空 TX，一段未泵出的 guest banner 会静默丢最旧字节；64 容纳横幅，溢出丢最旧仍作最后防线并计数。
2. **guest probe 重写为健壮版**：`tests/guest/psci_off.S` 的 FDT walk 改为 do-while 边界检查 + 立即数比较（去掉热循环里的 `adr str` 与无界名字解引用——旧版在 oemu 精确异常下 data-abort，QEMU 平坦地址空间侥幸容忍）。probe 现对 method="smc"/"hvc" 均正确解析；committed fixture 为 smc，e2e 门走 SMC。
3. **el1_smoke.S 退出协议改 PSCI SYSTEM_OFF**（D-M4a-9 兑现）：加 `.balign 0x1000` 使 Image 头 text_offset 与文件自洽，EOT sentinel 退役。
4. **`raw_image`/Image 头字段偏移修正**：boot 测试的字节构造头此前把 magic 放到 0x30；按 booting.rst 校正到 0x38，text_offset 取 0x80000（钉默认入口 0x40080000）。
5. **boot 增加 entry 越 RAM 拒绝**：`--entry` 落在 RAM 窗外时按 caller 错误 exit 1，而非让 guest 在无主地址上取指崩溃。
6. **CR 复位值钉模型常量**：test_pl011 断 `PL011_CR_RESET`（代码实测 TXE|LBE）；本卡片正文 "0x90" 为早期笔误，以实现与 oracle 一致的常量为准。

### 不变量复核

- [x] 设备回调零分配；TX/RX 环预嵌入结构体（`oemu_pl011` 内联数组）。
- [x] 未实现 MMIO 偏移行为成文：读返 0、写忽略、绝不 Data Abort（pl011_read/write default 分支 + 注释）。
- [x] loader 拒绝路径全部 `oemu_status`，半加载前返回（parse 先于任何 bus 写）。
- [x] boot 全路径 `goto done` 统一 fclose/free/dispose，无泄漏（clang-analyzer 的 `serial!=stdout` 守卫为误报）。

### L3 真内核引导进展（本轮，linux-6.6.156 tinyconfig Image）

目标：真内核 earlycon 打 "Booting Linux"。oracle（QEMU virt 同一
Image+DTB）确认能出横幅并跑到 "No working init"（无 initrd，预期）。

本轮为把 oemu 推到"真能在自己 MMU 下建页表、读 ID/调试寄存器、跑通用
定时器、进 earlycon"而落地的**正确且必要**的修复：

- `src/mmu/mmu.c`：walk 分派改为**按层级**——type 0b11 在 level 0..2
  是表、在 level 3 是 4KiB **页**（旧实现把 0b11 一律当表，于是内核
  每一条真实 L3 PTE 都被判成坏表 → fixmap/vmalloc 全崩）。配套把
  `MmuTest.TableDescriptorBelowThePageLevelFaults` 更正为
  `LastLevelTableBitsResolveAsAPage`（末级 0b11=页，spec 正解）。
- AT（地址翻译）：`OEMU_EXEC_SYS_AT`（stage-1 S1E*，op1==0）执行真
  走表并写 `PAR_EL1`（`.get` 行，sel 0x03a0）；stage-2 S12E*(op1==4)
  仍诚实 Undefined。`VcpuTest` 的 AT 用例由"Trap Undefined"更正为
  "publishes PAR_EL1"。内核 `at s1e1r; mrs par; tbnz par,#0` 探测路
  由此打通。
- `src/dev/pl011*`：寄存器面按 TRM/驱动更正（FR.TXFE=0x80、TXFF=0x20，
  删幻影 INTMASKSET/CLR）——早期 earlycon 轮询 FR 的两条 while 需要
  正确 TXFE/TXFF 才不死锁。
- 通用定时器（M4b 的最小子集，boot 必需）：CNTFRQ/CNTPCT/CNTVCT/
  CNTVOFF/CNTP_CTL/CVAL/TVAL/CNTKCTL sysreg 行 + `oemu_sysregs.cntvct`
  随步进的计数器（`src/vcpu/vcpu.c`）。没有 CNTVCT，内核
  `__delay_cycles`/calibrate 的忙等会因未定义而**无限自旋**卡死早期引导。
- ID/调试寄存器：ID_AA64PFR1/2、ZFR0、SMFR0、DFR1/2、AFR0、ISAR2..5、
  MMFR2/3 与 MDSCR_EL1 以 F_WI（读 0/忽略写）入表，内核启动探测不再 trap。
- `src/kernel/image.c`+`image_internal.h`：Image 头按 booting.rst 重写
  （接受 BTI 头式 0xd503201f；endian bit0 + pages 字段 [2:1]；16K/64K
  诚实 UNSUPPORTED）。`tests/support/image_builder.h`/`test_image.cpp`
  随此 spec 更正（LE 16K/64K 现期望 UNSUPPORTED；镜像 flag 镜像用例改写）。
- `src/decode`+`src/exec`：`MSR #imm`（SPSel/DAIF，`OEMU_OP_MSR_IMM`
  +`do_msr_immediate`）与 PRFM/字面量 load 的 HINT no-op 化。内核早期
  `msr daifset/daifclr` 必需。

门禁：`make test`/`make asan` 100%（811）；`make format-check` 我的文件
全绿（唯 bench/corpus/k_addsub.c 本机 clang-format 版本漂移，属既有，不动
语料）；`make tidy` exit 0。

**L3 门仍未闭合**（诚实记录）：内核已在 oemu 下深跑到 printk/panic
路径，但**未打出横幅**。当前卡点是内核自身早期页表构造里的一处 oops
级联——首个异常是 `__create_pgd_mapping` 处一条 `WARN_ON((phys^virt)
& ~PAGE_MASK)`（可存活），随后内核在 `prb_reserve`（printk 环形缓冲
预约）处踩 **level-1 translation fault**（FAR=0x802d88b0，随迭代递增）：
printk 的 log buffer 指针处在一个未被任何页表覆盖的低位 VA，于是每次
printk 自陷 → oops 再陷 → 最终停在 `panic()` 的忙等（tx_emitted=0）。
根因指向 oemu 对内核早期自身结构的仿真保真度仍有缺口（怀疑与线性映射
/ 内核写回读一致、或早期映射被上述 WARN 跳过的组合有关），属 M4b 量级
（GIC/timer-IRQ/更忠实 paging）的工作，非单点可修，故本轮如实记录为未完。

### L3 续：ID 保真度（oracle 校准到 Cortex-A53）

关键发现：内核 panic 前算出的线性映射是错的、prb_reserve 取到野指针。
用新探测 guest `tests/guest/id_probe.S`（QEMU `-cpu cortex-a53`）实测 oracle
的 ID 集，纠正了 oemu 谎报的 CPU 身份：

- oracle 横幅行本身印 `Booting Linux ... [0x410fd034]` —— 即 Cortex-**A53**。
  oemu 之前谎报 A76（MIDR 0x411FD080）+ 拼凑的 ID_AA64* 集。
- `ID_AA64MMFR0_EL1` 旧值 0x0FF00021（VA_BITS=36）是错的；oracle =
  **0x1122**（VA_BITS=**42**、PA_BITS=42）。VA_BITS 决定内核 TCR/线性映射
  布局，错一位就整片线性映射错位 → prb_reserve 野指针 → panic。
- 一并校准：MIDR 0x410FD034、REVIDR 0x100、PFR0 0x22、ISAR0 0x11120、
  DFR0 0x10305106、CTR_EL0 0x84448004、CLIDR 0x0A200023。
- VA_BITS 变 42 后 walk 起始层级变 0，内核跑进早期 `__cpu_setup`，它写
  TCR2/PIR*/DISR/OSL*/PMUSERENR/AMUSERENR——oemu 缺这些 → MSR/MRS trap
  Undefined 卡死。按 RAZ/WI 补齐（对应 FEAT 均已在 ID 里报"无"）。

结果：引导推进过 `__cpu_setup`，抵达 `create_pgd_mapping`。

**仍未出横幅**（诚实）：`create_pgd_mapping` 处 BRK WARN（x0=pgdir=0、
x1=phys=0、x2=virt=0x40000000000001，一个 phys=0 的假 fixmap 槽，可存活）；
随后 `prb_reserve+0x17c` 解引用 `x0=0x802d88b0`。而 `printk_rb` 静态结构本身在
合法高 VA（`x19=0xffffffc080280850`）。`0x802d88b0` 恰是合法内核 VA
`0xffffffc0802d88b0`（= phys 0x402d88b0 的线性映射）**砍掉高 32 位**的结果——
即某处把运行时 64 位指针截断成 32 位（截断的 store / 或 32 位宽的地址算术）。
定位需一条对 QEMU 的**总线写日志差分**（oemu 侧记录 setup_log_buf 前后对
printk_rb 那几十字节的每次写，比对是哪个 width/指令丢的高位），属更深的
写路径保真度排查，下一轮继续。

### L3 续：找到并修复真正的拦路石——UMADDL 加数宽度

真内核引导此前"卡死"的根因终于定位并修复（commit `43c27ec`）：

**根因**：`{S,U}MADDL/MSUBL` 的加数（第三源操作数 Ra）是**完整 64 位**，
不是 32 位字。oemu 把 Ra 按 W32 读，再符号/零扩展——于是 64 位加数的高 32 位
被静默丢弃。只有当加数是真正的 64 位值时才会暴露，而内核最常见的惯用法
"按元素大小缩放索引再累加一个基址指针"正好如此。

**为什么这条杀死引导**：printk 环的 `to_desc()` = `umaddl x0,w2,w1,x0`，
以环指针（`0xffffffc0802d88b0`）作加数。加数高位被截 → 描述符地址塌成低半
`0x802d88b0` → `prb_reserve` 解引用野低地址 → level-1 翻译故障 → oops →
panic，横幅永远印不出来。修复加数宽度后 `prb_reserve` 通过，内核一路跑到
init/idle 阶段（实测致命 prb 故障已消失，只剩一个 create_pgd brk）。

**补了回归测试**：现有加宽测试的加数都很小（<2^32），所以一直没照出这个洞；
新增用例把 `0xffffffc0..` 级 64 位加数灌进 smaddl/umsubl。

**修正对旧现象的判读**：`create_pgd_mapping+0x104` 的那个 brk **不是可存活
的 WARN，而是致命 BUG**——brk 处理器 → `die()` → `panic()` → 在 pid-0 idle
任务上 `make_task_dead`，故打印 "Attempted to kill the idle task!"。因为它死在
`paging_init` 的线性映射阶段、**早于 console_init**，所以内核从始至终没碰过
PL011（实测设备 0 次访问）——之前"跑到 idle"其实是 die→panic 的表象。

**下一轮拦路石（精确）**：致命的 `__create_pgd_mapping_locked`（mmu.c:393
`WARN_ON((phys^virt)&~PAGE_MASK)`）在 `paging_init` 线性映射时被以
`x1=phys=0, x2=virt=0x40000000000001, x3(size)=0xfffffffdfdffe000,
x4(prot)=0xfffffffdffdfe000` 调用——这组参数明显错乱（size/prot 是 fixmap
高地址、virt 还带着诡异的 bit0）。像是某个 oemu 指令截断/污染了 caller 的参数
寄存器（与 UMADDL 同类的"窄读"问题，可能藏在 fixmap/线性映射的早期地址算术里，
如 `fix_to_virt` / `__phys_to_virt` 路径，或某条 add/adrp/mov 的宽度）。下一步
沿 create_pgd 调用者帧链上溯，定位是哪条指令把 virt 变成 0x40000000000001。

### L3 续：create_pgd 致命 brk 已定位为 fixmap 路径的指令执行 bug

沿 `__primary_switched` 帧链上溯，致命 brk 的调用者是
`early_fdt_map → fixmap_remap_fdt → create_mapping_noalloc →
__create_pgd_mapping_locked`（内核早期把 FDT 映射进 fixmap）。

已用一次性探针**逐一排除**了引导 ABI / DTB 本身的问题：
- 入口实测 `x0 = 0x48000000`，`read@x0 = 0xd00dfeed`（合法 FDT magic），
  booting.rst 确认 **x0=DTB phys 是正确约定**（不是 x1）。
- `early_fdt_map` 入口实测 `x0 = x21 = 0x48000000`（FDT 物理地址**完好传到这里**）。

即：DTB 地址一路正确送到 `early_fdt_map`。但在
`fixmap_remap_fdt`→`create_mapping_noalloc`→`__create_pgd_mapping_locked` 内部，
到 brk 时参数变成 `phys(x1)=0`（原 `0x48000000`）、`virt(x2)=0x40000000000001`
（`__fix_to_virt(FIX_FDT)` 的错误值）。`create_pgd` 因此 `WARN_ON` → die → panic。

结论：这是**又一条被 oemu 执行错的指令**，症状与 UMADDL 同类（把地址/指针算错），
藏在 fixmap/`pgd_offset_pgd`/`__fix_to_virt` 的地址算术里。phys 由 `0x48000000`
变 0、`__fix_to_virt` 返回 `0x40000000000001` 都是错乱迹象。

**下一轮精确动作**：把指令级 trace 限定在 `fixmap_remap_fdt`(0x…8005?)、
`create_mapping_noalloc`、`__create_pgd_mapping_locked`(0xffffffc08001c…)、
`pgd_offset_pgd` 的地址上，逐条与 QEMU oracle（同 `-cpu cortex-a53`）对比，
定位是哪条指令（疑 adrp/adr_l、`bfi/ubfm/sbfm` 宽度、或 `__fix_to_virt` 里
对 fixaddr 基址的读取）算错了地址。修好它，`early_fdt_map` 成功，内核应能
进入 `console_init` 并首次打印 earlycon 横幅。

### L3 再续：更正前判 + 精确锁定（本轮实测，未改 src）

**推翻前一轮的"参数被污染"假说**：那条 brk 的 `phys=0 / virt=0x40000000000001`
是我早先在别的执行点抓的，误导了方向。本轮逐条单步实测：

- 用**相同** Image+DTB 在 QEMU（`-cpu cortex-a53 -accel tcg`）实跑，能一路
  印到横幅（`Booting Linux …` + `earlycon: pl11 at MMIO 0x9000000`），故当前
  拦路石 100% 是 **oemu 侧**的 bug，不是 DTB/装载/ABI/内核配置。
- fixmap noalloc 的 `__create_pgd_mapping` 入口参数**完全正确**
  （`pgdir=0x…80341000 phys=0x48000000 virt=0xfffffffdfddfe000 size=0x1000`），
  且其首个 PUD 迭代正常走"复用已建表"分支——**参数没有被污染**。
- 致命 brk 实为 `alloc_init_pte` 里的 `BUG_ON(!pgtable_alloc)`：控制流是
  `+0x36c`(c2cc `ldr x1,[x21]`; `and/cmp/b.eq` 未命中) → `c2ec cbnz x1` 未跳
  （因 x1=0）→ `c30c cbz x0`（x0=`[sp,#152]`=pgtable_alloc=**NULL**）→ brk。
  即 noalloc 变体在此处**需要新建一张 PTE 表却无分配器**。
- 该处 `x21=0xfffffffdfdc3a770`；oemu 翻译读回 **0**（status=0，无故障）。手动
  按总线下走同样的表链得到同一叶 PTE（`0xe8000040305703`→页落 `0x40305000`，
  偏移处内容确为 0），`and …,#3=3` 亦正确。**读取无误**：这个 PMD 槽在物理内存
  里就是 0，即 `early_fixmap_init` 没把整段 fixmap 的这张 PTE 表建全。

**关键新事实**：`TCR_EL1.T1SZ = 25` → 内核实际以 **VA_BITS=39**（3 级：
start_level=1，PGD/PMD/PTE）运行，**不是**我之前经 `ID_AA64MMFR0_EL1=0x1122`
宣称的 42 位。A53 本就只支持 39 位 VA / 40 位 PA，该 ID 值可疑（待与 QEMU 对
齐）。因此页表几何与 fixmap 自映射须在 39 位下被正确演练。

**结论（当前精确拦路石）**：内核早期把整段 fixmap 预映射（`early_fixmap_init`
给每个子区间填了 PTE 表），随后 `early_fdt_map` 的 `create_mapping_noalloc`
理应"零分配"地复用这些表。但在 oemu 里，FDT 所在的这个 PMD 槽读回 0 →
noalloc 无路可走 → `BUG_ON` → die → panic（早于 console_init，故仍无横幅）。
下一步：判定是 `early_fixmap_init` 的一条 **store** 在 oemu 里落到了错的物理页
（写入 `pmdp` 用的 fixmap 别名地址），还是 oemu 对**自映射 fixmap VA**（把页表
自身再映射一次的 39 位 fixmap 别名）的**翻译**与 QEMU 不同。

**可复现的取证通道（本轮验证）**：
- gdb 单步 QEMU 需 `aarch64` 远程目标，本环境只有 x86 原生 gdb（拒 aarch64），
  **不可用**；`-gdbstub` 需 `-accel tcg` 才开 TCP。
- 可用通道：`-d exec`（每 TB 记 guest PC，已能确认 QEMU 跑过
  `early_fixmap_init`/`fixmap_remap_fdt`/`__create_pgd_mapping` 全链）；
  `-monitor tcp:…,server` 可用（`x/1gx <VA>` 走 QEMU 真 MMU，能验 `…dfe000`
  读到 FDT magic `0xd00dfeed`，但 create_pgd 期那个瞬态 fixmap 别名已拆除）。
- 建议：加一条 `-plugin`（TCG 指令级 trace）或找一份 aarch64 gdb 做逐条比对。
