# 任务卡：M3–M5 测试资产先行落地（本卡 = 本次 PR 自己的卡）

> 依据：`docs/verification-strategy.md` P1（验收先于实现）。M2c 由另一
> 开发线进行中，本任务**不触碰其面**：不改 `src/`、不动 `tests/guest/`
> 构建骨架、不碰 boot CLI。
>
> **状态：已交付并经 QEMU 实测**（QEMU 8.2.2，命令与结果见下）。

## 范围

- 做什么：为 M3a/M3b/M4a/M4b/M5 填制任务卡（设计）；落地**不依赖
  未实现模块**的测试资产——`page_table_builder.h` 及其编码钉测试、
  guest 验收程序（`mmu_smoke.S`/`psci_off.S`）、QEMU oracle 脚本。
- 不做什么：任何生产代码；guest 构建骨架（M2c 交付物，后续并入）；
  CI 改动；引用不存在 API 的测试（会让构建挂掉——测试文件只在被测
  模块落地时创建）。

## 验收命令与通过标准

```sh
make test                                   # 含新增 test_page_table_builder
make asan                                   # 同套件过 ASan+UBSan
make format-check && make tidy              # lint 干净
scripts/build-guest.sh tests/guest/mmu_smoke.S tests/guest/psci_off.S
scripts/qemu-oracle.sh build/guest/mmu_smoke.bin EL1 MMU-OK
scripts/qemu-oracle.sh build/guest/psci_off.bin PSCI-OK --expect-exit 0
```

- 通过标准：四绿；两个 guest 在 QEMU virt 上 oracle 实测通过。
- **实测记录**（2026-09-06，qemu-system-aarch64 8.2.2，
  `-machine virt,virtualization=off -cpu cortex-a53 -m 256M -nographic`）：
  - `mmu_smoke`：`PASS (markers: EL1 MMU-OK; exit: timeout)`——guest
    成功后 WFI park，靠 oracle 超时回收，属设计内结局。
  - `psci_off`：`PASS (markers: PSCI-OK; exit: 0)`——guest 探测 DTB
    （实测 x0=0x48000000，`/psci` method="hvc"），按内核协议先
    PSCI_VERSION(0x84000000) 协商再 SYSTEM_OFF(0x84000008)，QEMU
    以退出码 0 关机。
  - 单元：650/650（632 存量 + 18 builder）100% 通过。

## 新增测试清单

| 层/风格 | 文件 | 钉住的行为 |
| --- | --- | --- |
| 单元 | `tests/unit/test_page_table_builder.cpp` | 描述符位布局（Linux pgtable-hwdef.h 交叉核对）、TnSZ→根级别阈值表、MAIR 编码、APTable 继承位、块/页尺寸、镜像→aspace alias 回读 |

## Oracle 来源

- [x] Linux 6.6 本地树 `arch/arm64/include/asm/pgtable-hwdef.h`
- [x] ARM ARM DDI 0487（TnSZ start-level 语义；注释引用条目名）
- [x] QEMU virt 实测（两个 guest）

## 不变量复核

- [x] 不触及 `src/`（生产代码零改动）
- [x] guest 源不入 CMake 构建（等待 M2c 骨架），不影响 lint/构建面
- [x] 构造器与未来 `src/mmu/` 零共享（独立参照物纪律）
- [x] `.gitignore` 的 vendor 规则锚定为 `/guest/`：未锚定时会把
      `tests/guest/` 的验收 guest 一并吞掉（本次实测踩中）

## 风险与回退

- 与 M2c 的唯一交汇点：`tests/guest/` 目录下的 .S 源文件——M2c 的
  build.sh 落地时按文件纳入即可（或由 M3a 实现时并入），无构建冲突。
- QEMU 路径不可用时：脚本以明确报错退出（exit 3 语义），不产生假绿。
