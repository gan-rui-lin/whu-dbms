# Stage 6 问题记录与解决方案

Stage 6 的目标是实现唯一索引、索引查询和索引维护。实际调试过程中，问题主要集中在三类：B+ 树索引结构维护、SQL 执行链路中的索引选择、以及提交平台隐藏 storage 测试触发的底层存储边界问题。

## 1. 提交包结构错误

### 问题现象

第一次提交 `stage6_submit.zip` 后平台直接给 0 分，报错信息中包含：

```text
JSON格式错误!
cp: cannot create regular file '/coursegrader/submit/src/': Not a directory
FileNotFoundError: [Errno 2] No such file or directory: 'src'
```

### 原因分析

测试平台会把压缩包解压后进入 `src` 目录编译和测试，因此压缩包根目录下必须直接包含 `src/`。如果压缩包里多了一层外层目录，或者只打包了零散文件，就会导致平台找不到 `src`。

另一个容易误提交的问题是工作区里存在 `refrence.zip`、`submissions_fixed/` 等本地参考包和生成包，这些文件不应该进入 git。

### 解决方案

1. 重新按平台要求打包，使压缩包根目录直接包含 `src/`。
2. 在根目录 `.gitignore` 中忽略本地参考包、提交包、构建产物：

```text
refrence.zip
reference.zip
submissions/
submissions_fixed/
*_submit.zip
rmdb/build/
```

### 经验

提交前一定先检查压缩包结构：

```bash
unzip -l submissions_fixed/stage6_submit.zip | head
```

期望看到：

```text
src/
src/storage/
src/index/
...
```

## 2. 创建索引时已有数据未正确导入

### 问题现象

在表中已经存在记录后执行：

```sql
create index warehouse(w_id);
select * from warehouse where w_id = 10;
```

可能出现索引查询为空，或者创建索引后只能查到后续插入的数据。

### 原因分析

索引文件创建后，不能只登记元数据，还必须扫描基表，把已有记录逐条插入 B+ 树。否则索引和基表会从创建时刻开始不一致。

### 解决方案

在 `SmManager::create_index()` 中完成以下流程：

1. 根据列名生成 `IndexMeta`。
2. 创建并打开索引文件。
3. 使用 `RmScan` 扫描表中所有已有记录。
4. 对每条记录按索引列顺序拼接 key。
5. 调用 `IxIndexHandle::insert_entry()` 插入 key 和 Rid。
6. 更新表元数据并刷新数据库元数据。

### 经验

索引不是只服务未来数据，而是基表的一个实时派生结构。创建索引的瞬间，需要完成一次全量构建。

## 3. 唯一索引重复键处理不稳定

### 问题现象

插入重复索引键时，可能没有正确报错，或者 B+ 树中出现重复 key，后续查询、删除、更新都会受到影响。

### 原因分析

Stage 6 要求的是唯一索引。B+ 树插入前必须先判断 key 是否已经存在。如果只在结点局部插入时处理，而没有在整棵树查找路径上统一判断，就容易在叶子分裂、联合索引或已有数据建索引时漏掉重复键。

### 解决方案

在索引插入入口 `IxIndexHandle::insert_entry()` 中先调用查找逻辑确认 key 是否存在：

```text
find_leaf_page(key)
    ↓
leaf_lookup(key)
    ↓
存在则拒绝插入
```

插入、创建索引、更新索引时都复用同一套入口，这样唯一性约束集中在 B+ 树层保证。

### 经验

唯一性检查应放在最底层的索引插入入口，而不是散落在 insert/update/create index 的上层流程中。这样上层漏调时也不容易破坏索引结构。

## 4. 联合索引列顺序和 key 拼接错误

### 问题现象

联合索引 `(w_id, name)` 可以创建，但查询结果错误。例如：

```sql
create index warehouse(w_id, name);
select * from warehouse where w_id = 100 and name = 'qwerghjk';
```

可能查不到数据，或者范围查询返回多余记录。

### 原因分析

联合索引的 key 是多个列的原始字节顺序拼接。只要创建索引、插入维护、查询构造边界时的列顺序有任何不一致，就会导致 B+ 树比较结果错误。

例如 `(w_id, name)` 的 key 布局必须始终是：

```text
[w_id bytes][name bytes]
```

不能因为 SQL where 条件写成 `name = ... and w_id = ...` 就按 SQL 出现顺序拼接。

### 解决方案

统一以 `IndexMeta::cols` 中的列顺序构造 key：

1. 创建索引扫描已有记录时按索引元数据顺序拼接。
2. 插入、删除、更新维护索引时按索引元数据顺序拼接。
3. `IndexScanExecutor` 构造 lower/upper key 时按索引元数据顺序拼接。
4. Planner 只负责选择索引和记录命中的索引列，不改变索引物理列顺序。

### 经验

联合索引的 SQL 条件顺序不重要，但索引文件中的列顺序非常重要。所有 key 构造都应该由索引元数据驱动。

## 5. 最左前缀匹配不完整

### 问题现象

有索引时查询没有真正走索引，平台性能测试可能不通过。例如索引 `(id, name, score)` 上：

```sql
select * from A where name = 'abcd' and id = 1;
select * from A where id > 1;
select * from A where id = 1 and name = 'abcd' and score > 90;
```

这些查询应该可以使用索引，但如果只做“条件列集合与索引列完全相等”的判断，就会退化为顺序扫描。

### 原因分析

题目要求使用最左匹配原则，而不是只有完全等值查询才能使用索引。Planner 需要按索引定义顺序分析条件：

1. 连续等值条件可以扩展索引前缀。
2. 第一个范围条件可以作为扫描边界。
3. 范围条件之后的列不能继续缩小索引扫描范围。
4. SQL 中条件出现顺序不能影响匹配。

### 解决方案

在 Planner 中改造索引选择逻辑：

1. 遍历表上的所有索引。
2. 对每个索引按列顺序寻找可用条件。
3. 优先选择匹配前缀最长的索引。
4. 生成 `IndexScanPlan`，把完整 where 条件继续传给执行器做回表过滤。

### 经验

索引扫描只负责减少候选 Rid，不能代替完整 where 判断。即使走了索引，回表后仍要执行全部谓词过滤，才能保证范围条件和非索引列条件正确。

## 6. 索引扫描边界构造错误

### 问题现象

范围查询容易出现边界多查、少查或死循环：

```sql
select * from warehouse where w_id < 534 and w_id > 100;
select * from warehouse where name > 'aszdefgh' and name < 'qweraaaa';
```

### 原因分析

B+ 树范围扫描需要 lower key 和 upper key。如果没有正确区分开闭区间，或者没有为未约束列填充类型最小值/最大值，就会导致 `lower_bound()` 和 `upper_bound()` 得到错误起止位置。

联合索引中还要注意：只有在前置列都是等值条件时，后续列的范围条件才能用于缩小扫描范围。

### 解决方案

在 `IndexScanExecutor` 中统一构造扫描区间：

1. 等值条件：lower 和 upper 都填等值。
2. `>` 或 `>=`：更新 lower。
3. `<` 或 `<=`：更新 upper。
4. 未约束列：lower 填类型最小值，upper 填类型最大值。
5. 扫描出来的记录再用完整条件过滤，修正边界开闭和非索引条件。

### 经验

索引边界可以保守，不能错误。宁愿多扫描一点再过滤，也不能少扫描漏结果。

## 7. 插入、删除、更新时索引未同步维护

### 问题现象

创建索引后执行：

```sql
insert into warehouse values (...);
delete from warehouse where ...;
update warehouse set w_id = ... where ...;
```

随后使用索引查询，可能出现查不到新记录、查到已删除记录，或者更新后新旧 key 都能查到。

### 原因分析

基表记录发生变化时，所有相关索引都必须同步变化：

- insert：向每个索引插入新 key -> Rid。
- delete：从每个索引删除旧 key。
- update：先删除旧 key，再插入新 key。

更新时尤其容易漏掉旧 key 删除，导致索引中残留过期 Rid。

### 解决方案

在执行器中补齐索引维护：

1. `InsertExecutor` 插入基表成功后，为表上每个索引构造 key 并插入。
2. `DeleteExecutor` 删除基表前保存旧记录 key，再从索引中删除。
3. `UpdateExecutor` 更新前保存旧记录，更新后构造新记录 key，依次删除旧索引项并插入新索引项。
4. 如果新 key 违反唯一约束，抛出异常并避免留下半更新索引。

### 经验

索引维护应围绕“记录变化前后两个版本”来写。尤其是 update，它既不是单纯 insert，也不是单纯 delete。

## 8. 删除索引或关闭文件后缓存页残留

### 问题现象

多次创建、删除索引后，后续查询可能读到旧索引内容，甚至出现异常。平台隐藏测试中还可能因为 fd 复用导致非常随机的错误。

### 原因分析

BufferPoolManager 的页表使用 `(fd, page_no)` 作为 key。操作系统关闭文件后，后续打开另一个文件可能复用同一个 fd。如果关闭索引文件时只 flush，不从 buffer pool 移除该 fd 的缓存页，新的索引文件就可能命中旧页面。

### 解决方案

新增并使用 `BufferPoolManager::delete_all_pages(fd)`：

1. 写回该 fd 对应的所有缓存页。
2. 从 page table 中移除这些页。
3. 重置 page 元数据。
4. 将 frame 放回 free list。

在关闭索引文件和记录文件时调用该逻辑，避免 fd 复用污染。

### 经验

只要 page id 中包含 fd，就必须认真处理文件关闭时的缓存失效问题。否则“同一个 fd”不一定代表“同一个文件”。

## 9. storage_test3/storage_test4 服务端停止运行

### 问题现象

平台反馈：

```text
Your program fails the following test cases: [storage_test3] [storage_test4]
In storage_test3, server stops running
In storage_test4, server stops running
```

本地公开的 BufferPoolManager 和 StorageTest 可以通过，但隐藏测试中服务端直接停止，说明问题不是普通结果不匹配，而是底层抛异常或进程崩溃。

### 原因分析

重点排查了 `DiskManager::read_page()` 和 `BufferPoolManager` 的页面换入换出逻辑。原实现中：

```cpp
if (bytes_read <= 0) throw InternalError("DiskManager::read_page Error");
```

这会导致一个边界问题：新分配的 page 可能已经有合法 page_no，但还没有真正写满一个 PAGE_SIZE 到磁盘。如果该页被换出前不是脏页，或者隐藏测试直接读取文件尾之后的页，`pread()` 会返回 0。此时把 EOF 当成致命错误，就会抛异常并导致 server 停止。

数据库页式存储中，读取文件尾之外或短读的新页，更合理的行为是把剩余部分视作 0 填充页。

### 解决方案

修改 `DiskManager::read_page()`：

1. `pread()` 被信号中断时继续读。
2. 真正系统错误才抛异常。
3. 读到 EOF 时，对剩余字节 `memset(..., 0, ...)`。
4. 已读部分保留，未读部分补零。

同时加固 `BufferPoolManager::flush_all_pages()` 和 `delete_all_pages()`：

1. 先收集目标 frame id。
2. 再逐个写回和清理。
3. 避免遍历 `unordered_map` 时边遍历边删除带来的不稳定。
4. `delete_all_pages(fd)` 清理前写回页面，保证关闭文件前数据落盘。

### 验证

本地重新构建并运行：

```bash
cmake --build rmdb/build -j2
./rmdb/build/bin/unit_test --gtest_filter='StorageTest.*:BufferPoolManagerTest.*:BufferPoolManagerConcurrencyTest.*:RecordManagerTest.SimpleTest'
```

通过的测试包括：

```text
BufferPoolManagerTest.SampleTest
BufferPoolManagerConcurrencyTest.ConcurrencyTest
StorageTest.SimpleTest
RecordManagerTest.SimpleTest
```

并且重新更新 `stage6_submit.zip`，确保压缩包中包含本次修复涉及的：

```text
src/storage/buffer_pool_manager.cpp
src/storage/buffer_pool_manager.h
src/storage/disk_manager.cpp
src/storage/disk_manager.h
```

### 经验

隐藏测试的 “server stops running” 往往不是 SQL 语义问题，而是底层异常没有被上层捕获。定位时应优先检查：

1. `assert`
2. 文件读写异常
3. 空指针
4. 缓冲池页表残留
5. fd 关闭和复用

## 10. 本阶段总结

Stage 6 表面上是“加索引”，实际牵涉到存储、索引、优化器、执行器、元数据五条链路。最终稳定通过需要保证：

1. B+ 树内部 key/rid 有序且同步移动。
2. 唯一索引在底层插入入口统一检查。
3. 创建索引时把已有基表数据全量导入。
4. insert/delete/update 都同步维护所有索引。
5. Planner 按最左前缀选择索引。
6. IndexScan 构造保守正确的扫描范围。
7. 回表后继续执行完整 where 过滤。
8. 关闭文件时清理 buffer pool 中对应 fd 的页面。
9. 磁盘短读按零页处理，避免隐藏 storage 测试直接打停 server。

这几个点中，最容易被忽略的是第 7、8、9 点：索引扫描不是最终过滤结果，fd 不是稳定文件身份，EOF 也不一定代表数据库页读取失败。
