# RMDB Stage 7：聚合函数

`stage7.md` 要求支持 SQL 聚合函数：

- `COUNT()` / `COUNT(*)` / `COUNT(col)`：统计行数。
- `MAX(col)`：求最大值。
- `MIN(col)`：求最小值。
- `SUM(col)`：求和。

其中 `COUNT`、`MAX`、`MIN` 支持 `int`、`float`、`char` 字段；`SUM` 只支持 `int` 和 `float`。输出列名必须使用 SQL 中的 `as` 别名。

聚合查询和普通查询最大的区别是：普通查询每输入一条记录，可能输出一条记录；聚合查询要先读完整个输入，再输出一条汇总记录。

```text
Scan / Join / Filter
    ↓ 多条记录
AggregateExecutor
    ↓ 一条记录
QlManager::select_from()
```

## 1. 语法层：把函数调用放进 AST

Parser 需要识别下面几类 selector：

```sql
select COUNT(*) as count_row from t;
select COUNT() as count_row from t;
select COUNT(id) as count_id from t;
select MAX(id) as max_id from t;
select MIN(val) as min_val from t;
select SUM(val) as sum_val from t;
```

AST 中可以把聚合函数看作一种特殊列：

```text
AggFunc
├── func_type: COUNT / MAX / MIN / SUM
├── col:       被聚合列，COUNT(*) 和 COUNT() 为空
├── alias:     输出列名
└── is_star:   是否 COUNT(*) / COUNT()
```

这样后续分析器和执行器仍然能沿用“selector 是列集合”的框架，只是遇到 `AggFunc` 时走聚合逻辑。

## 2. 语义分析：别名与被聚合列

`Analyze::do_analyze()` 对普通列会做表名推断：

```sql
select id from t;
```

会变成：

```text
TabCol{tab_name="t", col_name="id"}
```

聚合函数需要分开处理：

- 输出列使用 alias，例如 `sum_id`。
- 被聚合列仍然要检查是否存在，例如 `SUM(id)` 中的 `id`。
- `COUNT(*)` 和 `COUNT()` 没有被聚合列，只需要统计输入记录数。

因此 Query 中的 `cols` 保存的是输出列：

```text
select SUM(id) as sum_id ...
query->cols = [{tab_name="", col_name="sum_id"}]
```

真正要读的源列保存在 `AggFunc` AST 或 `AggCall` 计划节点中。

## 3. 计划层：AggregatePlan

普通 select 的计划通常是：

```text
ProjectionPlan
    ↓
SeqScan / IndexScan / Join / Sort
```

聚合 select 不应该生成普通 ProjectionPlan，因为 alias 不是基表列，ProjectionExecutor 找不到 `sum_id` 这样的列。

聚合查询应生成：

```text
AggregatePlan
    ↓
SeqScan / IndexScan / Join / Sort
```

`AggregatePlan` 保存 `AggCall`：

```text
AggCall
├── func_type
├── col       源列
├── alias     输出列
└── is_star
```

`AggregateExecutor` 根据这些信息决定输出记录的列类型和长度。

## 4. AggregateExecutor 的职责

`AggregateExecutor` 是一个阻塞算子。它在 `beginTuple()` 中消费完整个上游 executor：

```cpp
for (prev->beginTuple(); !prev->is_end(); prev->nextTuple()) {
    auto rec = prev->Next();
    update_aggregate_state(rec);
}
build_one_result_record();
```

之后它只输出一条记录：

```text
beginTuple()
    ↓
is_end() == false
    ↓
Next() 返回聚合结果
    ↓
nextTuple()
    ↓
is_end() == true
```

这种执行模型和普通 scan 不同：scan 的 `Next()` 每次读取当前 tuple；aggregate 的 `Next()` 返回已经计算好的单行结果。

## 5. 输出列元数据

聚合结果也要像普通记录一样交给 `QlManager::select_from()` 打印，所以 executor 必须提供 `cols()`：

```text
SUM(id) as sum_id
    ↓
ColMeta{name="sum_id", type=TYPE_INT, len=4, offset=0}
```

类型规则：

- `COUNT`：输出 `TYPE_INT`。
- `SUM(int)`：输出 `TYPE_INT`。
- `SUM(float)`：输出 `TYPE_FLOAT`。
- `MAX/MIN(col)`：输出类型与源列相同。

偏移量按输出列顺序连续分配：

```text
result record
┌──────────────┬──────────────┐
│ aggregate 0  │ aggregate 1  │
└──────────────┴──────────────┘
```

虽然 stage7 测试多为单个聚合函数，设计上仍可以支持多个聚合列。

## 6. 各聚合函数的计算

### 6.1 COUNT

`COUNT(*)`、`COUNT()` 和 `COUNT(col)` 在本项目没有 NULL，因此都等价于统计输入记录数：

```text
count += 1
```

如果 where 条件过滤后没有记录，结果是 0。

### 6.2 SUM

`SUM` 只允许数值列：

- `TYPE_INT`：累加为整数结果。
- `TYPE_FLOAT`：累加为浮点结果。
- `TYPE_STRING`：应抛出类型不兼容错误。

为了避免中间过程精度太差，可以用 `double` 保存累加值，最终再写回 int 或 float 输出槽。

### 6.3 MAX/MIN

`MAX/MIN` 需要保存“是否已经初始化”的状态：

```text
第一条输入记录：直接复制该列作为当前结果
后续记录：比较后决定是否替换
```

比较规则：

- int：按整数大小。
- float：按浮点大小。
- char(n)：按固定长度字节序比较。

字符串列在记录中是定长、零填充的，比较时要使用列长度，避免越界读取。

## 7. where 条件与聚合的关系

聚合函数只处理已经通过下层 executor 的记录：

```sql
select COUNT(name) as count_name
from aggregate
where val = 2.0;
```

执行顺序是：

```text
SeqScanExecutor 根据 val = 2.0 过滤
    ↓
AggregateExecutor 统计过滤后的记录
```

所以聚合 executor 不需要理解 where 条件；它只信任上游输出。

## 8. 输出格式

`QlManager::select_from()` 根据 executor 的 `cols()` 打印表头，根据 tuple 中的字段类型打印值。

stage7 对格式有两个关键要求：

1. `as` 别名必须和 SQL 一致。
2. float 保留 6 位小数，int 不显示小数。

因此 float 输出建议显式使用：

```cpp
std::fixed << std::setprecision(6)
```

而不是依赖默认流格式。

## 9. 常见错误

### 9.1 把 alias 当成基表列

错误路径：

```text
select SUM(id) as sum_id
    ↓
ProjectionExecutor 查找列 sum_id
    ↓
ColumnNotFoundError
```

修复方式是聚合查询生成 `AggregatePlan`，不生成普通 `ProjectionPlan`。

### 9.2 COUNT(*) 没有列元数据

`COUNT(*)` 不应尝试查找 `*` 这列。它的源列为空，输出列是 alias。

### 9.3 SUM(char) 被错误允许

题目只要求 `SUM` 支持 int 和 float。对 char 做 sum 没有语义，应在执行时抛出类型错误。

### 9.4 空输入没有输出记录

SQL 聚合即使输入为空，也应输出一条聚合结果记录。对于 stage7 常见场景：

- `COUNT` 输出 0。
- `SUM` 可以输出 0。
- `MAX/MIN` 在没有 NULL 语义的简化框架中一般不会专门测试空表，但实现时要避免读取未初始化内存。

## 10. 推荐完成顺序

1. 扩展 lexer/parser，识别聚合函数和 alias。
2. 扩展 AST，保存函数类型、源列和输出别名。
3. 在 Analyze 中检查源列存在性，Query 输出列使用 alias。
4. 增加 `AggregatePlan` 和 `AggCall`。
5. 增加 `AggregateExecutor`，先实现 `COUNT`。
6. 再实现 `SUM`，注意 int/float 输出类型。
7. 最后实现 `MAX/MIN` 和字符串比较。
8. 用 where + count 测试聚合是否只消费过滤后的记录。

调试时可以先确认 executor 的 `cols()` 是否正确。如果表头已经错了，问题通常在 Analyze 或 Plan；如果表头正确但值错误，问题通常在 AggregateExecutor 的状态更新。
