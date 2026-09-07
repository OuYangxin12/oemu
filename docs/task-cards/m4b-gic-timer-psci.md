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
