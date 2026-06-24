# Stage 7 问题记录与解决方案

Stage 7 的目标是支持 `COUNT`、`SUM`、`MAX`、`MIN` 聚合函数。本轮修复主要针对平台停在 `aggregate_test1` 的问题。

## 1. 聚合列别名丢失

### 问题现象

平台提示：

```text
Error. Stopping at aggregate_test1
```

本地排查聚合链路时，发现 `SUM(id) as sum_id` 这类查询依赖 `AggFunc::alias` 生成输出列名。如果 alias 为空，后续分析器和执行器会生成异常的结果列元数据，隐藏测试容易直接中断。

### 原因分析

`AggFunc` 构造函数原来先把 `alias_` move 到父类 `Col` 的 `col_name`，随后又从 `col_name` move 到 `alias`：

```cpp
Col("", std::move(alias_)),
alias(std::move(col_name))
```

这种写法依赖被移动后的字符串状态，行为不稳定。不同环境下可能导致 `alias` 为空或异常，进而使聚合输出列名不符合 SQL 中的 `as` 别名。

### 解决方案

改成先用 alias 的拷贝初始化父类列名，再把原始 alias move 到 `AggFunc::alias`：

```cpp
Col("", alias_),
alias(std::move(alias_))
```

这样 `AggFunc` 同时保留：

1. 父类 `Col::col_name`，用于沿用原有 selector 框架。
2. `AggFunc::alias`，用于 `AggregatePlan` 和 `AggregateExecutor` 生成输出列元数据。

## 2. 空输入聚合结果未初始化

### 问题现象

如果 `where` 条件过滤后没有记录，`COUNT` 能正确写入 0，但 `SUM`、`MIN`、`MAX` 的结果记录缓冲区原先没有清零。虽然公开样例不一定覆盖这个边界，但隐藏用例可能读到未定义内存。

### 解决方案

在 `AggregateExecutor::beginTuple()` 创建结果记录后立即清零：

```cpp
result_ = std::make_unique<RmRecord>(len_);
memset(result_->data, 0, len_);
```

这保证未被输入记录初始化的聚合槽位有确定值，不会把随机内存写入 `output.txt`。

## 3. 验证

本地重新构建：

```bash
cmake --build rmdb/build -j2
```

并用服务端和客户端验证了 stage7 样例：

```sql
select SUM(id) as sum_id from aggregate;
select SUM(val) as sum_val from aggregate;
select MAX(id) as max_id from aggregate;
select MIN(val) as min_val from aggregate;
select COUNT(*) as count_row from cnt;
select COUNT() as count_empty from cnt;
select COUNT(id) as count_id from cnt;
select COUNT(name) as count_name from cnt where val = 2.0;
```

`output.txt` 中输出：

```text
| sum_id |
| 9 |
| sum_val |
| 20.000000 |
| max_id |
| 5 |
| min_val |
| 4.500000 |
| count_row |
| 3 |
| count_empty |
| 3 |
| count_id |
| 3 |
| count_name |
| 2 |
```

## 4. 经验

聚合函数的输出列并不是基表列，`as` 别名必须作为一等元数据保留下来。构造 AST 节点时不要从已经 move 的字符串继续取值，否则这类问题会在不同评测环境中表现得很随机。
