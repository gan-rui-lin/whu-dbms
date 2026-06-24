# Stage 4 Guide

## 目标

Stage 4 增加 `DATETIME` 类型，格式固定为：

```text
YYYY-MM-DD HH:MM:SS
```

需要支持：

- 建表声明 `datetime`
- 插入、删除、更新、查询
- 输入合法性检查
- 非法输入写入 `failure`

## 关键实现

- `src/defs.h`
  - 新增 `TYPE_DATETIME`
- `src/common/common.h`
  - `parse_datetime_string()` 负责格式与范围校验
  - `format_datetime_value()` 负责结果格式化输出
  - `Value::set_datetime()` 与 8 字节编码存储
- `src/analyze/analyze.cpp`
  - `STRING -> DATETIME` 在分析阶段完成校验与编码
- `src/execution/executor_abstract.h`
  - 比较逻辑支持 `DATETIME`
- `src/index/ix_index_handle.h` / `src/execution/executor_index_scan.h`
  - 索引比较与范围边界支持 `DATETIME`
- `src/execution/execution_manager.cpp`
  - `select` 输出恢复为 `YYYY-MM-DD HH:MM:SS`

## 不变量

- 只接受长度严格为 19 的时间字符串
- 年份范围固定为 `1000..9999`
- 月、日、时、分、秒都要逐项检查
- 2 月天数必须按闰年规则校验

## 本地验证

统一使用：

```bash
python3 scripts/run_stage2_5_regression.py --build-dir build
```

该脚本会覆盖：

- 合法 `datetime` 的建表、插入、删除、更新、查询
- 非法月份、非法日、非法秒、长度不匹配、越界年份
- `output.txt` 中合法时间值与 `failure` 的落盘

## 线上评测状态

- 本地回归已通过
- 线上评测入口未在仓库中找到，结果待补录
