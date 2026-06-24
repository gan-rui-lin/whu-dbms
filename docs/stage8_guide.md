# RMDB Stage 8：ORDER BY 与 LIMIT

`stage8.md` 要求支持：

- 按一个或多个列排序。
- 每个排序列可指定 `ASC` 或 `DESC`。
- 未指定方向时默认升序。
- 使用 `LIMIT n` 限制输出记录数。

排序功能看似只影响输出顺序，但它贯穿 parser、planner 和 executor 三层：

```text
SQL ORDER BY / LIMIT
    ↓
AST 保存排序列、方向、limit
    ↓
Planner 生成 SortPlan
    ↓
SortExecutor 读入上游结果、排序、截断
    ↓
ProjectionExecutor / QlManager 输出
```

## 1. 语法层：多列排序和 limit

题目中的 SQL 形态包括：

```sql
SELECT company, order_number FROM orders ORDER BY order_number;
SELECT company, order_number FROM orders ORDER BY company, order_number;
SELECT company, order_number FROM orders ORDER BY company DESC, order_number ASC;
SELECT company, order_number FROM orders ORDER BY order_number ASC LIMIT 2;
```

AST 中推荐把 `ORDER BY` 表达为一个列表：

```text
OrderBy
└── items
    ├── OrderByItem{col=company,      dir=DESC}
    └── OrderByItem{col=order_number, dir=ASC}
```

`LIMIT` 可以作为 `SelectStmt` 的一个整数属性：

```text
limit = -1 表示没有 limit
limit >= 0 表示最多输出 limit 条
```

默认排序方向可以在 planner 或 executor 中统一解释为升序。

## 2. 语义分析：排序列必须存在

`ORDER BY` 中的列不一定出现在 select list 中，但必须能在输入表中找到。例如：

```sql
select company from orders order by order_number;
```

这是合法的，因为 `order_number` 是 `orders` 表中的列。

因此排序列解析应基于当前查询涉及的所有表列，而不是仅基于投影列：

```text
query->tables
    ↓
收集 all_cols
    ↓
解析 order item 的 TabCol
```

如果排序列未指定表名，且多个表中都有同名列，应抛出 `AmbiguousColumnError`。

## 3. 计划层：SortPlan 的位置

SQL 的逻辑顺序可以简化理解为：

```text
FROM / WHERE
    ↓
ORDER BY
    ↓
SELECT projection
    ↓
LIMIT
```

在本项目中，`SortPlan` 应放在 `ProjectionPlan` 之前：

```text
ProjectionPlan
    ↓
SortPlan
    ↓
SeqScan / IndexScan / Join
```

这样排序 executor 可以看到基表的所有列，即使某个排序列没有出现在最终输出中，也能正常比较。

`SortPlan` 需要保存：

- `subplan_`：排序前的输入计划。
- `sel_cols_`：排序列列表。
- `is_descs_`：每个排序列是否降序。
- `limit_`：输出上限。

## 4. SortExecutor 的执行模型

排序是阻塞算子：它必须先看到所有输入记录，才能知道第一条输出是什么。

`beginTuple()` 的典型流程：

```cpp
tuples.clear();
for (prev->beginTuple(); !prev->is_end(); prev->nextTuple()) {
    tuples.push_back(prev->Next());
}
std::sort(tuples.begin(), tuples.end(), comparator);
if (limit >= 0) tuples.resize(min(tuples.size(), limit));
cursor = 0;
```

之后 `Next()` 和 `nextTuple()` 像扫描内存数组一样工作：

```text
cursor = 0
Next()      -> tuples[cursor]
nextTuple() -> cursor++
is_end()   -> cursor >= tuples.size()
```

这种实现是内存排序，适合本实验的功能测试。真实数据库在结果集很大时会使用外部排序，但 stage8 不要求。

## 5. 多列比较规则

多列排序是字典序比较：

```text
ORDER BY company DESC, order_number ASC
```

比较两条记录 A 和 B：

1. 先比较 `company`。
2. 如果不同，按 `DESC` 决定顺序。
3. 如果相同，再比较 `order_number`。
4. 如果仍相同，保持相等即可。

伪代码：

```cpp
for each sort key i:
    cmp = compare(A[key_i], B[key_i])
    if cmp == 0:
        continue
    if desc[i]:
        return cmp > 0
    else:
        return cmp < 0
return false
```

比较函数按列类型处理：

- `TYPE_INT`：整数比较。
- `TYPE_FLOAT`：浮点比较。
- `TYPE_STRING`：按列长度 `memcmp`。

字符串是定长字段，不能假设一定以 `\0` 结束；打印时可以裁掉尾部零字节，但排序比较应使用字段长度。

## 6. LIMIT 的位置

`LIMIT` 应在排序后生效：

```sql
select * from orders order by order_number asc limit 2;
```

正确流程是：

```text
全部记录按 order_number 排序
    ↓
取前 2 条输出
```

不能先取前 2 条再排序，否则结果会依赖原始表扫描顺序。

在 `SortExecutor` 内部，排序后直接 `resize(limit)` 是一种简单做法：

```cpp
if (limit >= 0 && tuples.size() > limit) {
    tuples.resize(limit);
}
```

如果没有 `ORDER BY` 但有 `LIMIT`，可以额外实现一个 LimitExecutor；stage8 测试中的 limit 与 order by 同时出现，因此放在 SortExecutor 中即可覆盖题目要求。

## 7. 与 ProjectionExecutor 的关系

排序前的 tuple 仍然是下层 scan/join 的完整记录。排序后再投影：

```text
Scan 输出:       company, order_number, other_col ...
Sort 比较:       order_number
Projection 输出: company, order_number
```

如果先投影再排序，而排序列不在 select list 中，就会找不到排序列。因此 planner 中的顺序很关键。

## 8. 输出稳定性

题目没有要求相等 key 的稳定排序。对于以下记录：

```text
company = 'AAA', order_number = 12
company = 'AAA', order_number = 12
```

两条记录谁先输出都可以。比较器在所有排序键都相等时返回 `false`，满足 `std::sort` 对严格弱序的要求。

## 9. 常见错误

### 9.1 只支持一个排序列

原始框架里 `SortExecutor` 只有一个 `ColMeta cols_`。stage8 需要改成列表：

```text
sort_cols_: vector<ColMeta>
is_descs_:  vector<bool>
```

否则 `ORDER BY company, order_number` 会只按第一列或直接解析失败。

### 9.2 DESC 方向作用到所有列

每个排序列都有自己的方向：

```sql
order by company desc, order_number asc
```

不能用一个全局 `is_desc` 表示整条 order by。

### 9.3 LIMIT 在排序前执行

先截断再排序会产生错误结果。应先完整收集和排序，再截断。

### 9.4 排序列表名未推断

`ORDER BY order_number` 中没有表名，planner 必须在当前查询的表集合中推断表名。否则 `get_col_offset()` 会用空表名查找失败。

### 9.5 字符串比较越界

记录中的 char(n) 不一定是普通 C 字符串。排序比较应使用 `memcmp(lhs, rhs, col.len)`。

## 10. 推荐完成顺序

1. 扩展 parser：支持 `ORDER BY col [ASC|DESC], ...` 和 `LIMIT n`。
2. 扩展 AST：用列表保存多个排序项。
3. 扩展 `SortPlan`：保存排序列、方向列表和 limit。
4. 在 planner 中解析排序列的表名，并把 SortPlan 放在 ProjectionPlan 下面。
5. 实现 `SortExecutor::beginTuple()`，先读完上游结果。
6. 实现多列 comparator。
7. 排序后应用 limit。
8. 用单列升序、双列升序、混合方向、limit 四类 SQL 分别测试。

调试时可以先临时打印排序前后的 key 序列。如果 key 序列正确但输出列错误，问题多半在 ProjectionExecutor；如果 key 序列本身错误，问题多半在排序列解析或 comparator。
