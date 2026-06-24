# Stage 2 Guide

## 目标

Stage 2 负责把 RMDB 从“只具备底层存储能力”推进到“可通过 SQL 完成基础 DDL / DML / DQL”。当前实现已经覆盖：

- `create table` / `drop table`
- `insert` / `update` / `delete`
- 单表 `select`
- 基于条件过滤的查询
- 多表嵌套循环连接
- 非法 SQL 与语义错误统一输出 `failure`

## 关键模块

- `src/analyze/analyze.cpp`
  - 把 AST 转成 `Query`
  - 完成列解析、条件检查、`update` / `insert` 的值类型校验
- `src/optimizer/planner.h`
  - 规划扫描、过滤、投影、连接和索引扫描的生成路径
- `src/execution/`
  - 负责真正执行 `SeqScan / IndexScan / Projection / NestedLoopJoin / Update / Delete / Insert`
- `src/execution/execution_manager.cpp`
  - 负责把查询结果同时写回客户端与数据库目录下的 `output.txt`
- `src/rmdb.cpp`
  - 统一捕获解析错误、执行错误，并向 `output.txt` 追加 `failure`

## 本阶段需要保持的不变量

- `output.txt` 是评测主产物，任何失败路径都必须写入 `failure`
- `Projection` 的输出列顺序必须与 `select` 列表一致
- `where` 条件中列和值的类型必须先对齐，再进入执行层
- `update` / `insert` 的原始字节串在分析阶段就要按目标列类型构造好

## 本地构建与回归

在 `rmdb/` 根目录执行：

```bash
cmake -S . -B build
cmake --build build -j"$(nproc)"
python3 scripts/run_stage2_5_regression.py --build-dir build
```

回归脚本会：

- 启动 `build/bin/rmdb`
- 通过本地 socket 发送 stage2-5 的代表性 SQL
- 校验客户端响应
- 校验 `build/stage2_5_regression_db/output.txt`

## 可复现产物

- 数据目录：`build/stage2_5_regression_db/`
- 评测输出：`build/stage2_5_regression_db/output.txt`

## 已修复的典型问题

- 解析错误此前不会写 `failure`，现在统一落盘
- 整数字面量统一提升为 `BIGINT` 后，`score > 90` 这类 `FLOAT` 比较会报类型不兼容；现已在分析阶段补齐数值类型提升

## 线上评测状态

- 当前仓库内未提供可直接执行的评测机提交流程、脚本或凭据
- 本地回归已通过
- 线上评测结果待拿到提交入口后补录
