# Stage 3 Guide

## 目标

Stage 3 为系统增加 `BIGINT` 类型，要求支持：

- `create table` 中声明 `bigint`
- `insert / update / delete / select`
- 溢出检测
- 结果完整写入数据库目录下的 `output.txt`

## 关键实现

- `src/defs.h`
  - 新增 `TYPE_BIGINT`
- `src/common/common.h`
  - `Value` 新增 `int64_t bigint_val`
  - `init_raw()` 支持 8 字节整数
  - `coerce_value_type()` 负责 `INT <-> BIGINT` 与数值提升
- `src/analyze/analyze.cpp`
  - 整数字面量先按 `BIGINT` 处理，再按目标列类型收敛
- `src/execution/executor_abstract.h`
  - 记录比较逻辑支持 `BIGINT`
- `src/index/ix_index_handle.h` / `src/execution/executor_index_scan.h`
  - 索引键比较与边界构造支持 `BIGINT`
- `src/execution/execution_manager.cpp`
  - `select` 输出支持完整打印 `BIGINT`

## 解析层兼容方案

评测环境不一定提供 `flex/bison`。因此实现同时修改了：

- 源定义：`src/parser/lex.l`、`src/parser/yacc.y`
- 生成文件：`src/parser/lex.yy.cpp`、`src/parser/yacc.tab.cpp`

兼容策略是：

- 扫描器将 `BIGINT` 映射到已有的 `INT` 语法分支
- 通过 `scanner_last_type` 记录真实类型
- 在生成 parser 的归约动作中恢复为 `SV_TYPE_BIGINT`

这样即使没有 `bison/flex`，仓库内的 checked-in parser 也能直接编译。

## 本地验证

```bash
cmake -S . -B build
cmake --build build -j"$(nproc)"
python3 scripts/run_stage2_5_regression.py --build-dir build
```

重点校验：

- 大整数可正确插入与查询
- 超出 `int64_t` 范围的字面量会触发失败
- `output.txt` 中保留完整 `BIGINT` 文本，不截断

## 线上评测状态

- 本地回归已通过
- 仓库内暂未发现可直接执行的评测机提交流程，线上结果待补录
