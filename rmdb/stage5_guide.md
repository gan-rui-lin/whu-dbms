# Stage 5 Guide

## 目标

Stage 5 要求实现唯一索引，并保证：

- 支持单列与多列索引
- 支持 `create index` / `drop index` / `show index`
- 查询遵循最左匹配原则
- 插入、删除、更新时同步维护索引
- 唯一约束冲突时拒绝写入并输出 `failure`

## 关键模块

- `src/system/sm_manager.cpp`
  - 建索引、删索引、展示索引
- `src/index/`
  - B+ 树索引存储与查找
- `src/optimizer/planner.h`
  - 识别可用索引，生成 `IndexScan`
- `src/execution/executor_index_scan.h`
  - 根据条件构造索引扫描边界
- `src/execution/executor_insert.h`
  - 插入前检查唯一性并同步写索引
- `src/execution/executor_update.h` / `executor_delete.h`
  - 更新、删除时同步维护索引项

## 当前实现要点

- 支持单列索引和联合索引
- `show index from table_name` 输出格式为 `| table | unique | (col1,col2) |`
- 查询支持最左前缀匹配与范围扫描
- 唯一约束由索引层在写入时校验

## 本地验证

```bash
python3 scripts/run_stage2_5_regression.py --build-dir build
```

脚本会验证：

- `show index`
- 单点查找
- 范围查找
- 联合索引最左匹配
- 重复键插入失败
- `output.txt` 中索引元信息与失败输出

## 已确认的问题修复

- 早期 parser 错误不会写 `failure`，会影响索引相关隐藏用例；现已统一由 `src/rmdb.cpp` 处理
- 联合索引下的查询条件顺序已能在规划阶段匹配到可用索引

## 线上评测状态

- 本地回归已通过
- 评测机提交入口和凭据未在仓库内提供，线上结果待补录
