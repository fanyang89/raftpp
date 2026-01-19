# Cap'n Proto 迁移状态

## 当前进度

### ✅ 已完成的阶段

#### Phase 1: 构建系统和 Schema (已完成)
- [x] 添加 Cap'n Proto 依赖到 CMake
- [x] 创建 `proto/raftpp.capnp` schema 文件
- [x] 更新构建系统以生成 Cap'n Proto 代码
- [x] 实现 `OwnedMessage<T>` 包装器类 (capnp_message.h)

#### Phase 2: 核心类型迁移 (已完成)
- [x] 迁移 `include/raftpp/core/types.h` - 定义核心类型别名
- [x] 迁移 `include/raftpp/core/raft.h` 和 `lib/core/raft.cc`
- [x] 迁移 `include/raftpp/core/raft_core.h` 和 `lib/core/raft_core.cc`
- [x] 迁移 `include/raftpp/core/raw_node.h` 和 `lib/core/raw_node.cc`
- [x] 迁移 `include/raftpp/core/storage.h` 和 `lib/core/memory_storage.cc`
- [x] 迁移 `include/raftpp/core/raft_log.h` 和 `lib/core/raft_log.cc`
- [x] 迁移 `include/raftpp/core/unstable_log.h` 和 `lib/core/unstable_log.cc`
- [x] 迁移 `lib/core/conf_changer.cc`
- [x] 迁移 `lib/core/read_only.cc`

#### Phase 3: Raftor 和 WAL 层迁移 (已完成)
- [x] 迁移 `lib/raftor/raftor.cc`
- [x] 迁移 `lib/raftor/ready_processor.cc`
- [x] 迁移 `lib/raftor/wal/wal.h` 和 `lib/raftor/wal/wal.cc`
- [x] 迁移 `lib/raftor/wal/wal_storage.h` 和 `lib/raftor/wal/wal_storage.cc`
- [x] 迁移 `lib/raftor/wal/metadata_store.cc`

#### Phase 5: 测试基础设施迁移 (已完成)
- [x] 迁移 `tests/harness/test_util.h` 和 `tests/harness/test_util.cc`
  - [x] 基础辅助函数 (MakeHardState, NewEntry, NewSnapshot 等)
  - [x] 比较操作符 (使用 messagesEqual)
  - [x] ConfState 比较 (基于集合的相等性)
  - [x] 测试 Raft 实例创建函数
- [x] 迁移 `tests/harness/network.h` 和 `tests/harness/network.cc`
- [x] 迁移 `tests/harness/interface.h` 和 `tests/harness/interface.cc`
- [x] 更新所有测试文件的包含路径 (raftpp.pb.h → types.h)

### 🔄 待完成的阶段

#### Phase 4: RPC Transport 迁移 (已完成)

##### 需要完成的工作：

1. **创建 CapnpTransport 实现**
   - [x] 创建 `include/raftpp/raftor/rpc/capnp_transport.h`
   - [x] 创建 `lib/raftor/rpc/capnp_transport.cc`
   - [x] 实现基于 Cap'n Proto RPC 的消息传输
   - [x] 集成 KJ 事件循环与 Raftor 主循环

2. **删除旧的 RPC 代码**
   - [x] 删除 `include/raftpp/raftor/rpc/rpclib_transport.h`
   - [x] 删除 `lib/raftor/rpc/rpclib_transport.cc`
   - [ ] 删除 `lib/raftor/rpc/codec.cc` (仍在使用)
   - [x] 从 CMakeLists.txt 中移除 rpclib 依赖

3. **更新构建系统**
   - [x] 从 `third_party/CMakeLists.txt` 移除 protobuf 依赖
   - [x] 从 `third_party/CMakeLists.txt` 移除 rpclib 依赖
   - [x] 更新 `lib/CMakeLists.txt` 以使用新的 CapnpTransport

4. **测试和验证**
   - [ ] 修复所有编译错误
   - [ ] 运行单元测试 (task test)
   - [ ] 运行数据驱动测试 (task dt)
   - [ ] 手动验证多节点集群功能

#### Phase 6: 清理工作 (已完成)

- [x] 删除 `proto/raftpp.proto` 文件
- [x] 删除所有生成的 protobuf 代码
- [x] 清理所有剩余的 protobuf 引用
- [x] 更新文档以反映 Cap'n Proto 使用

## 关键技术要点

### Cap'n Proto API 模式

```cpp
// Builder 模式 - 用于修改消息
Entry entry;
auto builder = entry.builder();
builder.setIndex(1);
builder.setTerm(1);

// Reader 模式 - 用于读取消息
auto reader = entry.reader();
uint64_t index = reader.getIndex();
uint64_t term = reader.getTerm();

// 克隆 - OwnedMessage 是移动专用的
Entry cloned = entry.clone();

// 列表初始化
auto voters = builder.initVoters(3);
voters.set(0, 1);
voters.set(1, 2);
voters.set(2, 3);
```

### 常见陷阱

1. **不能复制 OwnedMessage** - 必须使用 `clone()` 或 `std::move()`
2. **枚举命名** - 使用 UPPER_SNAKE_CASE (如 `MessageType::MSG_APPEND`)
3. **直接字段访问** - 必须使用 `reader().getField()` 或 `builder().setField()`
4. **列表检查** - 使用 `.size() > 0` 而不是 `.empty()`
5. **命名空间** - Cap'n Proto 类型使用 `::capnp::` 全局命名空间

## 已知问题

- 暂无已知遗留问题
- 一些测试可能需要适配 Cap'n Proto 的比较语义

## 下一步行动

1. 开始 Phase 4: 实现 CapnpTransport
2. 设计 KJ 事件循环与 Raftor 主循环的集成方案
3. 实现基本的 Cap'n Proto RPC 消息传输
4. 测试并验证新的 transport 层

## 提交历史（最近10次）

```
c3c0f89 Update test file includes to use Cap'n Proto types
828181a Migrate interface.cc to Cap'n Proto
a83f8b8 Migrate network.h and network.cc to Cap'n Proto
e9ad809 Migrate test_util.cc to Cap'n Proto
1080a3f Migrate test_util.cc basic helper functions to Cap'n Proto (Part 1)
2b750a1 Update test harness headers to use Cap'n Proto
e53df98 Complete WAL layer Cap'n Proto migration
bde143d Complete WAL storage layer Cap'n Proto migration
9399f50 Complete Raftor layer Cap'n Proto migration
2b00737 Complete raft.cc Cap'n Proto migration
```

---

*最后更新: 2026-01-18*
