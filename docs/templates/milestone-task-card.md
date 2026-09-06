# 任务卡：<里程碑/PR 名>

> 用法：动工前复制本模板并填写"范围 / 验收命令 / 测试清单 / oracle"
> 四节——这四节是开工的前提（纪律 P1），其余随 PR 更新。填写时删除
> 本说明与示例行。

## 范围

- 做什么：
- 不做什么（防范围蔓延，写清边界）：

## 验收命令与通过标准（动工前先写死）

```sh
# 示例：ctest --test-dir build/debug -L guest
# 示例：make test && make asan
```

- 通过标准：

## 新增测试清单

| 层/风格 | 文件 | 钉住的行为 |
| --- | --- | --- |
| 黑盒 | `tests/unit/test_xxx.cpp` | |
| 白盒 | `tests/unit/test_xxx_internal.cpp` | |
| 死亡 | `tests/unit/test_xxx_check.cpp` | |
| guest | `tests/guest/xxx.S` + 驱动 | |

- `tests/CMakeLists.txt` 注册项与标签：
- 测试支撑新增（构造器 / fake clock / …）：

## Oracle 来源（期望值从哪来，P2）

- [ ] llvm-mc / clang 语料（条目：）
- [ ] QEMU 行为对照（对象：）
- [ ] ARM ARM 引用（条目：）
- [ ] 独立构造器 / 黄金样本：

## 不变量复核（P4，合入前打勾）

- [ ] 纯 C11；步进路径零分配
- [ ] 精确异常：交付时不越过触发指令
- [ ] 一切分配经 `oemu_allocator` seam
- [ ] `make test` / `make asan` / `make format-check` / `make tidy` 全绿
- [ ] 新模块行覆盖 ≥97%（`make coverage-summary`）
- [ ] 失败操作不留痕迹；DECODE / UNSUPPORTED 文案可区分（P5）

## 风险与回退

- 主要风险：
- 失败时的回退/缩小方案：

## 完工记录（P6：只写实际跑过的）

- 实际运行的配置与结果（真实数字）：
- 遗留问题 / 跟进项：
