# Stage 0：构建与测试 RMDB

本文说明如何准备环境、构建 RMDB、运行已有测试，以及为自己的实现补充测试。以下命令默认从仓库的 `rmdb/` 目录执行。

## 1. 准备工具链

RMDB 使用 C++17、CMake、Flex/Bison 和 Readline。Ubuntu/Debian 可以一次安装：

```bash
sudo apt update
sudo apt install -y build-essential cmake flex bison libreadline-dev
```

确认工具可用：

```bash
g++ --version
cmake --version
flex --version
bison --version
```

“CMake 找不到 BISON/FLEX”属于环境问题；编译器指出类型、头文件或返回值有问题，则属于源码问题，应分开排查。

## 2. 使用 CMake 构建

配置项目：

```bash
cmake -S . -B build
```

`-S .` 指定源码目录，`-B build` 把生成文件和编译产物放入 `build/`。修改普通 `.cpp/.h` 后直接重新构建即可；修改 `CMakeLists.txt` 后再次运行配置命令。

只构建 Stage 1 测试：

```bash
cmake --build build --target unit_test -j"$(nproc)"
```

产物是 `build/bin/unit_test`。开发 Stage 1 时优先构建该目标，反馈比全量构建更直接。

构建整个项目：

```bash
cmake --build build -j"$(nproc)"
```

主要产物包括 `build/bin/rmdb`、`build/bin/unit_test` 和 `build/bin/test_parser`。warning 值得后续处理，但只有命令返回非零或出现 error 才表示构建失败。

## 3. 运行已有测试

建议进入构建目录运行测试，使测试产生的临时数据库文件留在 `build/`：

```bash
cd build
./bin/unit_test
```

当前 Stage 1 测试组包括 `LRUReplacerTest`、`BufferPoolManagerTest`、`BufferPoolManagerConcurrencyTest`、`StorageTest` 和 `RecordManagerTest`。

常用 GoogleTest 参数：

```bash
# 列出测试
./bin/unit_test --gtest_list_tests

# 运行一个测试组
./bin/unit_test --gtest_filter='BufferPoolManagerTest.*'

# 运行单个测试
./bin/unit_test --gtest_filter='RecordManagerTest.SimpleTest'

# 重复并发测试，遇到失败立即停止
./bin/unit_test --gtest_filter='BufferPoolManagerConcurrencyTest.*' --gtest_repeat=100 --gtest_break_on_failure
```

### CTest 的注意点

当前 CMake 只把 parser 测试注册给 CTest，没有注册 `unit_test`。全量构建后可以执行：

```bash
ctest --test-dir build --output-on-failure
```

但这不能代替 `build/bin/unit_test`；只运行 `ctest` 会漏掉 Stage 1 的存储与记录测试。

## 4. 创建自己的测试

### 4.1 追加到 unit_test.cpp

最省事的方法是在 `src/unit_test.cpp` 中增加 GoogleTest 用例：

```cpp
TEST(BufferPoolManagerExtraTest, NewPageStartsClean) {
    DiskManager disk_manager;
    // 创建并打开测试文件，得到 fd

    BufferPoolManager bpm(1, &disk_manager);
    PageId page_id{fd, INVALID_PAGE_ID};
    Page *page = bpm.new_page(&page_id);

    ASSERT_NE(page, nullptr);
    EXPECT_FALSE(page->is_dirty());

    // 关闭并删除测试文件
}
```

重新构建并定向运行：

```bash
cmake --build build --target unit_test -j"$(nproc)"
./build/bin/unit_test --gtest_filter='BufferPoolManagerExtraTest.*'
```

优先补充以下边界：

- 新页面初始 `pin_count == 1`、`is_dirty == false`。
- `unpin_page(page_id, false)` 不应把页面标脏。
- 多次 fetch 后，需要相同次数的 unpin 才能进入 replacer。
- 所有 frame 都 pinned 时，`fetch_page/new_page` 返回 `nullptr`。
- dirty victim 在 frame 被复用前已经写回磁盘。
- 删除满页中的记录后，该页重新进入空闲页链表。
- 空文件、跨页记录和稀疏 bitmap 的扫描结果正确。

### 4.2 创建独立测试文件

可以创建 `src/my_storage_test.cpp`，并在 `src/CMakeLists.txt` 中增加：

```cmake
add_executable(my_storage_test my_storage_test.cpp)
target_link_libraries(my_storage_test storage lru_replacer record gtest_main)
```

测试文件的最小结构：

```cpp
#include "gtest/gtest.h"
#include "storage/buffer_pool_manager.h"

TEST(MyStorageTest, Example) {
    EXPECT_EQ(1 + 1, 2);
}
```

修改 CMake 后重新配置、构建、运行：

```bash
cmake -S . -B build
cmake --build build --target my_storage_test -j"$(nproc)"
./build/bin/my_storage_test
```

只有确实需要检查私有状态时，才考虑 `#define private public`；一般应优先通过公开接口和最终行为验证。

## 5. 写测试时的习惯

### 每个测试独立管理文件

测试应创建唯一文件，并在结束时关闭、删除。使用 fixture 时把准备和清理放入 `SetUp()`、`TearDown()`，避免某次失败留下的数据影响下一次运行。

### 同时检查三层结果

一个 BufferPool 测试通常应覆盖接口返回值、PageId/pin/dirty 等内存状态，以及 flush 或 eviction 后的磁盘内容。

### 先写最小复现

隐藏测试失败时，先把语义压缩成小序列：

```text
new page → 修改内容 → unpin(false) → 淘汰 → 重新读取
```

它能直接区分“页面初始被错误标脏”和“unpin 的 dirty 传播错误”。最小复现稳定后，再运行完整测试和并发重复测试。

## 6. 常用排查命令

```bash
# GDB 调试单个用例
gdb --args ./build/bin/unit_test --gtest_filter='BufferPoolManagerTest.SampleTest'

# 显示完整编译命令
cmake --build build --target unit_test --verbose

# 检查当前修改
git status --short
git diff --check
git diff
```
