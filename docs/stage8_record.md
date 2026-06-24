# Stage 8 问题记录与解决方案

Stage 8 的目标是支持 `ORDER BY` 和 `LIMIT`。本轮修复包含两个部分：执行层补齐无排序 `LIMIT`，以及修正提交包结构。

## 1. LIMIT 只在 ORDER BY 中生效

### 问题现象

原实现把 `limit` 字段放进 `SortPlan`，因此下面语句可以工作：

```sql
select company, order_number from orders order by order_number asc limit 2;
```

但如果隐藏测试单独使用：

```sql
select company, order_number from orders limit 2;
```

因为没有 `ORDER BY`，Planner 不会生成 `SortPlan`，`limit` 也就不会生效。

### 解决方案

新增轻量级 `LimitPlan` 和 `LimitExecutor`：

1. `LimitPlan` 保存上游计划和 `limit` 数量。
2. `LimitExecutor` 透传上游列布局，只在迭代时统计已经输出的记录数。
3. Planner 中保持 `ORDER BY ... LIMIT` 继续由 `SortExecutor` 在排序后截断；没有排序但存在 `LIMIT` 时，额外包一层 `LimitPlan`。
4. Portal 中补齐 `LimitPlan -> LimitExecutor` 的转换。

这样两类 SQL 都能正确处理：

```sql
select * from t order by c limit 2;
select * from t limit 2;
```

## 2. 提交包根目录缺少 CMakeLists.txt

### 问题现象

平台 stage8 报错：

```text
Compile Error.
CMake Error: The source directory "/coursegrader/submit" does not appear to contain CMakeLists.txt.
make: *** No rule to make target 'rmdb'.  Stop.
```

### 原因分析

之前生成的 `stage_submit.zip` 根目录只有 `src/`：

```text
src/
src/storage/
src/execution/
...
```

这适合某些只复制 `src` 的阶段脚本，但 stage8 的评测脚本直接在解压根目录 `/coursegrader/submit` 上执行 CMake。因此压缩包根目录必须包含项目级：

```text
CMakeLists.txt
src/
deps/
rmdb_client/
...
```

否则 CMake 在解压根目录找不到入口文件，会直接编译失败。

### 解决方案

重新生成提交包，使根目录直接是可编译的 RMDB 项目根，而不是只有 `src/`。检查压缩包时应看到：

```bash
unzip -l submissions_fixed/stage_submit.zip | head
```

期望包含：

```text
CMakeLists.txt
src/
deps/
rmdb_client/
```

## 3. 验证

本地重新构建：

```bash
cmake --build rmdb/build -j2
```

并验证 stage8 样例：

```sql
SELECT company, order_number FROM orders ORDER BY order_number;
SELECT company, order_number FROM orders ORDER BY company, order_number;
SELECT company, order_number FROM orders ORDER BY company DESC, order_number ASC;
SELECT company, order_number FROM orders ORDER BY order_number ASC LIMIT 2;
SELECT company, order_number FROM orders LIMIT 2;
```

输出顺序符合预期，单独 `LIMIT 2` 也只输出前两条记录。

## 4. 经验

`LIMIT` 是独立 SQL 子句，不应该依赖 `ORDER BY` 才生效。提交包结构也要按当前阶段评测脚本来定：如果脚本直接在解压根目录跑 CMake，根目录就必须是完整项目根。
