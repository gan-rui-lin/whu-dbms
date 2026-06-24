# WHU DBMS 实验仓库说明

本仓库是在 RMDB 课程框架基础上完成数据库管理系统实验的工作区。真正的数据库项目位于 `rmdb/`，仓库根目录主要用于保存题目文档、阶段记录、参考压缩包和提交包。

## 目录构成

```text
.
├── docs/                 阶段题面、实现指南和问题记录
├── rmdb/                 RMDB 数据库内核项目源码
├── stage_submit.zip      本地生成的提交压缩包
├── .gitignore            忽略本地包、构建产物、运行数据库和日志
└── README.md             当前仓库说明
```

## docs/

`docs/` 保存课程各阶段相关材料：

- `stage0.md` 到 `stage11.md`：各阶段题目要求。
- `stage6_guide.md`、`stage7_guide.md`、`stage8_guide.md`：针对负责阶段整理的实现思路。
- `stage6_record.md`、`stage7_record.md`、`stage8_record.md`：调试记录、平台报错原因和最终解决方案。

其中本仓库重点维护 stage6 到 stage8：

- stage6：唯一索引、索引查询和索引维护。
- stage7：聚合函数 `COUNT`、`SUM`、`MAX`、`MIN`。
- stage8：`ORDER BY` 和 `LIMIT`。

## rmdb/

`rmdb/` 是可独立编译运行的 RMDB 项目根目录，里面的 `README.md` 是原框架说明。

主要内容：

- `CMakeLists.txt`：项目 CMake 入口。
- `src/`：数据库服务端和内核源码。
- `deps/`：第三方依赖，主要是 googletest。
- `rmdb_client/`：交互式客户端。
- `build/`：本地 CMake 构建目录，被 `.gitignore` 忽略。
- `RMDB使用文档.pdf`、`RMDB项目结构.pdf` 等：原框架文档。

`rmdb/src/` 中的重要模块：

- `parser/`：SQL 词法、语法和 AST。
- `analyze/`：语义分析，检查表名、列名、类型和条件。
- `optimizer/`：生成查询计划，包括 scan、join、projection、sort、aggregate、limit 等。
- `execution/`：执行器实现，负责真正产出查询结果或执行 insert/delete/update。
- `system/`：数据库、表和索引元数据管理。
- `record/`：记录文件管理。
- `storage/`：磁盘页和 buffer pool 管理。
- `index/`：B+ 树索引。
- `transaction/`：事务和锁相关框架。
- `recovery/`：日志和恢复相关框架。
- `rmdb.cpp`：服务端入口。
- `unit_test.cpp`：本地单元测试入口。

## 常用命令

在仓库根目录构建 RMDB：

```bash
cmake -S rmdb -B rmdb/build
cmake --build rmdb/build -j2
```

运行部分存储相关单元测试：

```bash
./rmdb/build/bin/unit_test --gtest_filter='StorageTest.*:BufferPoolManagerTest.*:BufferPoolManagerConcurrencyTest.*:RecordManagerTest.SimpleTest'
```

检查提交包结构：

```bash
unzip -l submissions_fixed/stage_submit.zip | head
```
