# RMDB Stage 6：唯一索引与 B+ 树访问路径

`stage6.md` 要求系统支持唯一索引，包括创建、删除、展示索引，利用索引做单点和范围查询，并在插入、删除、更新基表记录时同步维护索引。

这一阶段可以理解为在原有记录扫描链路旁边增加一条更快的定位链路：

```text
SQL WHERE 条件
    ↓
Planner 选择可用索引
    ↓
IndexScanExecutor 生成索引 key 范围
    ↓
B+ 树叶子结点扫描得到 Rid
    ↓
RmFileHandle 根据 Rid 读取基表记录
```

索引本身不保存完整记录，只保存：

```text
index key = col1 bytes + col2 bytes + ...
value     = Rid(page_no, slot_no)
```

因此索引查询最终仍然要回表读取记录。索引的正确性依赖两个条件：**key 的字节拼接必须和索引元数据一致，Rid 必须指向基表中真实存在的记录。**

## 1. 类与职责

### 1.1 IndexMeta：描述一个索引的逻辑结构

`IndexMeta` 位于 `src/system/sm_meta.h`，保存在数据库元数据中。它记录：

- `tab_name`：索引所属表。
- `col_num`：索引列数。
- `col_tot_len`：拼接 key 的总长度。
- `cols`：每个索引列的 `ColMeta`，包含类型、长度和在记录中的 offset。

联合索引的列顺序非常重要。例如 `(w_id, name)` 和 `(name, w_id)` 是两个不同索引，因为 key 的字节布局不同：

```text
(w_id, name): [4 bytes int][8 bytes char]
(name, w_id): [8 bytes char][4 bytes int]
```

`TabMeta::is_index()` 和 `get_index_meta()` 都按这个顺序匹配索引。

### 1.2 IxManager：管理索引文件生命周期

`IxManager` 负责把索引元数据转换成一个独立的 `.idx` 文件：

- `get_index_name()` 根据表名和索引列生成文件名，例如 `warehouse_w_id_name.idx`。
- `create_index()` 初始化索引文件头、叶子链表头页和根页。
- `open_index()` 返回 `IxIndexHandle`。
- `close_index()` 写回索引文件头和缓冲页。
- `destroy_index()` 删除索引文件。

索引文件前几个页面有固定含义：

```text
page 0: IxFileHdr
page 1: leaf header，叶子链表哨兵
page 2: 初始 root，同时也是初始 leaf
```

这一阶段要特别注意 fd 复用问题：关闭索引文件时不能只 flush 缓冲页，还要把该 fd 对应的页从 BufferPoolManager 的 page table 中移除。否则删除一个索引后新建另一个索引，如果操作系统复用了同一个 fd，`(fd, page_no)` 可能命中旧缓冲页。

### 1.3 IxFileHdr 与 IxPageHdr：B+ 树物理布局

`IxFileHdr` 保存整棵 B+ 树的文件级信息：

- `root_page_`：根结点页号。
- `first_leaf_`、`last_leaf_`：叶子链表范围。
- `col_types_`、`col_lens_`、`col_tot_len_`：key 比较需要的类型和长度。
- `btree_order_`：每个结点可容纳的最大 key 数。
- `keys_size_`：结点内 key 数组占用的字节数。

每个 B+ 树页面内部布局如下：

```text
Page::data_
┌───────────┬──────────────────────┬──────────────────────┐
│ IxPageHdr │ key[0..max_size - 1] │ rid[0..max_size - 1] │
└───────────┴──────────────────────┴──────────────────────┘
```

在叶子结点中，`rid[i]` 是基表记录位置。在内部结点中，`rid[i].page_no` 表示子结点页号。

### 1.4 IxNodeHandle：一个 B+ 树结点的操作视图

`IxNodeHandle` 不拥有页面，它只是把 `Page::data_` 解释成 B+ 树结点。核心操作包括：

- `lower_bound(key)`：找到第一个 `>= key` 的位置。
- `upper_bound(key)`：找到第一个 `> key` 的位置。
- `leaf_lookup(key, &rid)`：叶子结点精确查找。
- `internal_lookup(key)`：内部结点选择下一层子页。
- `insert_pairs()`、`erase_pair()`：移动 key/rid 数组。
- `insert()`、`remove()`：结点内唯一键插入和删除。

节点内数组维护的基本不变量是：

```text
key[0] <= key[1] <= ... <= key[num_key - 1]
rid[i] 与 key[i] 一一对应
```

插入和删除都要同时移动 key 数组和 rid 数组。只移动其中一个，索引就会返回错误 Rid。

### 1.5 IxIndexHandle：整棵 B+ 树的查找、插入、删除

`IxIndexHandle` 是 B+ 树的主入口：

- `find_leaf_page()` 从根向下找到目标叶子。
- `get_value()` 做唯一键精确查找。
- `insert_entry()` 插入 key 和 Rid，必要时分裂。
- `delete_entry()` 删除 key，必要时合并或重分配。
- `lower_bound()`、`upper_bound()` 返回可供 `IxScan` 使用的叶子位置。

如果结点满了，`split()` 会把右半部分搬到新结点；然后 `insert_into_parent()` 把新结点的第一个 key 插入父结点。根结点分裂时，需要创建新的内部根。

### 1.6 IxScan：沿叶子链表遍历索引范围

`IxScan` 保存当前 `Iid(page_no, slot_no)` 和结束 `Iid`：

```text
lower <= iid < upper
```

每次 `rid()` 用当前 `Iid` 从索引叶子读取 Rid；`next()` 让 slot 前进一步，当前叶子结束时跳到下一片叶子。

`IxScan` 只关心索引叶子范围，不判断 SQL where 条件。因此 `IndexScanExecutor` 还要对回表记录做一次谓词过滤。

### 1.7 SmManager：把索引接入元数据和基表

`SmManager::create_index()` 做三件事：

1. 根据列名生成 `IndexMeta`。
2. 创建并打开索引文件。
3. 扫描基表已有记录，把每条记录的 key 和 Rid 插入索引。

`drop_index()` 要关闭索引句柄、删除索引文件、从 `TabMeta::indexes` 中移除元数据，并刷新数据库元数据。

`show_index()` 按题目格式输出：

```text
| table_name | unique | (column_name,column_name) |
```

注意联合索引列名之间没有空格。

## 2. 索引 key 的构造

索引 key 是多个字段原始字节的顺序拼接。对于 `(w_id, name)`：

```cpp
offset = 0;
memcpy(key + offset, rec.data + w_id.offset, w_id.len);
offset += w_id.len;
memcpy(key + offset, rec.data + name.offset, name.len);
```

比较时也按相同顺序逐列比较：

1. 先比较 `w_id`。
2. 如果相等，再比较 `name`。
3. 所有列都相等，才认为 key 相等。

这个规则自然支持联合索引的字典序，也就是 SQL 中常说的最左前缀。

## 3. 最左前缀匹配

对于索引 `(a, b, c)`，可以使用索引的条件包括：

- `a = 1`
- `a > 1`
- `a = 1 and b = 2`
- `b = 2 and a = 1`
- `a = 1 and b = 2 and c > 3`

不能直接用这个索引起步的条件是：

- `b = 2`
- `c > 3`
- `b = 2 and c = 3`

Planner 的任务是从表的全部索引里挑一个“匹配前缀最长”的索引，并按索引定义顺序返回 `index_col_names`。where 条件在 SQL 中出现的顺序不重要，匹配时应按索引列顺序重新组织。

生成范围边界时，规则可以理解为：

```text
等值列：lower 和 upper 都填该值
范围列：lower 或 upper 填范围值，另一边填类型最小/最大值
后续列：填类型最小/最大值
```

例如 `(w_id, name)` 上有条件：

```sql
w_id < 600 and name > 'bztyhnmj'
```

真正能用于索引定位的最左列是 `w_id < 600`。`name > ...` 不是最左等值列之后的范围列，所以它不能缩小索引扫描起点，但可以在回表后继续过滤。

## 4. 索引查询执行流

### 4.1 选择 IndexScanPlan

Planner 对每张表的条件做拆分：

```text
全局 where 条件
    ↓ pop_conds()
当前表自己的过滤条件
    ↓ get_index_cols()
SeqScanPlan 或 IndexScanPlan
```

如果 `get_index_cols()` 找到可用索引，就生成 `T_IndexScan`；否则退回 `T_SeqScan`。

### 4.2 IndexScanExecutor 初始化范围

`IndexScanExecutor::beginTuple()` 根据条件构造 `lower_key` 和 `upper_key`，然后：

```cpp
Iid lower = ih->lower_bound(lower_key);
Iid upper = ih->upper_bound(upper_key);
scan_ = std::make_unique<IxScan>(ih, lower, upper, bpm);
```

扫描过程中，每个索引项先得到 Rid，再回表读取记录：

```text
IxScan::rid()
    ↓
RmFileHandle::get_record(rid)
    ↓
eval_conds()
```

即使索引边界比较宽，只要回表过滤正确，最终结果仍然正确。

## 5. 索引维护

### 5.1 插入

插入基表记录时，顺序是：

1. 根据表字段构造 `RmRecord`。
2. 插入记录文件得到 `Rid`。
3. 对表上每个索引构造 key。
4. 先查重，保证唯一索引约束。
5. 插入 `(key, Rid)` 到 B+ 树。

如果唯一性检查失败，需要删除刚插入的基表记录，避免基表和索引不一致。

### 5.2 删除

删除时必须先读取旧记录，因为删除索引需要旧 key：

```text
Rid
 ↓ get_record()
old record
 ↓ build key for each index
delete index entry
 ↓
delete base record
```

如果先删基表，再想构造 key，就已经丢失了索引字段值。

### 5.3 更新

更新最容易出错，因为可能有些索引列改变，有些没改变。推荐流程：

1. 读取旧记录。
2. 复制旧记录得到新记录，并应用 set 子句。
3. 对每个索引分别构造 old key 和 new key。
4. 如果 key 变化，先检查 new key 是否已经存在。
5. 删除 old key，插入 new key。
6. 最后更新基表记录。

唯一性检查要在任何真实修改之前完成，否则中途抛异常会留下半更新状态。

## 6. 关键不变量

1. `TabMeta::indexes` 中存在的每个索引，都应该在 `SmManager::ihs_` 中有打开的句柄。
2. 每个索引文件名由表名和索引列顺序唯一确定。
3. key 构造顺序必须与 `IndexMeta::cols` 完全一致。
4. B+ 树叶子结点中的每个 Rid 都必须指向记录页，而不是文件头 page 0。
5. 插入、删除、更新基表时，所有相关索引都要同步维护。
6. 删除索引时，元数据、索引句柄、索引文件和列的 `index` 标记要一起更新。
7. 关闭文件时，要清理缓冲池中该 fd 的页面，避免 fd 复用命中旧页面。
8. `lower_bound` 和 `upper_bound` 返回的区间应满足左闭右开。

## 7. 推荐调试顺序

1. 先测试单字段索引的 create/show/drop。
2. 再测试单字段等值查询。
3. 再测试单字段范围查询。
4. 再测试联合索引等值查询。
5. 再测试最左前缀和乱序 where 条件。
6. 最后测试 insert/delete/update 后索引是否仍然正确。

出现错误时，可以按下面路径逆向排查：

```text
查询结果错误
    ↓
回表过滤是否正确
    ↓
IxScan 返回的 Rid 是否正确
    ↓
B+ 树叶子 key/rid 是否对应
    ↓
create/insert/update/delete 时 key 构造是否一致
```

如果出现 `Page 0 in table ... not exists`，通常说明索引里保存了非法 Rid，或者缓冲池命中了旧索引页。优先检查索引关闭、删除和重建路径。
