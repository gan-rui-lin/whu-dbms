题目一：存储管理

课程提供的代码框架的基础上，提供相关接口的实现，完成指定功能。本题目包含若干子任务，每个任务对应不同的测试点，只有通过相应测试点才可以获得相应的分数。

提示：

l 测试代码会调用指定接口来进行测试，学生不能修改已有的接口，也不能删除已有的数据结构及数据结构中的变量，但是可以增添新的接口、数据结构、变量。

l 课程提供了项目结构文档来帮助学生理解代码框架，同时在代码注释中，也对数据结构及接口进行了说明，学生可以通过阅读代码注释来辅助理解代码框架。

1、磁盘管理器

在本任务中，学生需要实现磁盘管理器DiskManager的相关接口，磁盘管理器负责文件操作、读写页面等。在完成本任务之前，学生需要阅读项目结构文档中磁盘管理器的相关说明，以及代码框架中src/errors.h、src/storage/disk_manager.h、src/storage/disk_manager.cpp、src/common/config.h文件。

学生需要实现以下接口：

（1）void DiskManager::create_file(const std::string &path);

该接口的参数path为文件名，该接口的功能是创建文件，其文件名为path参数指定的文件名。

（2）void DiskManager::open_file(const std::string &path);

该接口的参数path为文件名，该接口的功能是打开文件名参数path指定的文件。

（3）void DiskManager::close_file(const std::string &path);

该接口的参数path为文件名，该接口的功能是关闭文件名参数path指定的文件。

（4）void DiskManager::destroy_file(const std::string &path);

该接口的参数path为文件名，该接口的功能是删除文件名参数path指定的文件。

（5）void DiskManager::write_page(int fd, page_id_t page_no, const char *offset, int num_bytes);

该接口负责在文件的指定页面写入指定长度的数据，该接口从指定页面的起始位置开始写入数据。

（6）void DiskManager::read_page(int fd, page_id_t page_no, char *offset, int num_bytes);

该接口需要从文件的指定页面读取指定长度的数据，该接口从指定页面的起始位置开始读取数据。

2、缓冲池管理器

在本任务中，学生需要实现缓冲池管理器BufferPoolManager和缓冲池替换策略Replacer相关的接口，缓冲池管理器负责管理缓冲池中的页面在内外存的交换，缓冲池替换策略主要负责缓冲区页面的淘汰和查找。在完成本任务之前，学生需要首先阅读项目结构文档中缓冲池管理器的相关说明，以及代码框架中src/storage和src/replacer文件夹下的代码文件。

对于缓冲池替换策略Replacer，学生需要实现一个Replacer的子类LRUReplacer，LRUReplacer实现了缓冲池页面替换的LRU策略，需要实现的接口如下：

(1) bool LRUReplacer::victim(frame_id_t* frame_id);

该接口的功能是选择并淘汰缓冲池中一个页面。如果成功找到要淘汰的页面，则函数返回值为true；否则，返回值为false。被淘汰的页面所在的帧由参数frame_id返回。

(2) void LRUReplacer::pin(frame_id_t frame_id);

该接口的功能是固定住一个帧中的页面，代表该页面正在使用，不可被换出，参数frame_id指定了帧的编号。

(3) void LRUReplacer::unpin(frame_id_t frame_id);

该接口的功能是取消固定一个帧中的页面，当该页面使用完毕时调用unpin函数取消对该页面的固定，参数frame_id指定了帧的编号。

对于缓冲池管理器BufferPoolManager，学生需要管理缓冲池中的页面，并对缓冲池进行并发控制，需要实现的接口如下：

(1) Page *BufferPoolManager::new_page(PageId *page_id);

该成员函数用于在缓冲池中申请创建一个新页面。如果创建新页面成功，则返回指向该页面的指针，同时通过参数page_id返回新建页面的编号。

(2) Page *BufferPoolManager::fetch_page(PageId page_id);

该成员函数用于获取缓冲池中的指定页面。待获取页面的编号由参数page_id给出。

(3) bool BufferPoolManager::find_victim_page(frame_id_t *frame_id);

当缓冲池中没有可用的空闲帧时，该成员函数用于寻找需要淘汰的页面。如果成功找到要淘汰的页面，则函数返回值为true；否则，返回值为false。被淘汰的页面所在的帧由参数frame_id返回。在实现这个成员函数时，需要调用LRUReplacer::victim函数。

(4) void BufferPoolManager::update_page(Page *page, PageId new_page_id, frame_id_t new_frame_id);

当缓冲池想要把某个帧中的页面置换为新页面或者删除该帧中的页面时，会调用update_page函数，该函数把指定帧中的原有页面刷入磁盘中，并将新页面和该帧建立映射，即更新页表。

(5) bool BufferPoolManager::unpin_page(PageId page_id, bool is_dirty);

当某个操作完成某个页面的使用之后，需要调用该函数将取消该操作对该页面的固定。当所有操作都完成该页面的使用之后，需要在Replacer中调用Unpin函数取消该页面的固定。

(6) bool BufferPoolManager::delete_page(PageId page_id);

用于在缓冲池中删除指定页面，同时将该页面所在的帧置为空闲帧。如果当前页面正在被某个操作使用，则该页面不能被删除。

(7) bool BufferPoolManager::flush_page(PageId page_id);

用于强制刷新缓冲池中的指定页面到磁盘上，无论该页是否正在被使用，或者是否为脏页，都需要把该页面的数据刷入磁盘中。

(8) void BufferPoolManager::flush_all_pages(int fd);

用于将指定文件中的存在于缓冲池的所有页面都刷新到磁盘。

提示：在缓冲池中，需要淘汰某个脏页时，需要将脏页写入磁盘。

3、记录管理器

在本任务中，学生需要填充记录管理器涉及的RmFileHandle类和RmScan类，RmFileHandle类负责对表的记录进行操作，RmScan类用于遍历表文件中存放的记录。

对于RmFileHandle类。在完成本文任务之前，学生需要阅读项目结构文档中记录管理器的相关说明，以及代码框架中src/record文件夹下的代码文件。

学生需要实现的接口如下：

(1) std::unique_ptr`<RmRecord>` RmFileHandle::get_record(const Rid &rid, Context *context) const;

该函数用于获取表中某一条指定位置的记录，每条记录由Rid来进行唯一标识。

(2) Rid RmFileHandle::insert_record(char *buf, Context *context);

该函数负责向表中插入一条新记录，在该函数中未指定记录的插入位置，学生需要选择一个空闲位置插入记录并同步更新表的元数据信息。

(3) void RmFileHandle::insert_record(const Rid &rid, char *buf);

该函数负责向表中的指定位置插入一条记录，该函数主要用于事务的回滚和系统故障恢复。

(4) void RmFileHandle::delete_record(const Rid &rid, Context *context);

该函数负责删除表中指定位置的记录。

(5) void RmFileHandle::update_record(const Rid &rid, char *buf, Context *context);

该函数负责把表中指定位置的记录更新为新的值。

对于RmScan类，学生需要实现的接口如下：

(1) RmScan(const RmFileHandle *file_handle);

该函数为RmScan类的构造函数，需要初始化相关成员变量。

(2) void RmScan::next() override;

该函数负责找到表文件中下一个存放了合法记录的位置。

(3) bool RmScan::is_end() const override;

该函数负责判断是否已经扫描到文件的末尾位置。

---

# 实现记录：尝试、失败与修复

这一节记录本次完成 Stage 1 的实际过程。它不是接口说明，重点是哪些假设有效、哪些假设通过了本地测试但仍然是错的。

## 1. 第一轮实现

实现顺序沿着依赖方向展开：

1. DiskManager：完成文件生命周期、页面分配以及 `pread/pwrite` 页面读写。
2. LRUReplacer：用链表维护顺序，用哈希表支持 frame 的快速移除。
3. BufferPoolManager：连接 free list、replacer、page table 和磁盘读写。
4. Record Manager：完成页内 bitmap、slot、空闲页链表和记录增删改查。
5. RmScan：逐页查找 bitmap 中的有效 slot。

DiskManager 最初可以只检查一次 `pread/pwrite` 的返回值，但更稳妥的实现是处理 `EINTR` 和部分读写，直到请求字节数全部完成。文件接口的错误则转换为项目中的 `FileExistsError`、`FileNotFoundError` 或 `UnixError`。

BufferPoolManager 的实现重点不是某个单独函数，而是同步维护：

```text
PageId ↔ frame_id
frame ∈ free_list / replacer / pinned 三种状态之一
Page 的 pin_count 与 is_dirty
```

Record Manager 则需要同步维护记录字节、bitmap、`num_records` 和空闲页链表。任意一个遗漏都会在后续插入或扫描时暴露。

## 2. 本地测试通过，但隐藏测试失败

第一轮实现运行仓库自带的 5 个测试，全部通过：

- `LRUReplacerTest.SampleTest`
- `BufferPoolManagerTest.SampleTest`
- `BufferPoolManagerConcurrencyTest.ConcurrencyTest`
- `StorageTest.SimpleTest`
- `RecordManagerTest.SimpleTest`

但线上评测是 21 个测试通过、2 个测试失败：

- `BufferPoolManagerTest.IsDirty`
- `BufferPoolManagerConcurrencyTest.HardTest_1`

`IsDirty` 的断言显示：`new_page()` 返回的新页应当是 clean，实际却是 dirty。

`HardTest_1` 则发现：页面修改后使用 `unpin_page(page_id, false)`，经历淘汰再读取，未声明为 dirty 的内容仍被保留了。

两个失败的根因相同。第一轮实现曾在 `new_page()` 中主动执行：

```cpp
page->is_dirty_ = true;
```

当时的想法是“新分配的空白页也应该最终存在于磁盘”，所以预先标脏。这个想法看似保守，实际上破坏了 BufferPool 的接口语义：

- `new_page()` 负责分配、清空和 pin 页面，初始 dirty 应为 false。
- 调用者修改页面后，必须通过 `unpin_page(page_id, true)` 声明修改。
- 使用 `false` unpin 的页面不能因为“它曾经是新页”而被写回。

本地测试全部通过，是因为原有用例在写页面后通常都传入了 `true`，没有检查新页的初始 dirty 状态，也没有覆盖“修改后故意 unpin(false)，再强制淘汰”的路径。

## 3. 修复与最小复现

修复方式是删除 `new_page()` 中预设 dirty 的代码，让 `update_page()` 清理出的 `false` 保持不变。dirty 状态只由真正的修改者传播。

验证使用的最小测试序列是：

```text
磁盘 page 0 先写入全零基线
→ new_page 得到 page 0，断言 is_dirty == false
→ 修改内存内容
→ unpin(page 0, false)
→ 用另一个新页迫使 page 0 被淘汰
→ fetch page 0
→ 断言未声明为 dirty 的修改没有落盘
```

这个测试同时覆盖两个线上失败关心的行为。修复后，最小测试和仓库自带 5 个测试均通过。

## 4. 构建过程中的问题

最初环境缺少 CMake，之后又缺少 Flex、Bison 和 Readline 开发包。工具链补齐后，CMake 能正常完成配置。

工具链完整不等于源码一定能编译。第一次 CMake 构建还发现 `transaction.h` 使用 `std::shared_ptr` 和 `std::make_shared`，却没有直接包含 `<memory>`。补上头文件后，`unit_test` 和整个项目均可由 CMake 构建。

排查构建问题时应先分类：

- CMake 找不到 Bison/Flex/Readline：环境或依赖问题。
- 编译器报告某个 `std::` 类型未定义：源码 include 问题。
- 链接器报告 undefined reference：目标依赖或实现缺失问题。

## 5. 这次实现留下的经验

1. 不要用“更保险”为理由改变接口语义。dirty 是调用者对修改事实的声明，不是页面重要程度的标记。
2. 本地测试通过只说明覆盖到的行为正确，必须主动补充边界和反例。
3. 对 BufferPool 应分别测试 clean/dirty、hit/miss、pinned/unpinned、free/victim 的组合。
4. 并发测试失败不一定首先是锁的问题，应先从失败断言还原它验证的状态转换。
5. 最小复现比反复跑完整随机测试更容易定位根因。
6. 每次 fetch/new page 都检查对应 unpin；每次 frame 复用都检查旧映射、脏页写回和新映射是否形成闭环。
