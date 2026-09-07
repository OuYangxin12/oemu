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

D-M4a-2 **PL011 寄存器面**：DR/FR/CR/IBRD/FBRD/LCR_H/IMSC/RIS/MIS/ICR +
  PERIPH* 表（0xFE0-0xFFC，ARM-pureCell 身份证 0x00/0x00/0x11/0x14）；
  其余偏移读 0 写忽略（**不**发 Data Abort——PL011 解码整页）。TX：写
  DR 且 TRE 门控——CR.TE=0 时**丢弃**（实证观察 4：与 QEMU 一致，内核
  使能前丢字节；对齐后写此处明文）。RX 钩子 `oemu_pl011_inject(dev,
  byte)`：16 项 RX 环满则返回 OEMU_ERR_FULL（调用方丢字节并清 FE）。
  中断面（MIS/RIS/IMSC）：设备只维护状态位，**不接 IRQ 线**（M4b GIC
  接入后由 machine 拉线）。

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

D-M4a-5 **DTB 地址**：QEMU virt 实测 x0=0x48000000 = ram_base +
  ram_size/2（256M 配置）。oemu 同式：`dtb_addr = mem_base +
  (mem_base==0x40000000 ? ram/2)`；镜像/initrd 撞车时右移一帧。boot
  协议：x0=DTB PA、PSTATE=M_EL1、MMU/cache 复位态（现状已满足）。

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

D-M4a-8 **PSCI 最小拦截（仅够 psci_off 门）**：boot 场景在 vcpu 步进
  循环里拦 `HVC #0`：funcid 0x84000002 SYSTEM_OFF → 干净退出 0；
  0x84000000 VERSION → 0x00010000? （实测 QEMU PSCI 1.0 返回
  0x00010000）；其余 NOT_SUPPORTED(-1)。住 `src/fw/psci.c` 骨架
  （M4b 扩 CPU_ON/RESET）。

D-M4a-9 **TX 策略与 EOT 退役**：M2c stopgap UART 与 EOT sentinel 随
  `oemu boot` 改造一并删除——psci_off 门用真 PSCI SYSTEM_OFF 停机，
  不再需要魔法字节；el1_smoke.bin 的 EOT 依赖改由 PL011 + HVC
  SYSTEM_OFF 重编（guest 资产同步改，oracle 重验）。
