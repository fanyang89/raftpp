# raftpp 实现计划

## 概述

本文档基于对 `third_party/raft-rs` 的分析，列出了 `raftpp` 项目当前缺失的特性、功能和测试。

## 1. 缺失的特性

### 1.1 配置选项
raft-rs 和 raftpp 都已实现的配置选项：
- ✅ `id` - 节点 ID
- ✅ `election_tick` - 选举超时 tick 数
- ✅ `heartbeat_tick` - 心跳 tick 数
- ✅ `applied` - 最后应用的索引
- ✅ `max_size_per_msg` - 每条消息最大大小
- ✅ `max_inflight_msgs` - 最大 in-flight 消息数
- ✅ `check_quorum` - 检查 quorum 活跃性
- ✅ `pre_vote` - 启用 Pre-Vote 算法
- ✅ `min_election_tick` - 最小选举超时
- ✅ `max_election_tick` - 最大选举超时
- ✅ `read_only_option` - 只读选项 (Safe/LeaseBased)
- ✅ `skip_bcast_commit` - 跳过广播提交
- ✅ `batch_append` - 批量追加
- ✅ `priority` - 选举优先级
- ✅ `max_uncommitted_size` - 最大未提交大小
- ✅ `max_committed_size_per_ready` - 每个 Ready 的最大提交大小
- ✅ `max_apply_unpersisted_log_limit` - 最大应用未持久化日志限制
- ✅ `disable_proposal_forwarding` - 禁用提案转发

**状态**: raftpp 的配置选项与 raft-rs 基本一致 ✅

### 1.2 核心数据结构
- ✅ `Raft` / `RaftCore` - 核心 Raft 实现
- ✅ `RawNode` - 用户接口
- ✅ `Ready` / `LightReady` - 就绪状态
- ✅ `ProgressTracker` - 进度跟踪器
- ✅ `Progress` - 单个节点进度
- ✅ `Inflights` - in-flight 消息窗口
- ✅ `RaftLog` - Raft 日志
- ✅ `Unstable` - 不稳定日志
- ✅ `Storage` - 存储接口
- ✅ `MemStorage` - 内存存储
- ✅ `ConfState` - 配置状态
- ✅ `ConfChangeV2` - 配置变更
- ✅ `MajorityConf` - 多数派配置
- ✅ `JointConf` - 联合配置
- ✅ `ReadOnly` - 只读请求处理
- ✅ `SoftState` - 软状态
- ✅ `HardState` - 硬状态

**状态**: 核心数据结构基本完整 ✅

### 1.3 消息类型
raft-rs 和 raftpp 都支持的消息类型：
- ✅ `MsgHup` - 本地消息，触发选举
- ✅ `MsgBeat` - 本地消息，触发心跳
- ✅ `MsgPropose` - 提案消息
- ✅ `MsgAppend` - 追加条目消息
- ✅ `MsgAppendResponse` - 追加响应
- ✅ `MsgRequestVote` - 请求投票
- ✅ `MsgRequestVoteResponse` - 投票响应
- ✅ `MsgSnapshot` - 快照消息
- ✅ `MsgHeartbeat` - 心跳消息
- ✅ `MsgHeartbeatResponse` - 心跳响应
- ✅ `MsgUnreachable` - 不可达消息
- ✅ `MsgSnapStatus` - 快照状态
- ✅ `MsgCheckQuorum` - 检查 quorum
- ✅ `MsgTransferLeader` - 转移领导权
- ✅ `MsgTimeoutNow` - 立即超时
- ✅ `MsgReadIndex` - 读索引
- ✅ `MsgReadIndexResp` - 读索引响应
- ✅ `MsgRequestPreVote` - 请求预投票
- ✅ `MsgRequestPreVoteResponse` - 预投票响应

**状态**: 消息类型完整 ✅

## 2. 缺失的功能

### 2.1 Raft 核心功能
| 功能 | raft-rs | raftpp | 状态 |
|------|-----------|---------|------|
| 基础选举 | ✅ | ✅ | 已实现 |
| Pre-Vote | ✅ | ✅ | 已实现 |
| Check Quorum | ✅ | ✅ | 已实现 |
| 日志复制 | ✅ | ✅ | 已实现 |
| 提交机制 | ✅ | ✅ | 已实现 |
| 配置变更 (简单) | ✅ | ✅ | 已实现 |
| 配置变更 (联合) | ✅ | ✅ | 已实现 |
| Learner 支持 | ✅ | ✅ | 已实现 |
| 领导权转移 | ✅ | ✅ | 已实现 |
| 快照 | ✅ | ✅ | 已实现 |
| 只读请求 (Safe) | ✅ | ✅ | 已实现 |
| 只读请求 (Lease) | ✅ | ✅ | 已实现 |
| 流控 (inflight) | ✅ | ✅ | 已实现 |
| 批量追加 | ✅ | ✅ | 已实现 |
| 优先级 | ✅ | ✅ | 已实现 |
| 未提交大小限制 | ✅ | ✅ | 已实现 |
| 提交条目分页 | ✅ | ✅ | 已实现 |
| 异步条目获取 | ✅ | ✅ | 已实现 |
| 禁用提案转发 | ✅ | ✅ | 已实现 |
| 资源释放 (free inflight buffers) | ✅ | ✅ | 已实现 |
| 动态调整 max_inflight | ✅ | ✅ | 已实现 (adjust_max_inflight_msgs) |
| Request Snapshot | ✅ | ✅ | 已实现 |

**状态**: 核心功能基本完整 ✅

### 2.2 RawNode 功能
| 功能 | raft-rs | raftpp | 状态 |
|------|-----------|---------|------|
| `new()` | ✅ | ✅ | 已实现 |
| `tick()` | ✅ | ✅ | 已实现 |
| `campaign()` | ✅ | ✅ | 已实现 |
| `propose()` | ✅ | ✅ | 已实现 |
| `propose_conf_change()` | ✅ | ✅ | 已实现 |
| `step()` | ✅ | ✅ | 已实现 |
| `ready()` | ✅ | ✅ | 已实现 |
| `advance()` | ✅ | ✅ | 已实现 |
| `advance_append()` | ✅ | ✅ | 已实现 |
| `advance_append_async()` | ✅ | ✅ | 已实现 |
| `advance_apply()` | ✅ | ✅ | 已实现 |
| `advance_apply_to()` | ✅ | ✅ | 已实现 |
| `on_entries_fetched()` | ✅ | ✅ | 已实现 |
| `on_persist_ready()` | ✅ | ✅ | 已实现 |
| `set_priority()` | ✅ | ✅ | 已实现 |
| `request_snapshot()` | ✅ | ✅ | 已实现 |
| `transfer_leader()` | ✅ | ✅ | 已实现 |
| `read_index()` | ✅ | ✅ | 已实现 |
| `abort_leader_transfer()` | ✅ | ✅ | 已实现 |
| `has_ready()` | ✅ | ✅ | 已实现 |
| `commit_ready()` | ✅ | ✅ | 已实现 |
| `set_max_committed_size_per_ready()` | ✅ | ✅ | 已实现 |
| `status()` | ✅ | ✅ | 已实现 (GetStatus) |
| `report_unreachable()` | ✅ | ✅ | 已实现 |
| `report_snapshot()` | ✅ | ✅ | 已实现 |
| `ping()` | ✅ | ✅ | 已实现 |

**状态**: RawNode 功能完整 ✅

### 2.3 ProgressTracker 功能
| 功能 | raft-rs | raftpp | 状态 |
|------|-----------|---------|------|
| `ApplyConf()` | ✅ | ✅ | 已实现 |
| `ResetVotes()` | ✅ | ✅ | 已实现 |
| `MaxCommittedIndex()` | ✅ | ✅ | 已实现 |
| `RecordVote()` | ✅ | ✅ | 已实现 |
| `HasQuorum()` | ✅ | ✅ | 已实现 |
| `QuorumRecentlyActive()` | ✅ | ✅ | 已实现 |
| `GetVoteResult()` | ✅ | ✅ | 已实现 |
| `CountVotes()` | ✅ | ✅ | 已实现 |
| `SetProgress()` | ✅ | ✅ | 已实现 (ApplyConf) |
| `GetProgress()` | ✅ | ✅ | 已实现 |

**状态**: ProgressTracker 功能完整 ✅

### 2.4 Progress 功能
| 功能 | raft-rs | raftpp | 状态 |
|------|-----------|---------|------|
| `Reset()` | ✅ | ✅ | 已实现 |
| `BecomeProbe()` | ✅ | ✅ | 已实现 |
| `BecomeReplicate()` | ✅ | ✅ | 已实现 |
| `BecomeSnapshot()` | ✅ | ✅ | 已实现 |
| `IsPaused()` | ✅ | ✅ | 已实现 |
| `Resume()` | ✅ | ✅ | 已实现 |
| `Pause()` | ✅ | ✅ | 已实现 |
| `MaybeUpdate()` | ✅ | ✅ | 已实现 |
| `MaybeDecTo()` | ✅ | ✅ | 已实现 |
| `SnapshotFailure()` | ✅ | ✅ | 已实现 |
| `OptimisticUpdate()` | ✅ | ✅ | 已实现 |
| `UpdateState()` | ✅ | ✅ | 已实现 |
| `UpdateCommitted()` | ✅ | ✅ | 已实现 |
| `IsSnapshotCaughtUp()` | ✅ | ✅ | 已实现 |

**状态**: Progress 功能完整 ✅

### 2.5 RaftLog 功能
| 功能 | raft-rs | raftpp | 状态 |
|------|-----------|---------|------|
| `Term()` | ✅ | ✅ | 已实现 |
| `FirstIndex()` | ✅ | ✅ | 已实现 |
| `LastIndex()` | ✅ | ✅ | 已实现 |
| `LastTerm()` | ✅ | ✅ | 已实现 |
| `Append()` | ✅ | ✅ | 已实现 |
| `MaybeAppend()` | ✅ | ✅ | 已实现 |
| `MaybeCommit()` | ✅ | ✅ | 已实现 |
| `CommitTo()` | ✅ | ✅ | 已实现 |
| `AppliedTo()` | ✅ | ✅ | 已实现 |
| `StableEntries()` | ✅ | ✅ | 已实现 |
| `StableSnapshot()` | ✅ | ✅ | 已实现 |
| `GetEntries()` | ✅ | ✅ | 已实现 |
| `AllEntries()` | ✅ | ✅ | 已实现 |
| `NextEntries()` | ✅ | ✅ | 已实现 |
| `NextEntriesSince()` | ✅ | ✅ | 已实现 |
| `HasNextEntries()` | ✅ | ✅ | 已实现 |
| `HasNextEntriesSince()` | ✅ | ✅ | 已实现 |
| `FindConflict()` | ✅ | ✅ | 已实现 |
| `FindConflictByTerm()` | ✅ | ✅ | 已实现 |
| `MatchTerm()` | ✅ | ✅ | 已实现 |
| `IsUpToDate()` | ✅ | ✅ | 已实现 |
| `Restore()` | ✅ | ✅ | 已实现 |
| `GetSnapshot()` | ✅ | ✅ | 已实现 |
| `MaybePersist()` | ✅ | ✅ | 已实现 |
| `MaybePersistSnapshot()` | ✅ | ✅ | 已实现 |
| `unstable()` | ✅ | ✅ | 已实现 |
| `applied()` | ✅ | ✅ | 已实现 |
| `committed()` | ✅ | ✅ | 已实现 |
| `persisted()` | ✅ | ✅ | 已实现 |

**状态**: RaftLog 功能完整 ✅

## 3. 缺失的测试

### 3.1 raft_test.cc (test_raft.rs 对应)
raft-rs 的 test_raft.rs 有约 5000+ 行测试，包含大量测试场景。

**raftpp 已实现的测试** (raft_test.cc):
- ✅ `progress committed index` - 进度提交索引测试
- ✅ `leader election` - 领导选举测试 (部分)

**raftpp 缺失的测试** (来自 test_raft.rs):

#### 进度跟踪测试
- ❌ `test_progress_leader()` - 测试 leader 状态下的进度跟踪
- ❌ `test_progress_resume_by_heartbeat_resp()` - 测试心跳响应恢复进度
- ❌ `test_progress_paused()` - 测试暂停状态

#### 选举测试
- ❌ `test_leader_cycle()` - 测试 leader 循环
- ❌ `test_leader_election_overwrite_newer_logs()` - 测试选举覆盖新日志
- ❌ `test_vote_from_any_state()` - 测试从任何状态投票
- ❌ `test_prevote_from_any_state()` - 测试从任何状态预投票

#### 日志复制测试
- ❌ `test_log_replication()` - 完整的日志复制测试
- ❌ `test_single_node_commit()` - 单节点提交测试
- ❌ `test_cannot_commit_without_new_term_entry()` - 无新 term 条目无法提交
- ❌ `test_commit_without_new_term_entry()` - 无新 term 条目提交
- ❌ `test_dueling_candidates()` - 决斗候选者测试
- ❌ `test_dueling_pre_candidates()` - 决斗预候选者测试
- ❌ `test_candidate_concede()` - 候选者让步测试
- ❌ `test_single_node_candidate()` - 单节点候选者测试
- ❌ `test_single_node_pre_candidate()` - 单节点预候选者测试

#### 状态转换测试
- ❌ `test_old_messages()` - 旧消息测试
- ❌ `test_proposal()` - 提案测试
- ❌ `test_commit()` - 提交测试
- ❌ `test_pass_election_timeout()` - 通过选举超时测试
- ❌ `test_handle_msg_append()` - 处理追加消息测试
- ❌ `test_handle_heartbeat()` - 处理心跳测试
- ❌ `test_handle_heartbeat_resp()` - 处理心跳响应测试
- ❌ `test_recv_msg_request_vote()` - 接收投票请求测试
- ❌ `test_recv_msg_request_vote_for_type()` - 接收特定类型投票请求测试
- ❌ `test_state_transition()` - 状态转换测试

#### Leader Stepdown 测试
- ❌ `test_all_server_stepdown()` - 所有服务器 stepdown 测试
- ❌ `test_candidate_reset_term_msg_heartbeat()` - 候选者重置 term 心跳消息
- ❌ `test_candidate_reset_term_msg_append()` - 候选者重置 term 追加消息
- ❌ `test_candidate_reset_term()` - 候选者重置 term
- ❌ `test_leader_stepdown_when_quorum_active()` - quorum 活跃时 leader stepdown
- ❌ `test_leader_stepdown_when_quorum_lost()` - quorum 丢失时 leader stepdown
- ❌ `test_leader_superseding_with_check_quorum()` - check quorum 时 leader 超越
- ❌ `test_leader_election_with_check_quorum()` - check quorum 时 leader 选举
- ❌ `test_free_stuck_candidate_with_check_quorum()` - check quorum 时释放卡住的候选者
- ❌ `test_non_promotable_voter_with_check_quorum()` - check quorum 时不可提升投票者
- ❌ `test_disruptive_follower()` - 干扰性 follower 测试
- ❌ `test_disruptive_follower_pre_vote()` - pre-vote 干扰性 follower 测试

#### 只读选项测试
- ❌ `test_read_only_option_safe()` - 安全只读选项测试
- ❌ `test_read_only_with_learner()` - learner 只读测试
- ❌ `test_read_only_option_lease()` - lease 只读选项测试
- ❌ `test_read_only_option_lease_without_check_quorum()` - 无 check quorum 的 lease 只读
- ❌ `test_read_only_for_new_leader()` - 新 leader 只读测试
- ❌ `test_advance_commit_index_by_read_index_response()` - 读索引响应推进提交索引

#### Leader 追加响应测试
- ❌ `test_leader_append_response()` - leader 追加响应测试

#### 广播测试
- ❌ `test_bcast_beat()` - 广播心跳测试
- ❌ `test_recv_msg_beat()` - 接收心跳测试

#### Leader 增加 next 测试
- ❌ `test_leader_increase_next()` - leader 增加 next 测试

#### 发送追加测试
- ❌ `test_send_append_for_progress_probe()` - probe 状态发送追加
- ❌ `test_send_append_for_progress_replicate()` - replicate 状态发送追加
- ❌ `test_send_append_for_progress_snapshot()` - snapshot 状态发送追加

#### 不可达消息测试
- ❌ `test_recv_msg_unreachable()` - 接收不可达消息测试

#### 恢复测试
- ❌ `test_restore()` - 恢复测试
- ❌ `test_restore_ignore_snapshot()` - 忽略快照恢复
- ❌ `test_provide_snap()` - 提供快照测试
- ❌ `test_ignore_providing_snapshot()` - 忽略提供快照
- ❌ `test_restore_from_snap_msg()` - 从快照消息恢复
- ❌ `test_slow_node_restore()` - 慢节点恢复

#### 配置变更测试
- ❌ `test_step_config()` - 配置 step 测试
- ❌ `test_step_ignore_config()` - 忽略配置 step 测试
- ❌ `test_new_leader_pending_config()` - 新 leader 待处理配置
- ❌ `test_add_node()` - 添加节点测试
- ❌ `test_add_node_check_quorum()` - check quorum 添加节点
- ❌ `test_remove_node()` - 移除节点测试
- ❌ `test_remove_node_itself()` - 移除自身节点测试
- ❌ `test_promotable()` - 可提升测试
- ❌ `test_raft_nodes()` - raft 节点测试
- ❌ `test_campaign_while_leader()` - leader 时竞选测试
- ❌ `test_pre_campaign_while_leader()` - leader 时预竞选测试
- ❌ `test_campaign_while_leader_with_pre_vote()` - pre-vote 时 leader 竞选
- ❌ `test_commit_after_remove_node()` - 移除节点后提交
- ❌ `test_conf_change_check_before_campaign()` - 竞选前配置变更检查

#### Leader Transfer 测试
- ❌ `test_leader_transfer_to_uptodate_node()` - 转移到最新节点
- ❌ `test_leader_transfer_to_uptodate_node_from_follower()` - follower 转移到最新节点
- ❌ `test_leader_transfer_with_check_quorum()` - check quorum 转移
- ❌ `test_leader_transfer_to_slow_follower()` - 转移到慢 follower
- ❌ `test_leader_transfer_after_snapshot()` - 快照后转移
- ❌ `test_leader_transfer_to_self()` - 转移到自身
- ❌ `test_leader_transfer_to_non_existing_node()` - 转移到不存在的节点
- ❌ `test_leader_transfer_to_learner()` - 转移到 learner
- ❌ `test_leader_transfer_timeout()` - 转移超时
- ❌ `test_leader_transfer_ignore_proposal()` - 转移忽略提案
- ❌ `test_leader_transfer_receive_higher_term_vote()` - 转移接收更高 term 投票
- ❌ `test_leader_transfer_remove_node()` - 转移移除节点
- ❌ `test_leader_transfer_back()` - 转移回来
- ❌ `test_leader_transfer_second_transfer_to_another_node()` - 第二次转移到另一个节点
- ❌ `test_leader_transfer_second_transfer_to_same_node()` - 第二次转移到同一节点
- ❌ `check_leader_transfer_state()` - 检查转移状态
- ❌ `test_transfer_non_member()` - 转移非成员

#### Learner 测试
- ❌ `test_node_with_smaller_term_can_complete_election()` - 更小 term 节点完成选举
- ❌ `test_learner_election_timeout()` - learner 选举超时
- ❌ `test_learner_promotion()` - learner 提升
- ❌ `test_learner_log_replication()` - learner 日志复制
- ❌ `test_restore_with_learner()` - 带 learner 恢复
- ❌ `test_restore_with_voters_outgoing()` - 带 outgoing voters 恢复
- ❌ `test_restore_depromote_voter()` - 降级 voter 恢复
- ❌ `test_restore_learner()` - 恢复 learner
- ❌ `test_restore_learner_promotion()` - 恢复 learner 提升
- ❌ `test_learner_receive_snapshot()` - learner 接收快照
- ❌ `test_add_learner()` - 添加 learner
- ❌ `test_remove_learner()` - 移除 learner

#### Pre-Vote 迁移测试
- ❌ `test_prevote_migration_can_complete_election()` - pre-vote 迁移完成选举
- ❌ `test_prevote_migration_with_free_stuck_pre_candidate()` - pre-vote 迁移释放卡住的预候选者
- ❌ `test_learner_respond_vote()` - learner 响应投票

#### 其他测试
- ❌ `test_election_tick_range()` - 选举 tick 范围测试
- ❌ `test_prevote_with_split_vote()` - pre-vote 分裂投票
- ❌ `test_prevote_with_check_quorum()` - pre-vote check quorum
- ❌ `test_new_raft_with_bad_config_errors()` - 错误配置创建 raft
- ❌ `test_batch_msg_append()` - 批量消息追加
- ❌ `test_advance_commit_index_by_vote_request()` - 投票请求推进提交索引
- ❌ `test_advance_commit_index_by_direct_vote_request()` - 直接投票请求推进提交索引
- ❌ `test_advance_commit_index_by_prevote_request()` - 预投票请求推进提交索引
- ❌ `test_advance_commit_index_by_vote_response()` - 投票响应推进提交索引
- ❌ `test_advance_commit_index_by_direct_vote_response()` - 直接投票响应推进提交索引
- ❌ `test_advance_commit_index_by_prevote_response()` - 预投票响应推进提交索引
- ❌ `prepare_request_snapshot()` - 准备请求快照
- ❌ `test_follower_request_snapshot()` - follower 请求快照
- ❌ `test_request_snapshot_unavailable()` - 请求不可用快照
- ❌ `test_request_snapshot_matched_change()` - 请求快照匹配变更
- ❌ `test_request_snapshot_none_replicate()` - 请求快照无复制

### 3.2 raw_node_test.cc (test_raw_node.rs 对应)
raft-rs 的 test_raw_node.rs 有约 1985 行测试。

**raftpp 已实现的测试** (raw_node_test.cc):
- ✅ `is local message` - 本地消息测试
- ✅ `step local message ignored` - 忽略本地消息测试
- ✅ `propose data` - 提案数据测试 (部分)

**raftpp 缺失的测试** (来自 test_raw_node.rs):

#### RawNode 基础测试
- ❌ `test_raw_node_step()` - RawNode step 测试
- ❌ `test_raw_node_read_index_to_old_leader()` - 旧 leader 读索引测试
- ❌ `test_raw_node_propose_and_conf_change()` - 提案和配置变更测试
- ❌ `test_raw_node_joint_auto_leave()` - 联合自动离开测试
- ❌ `test_raw_node_propose_add_duplicate_node()` - 提案添加重复节点
- ❌ `test_raw_node_propose_add_learner_node()` - 提案添加 learner 节点
- ❌ `test_raw_node_read_index()` - 读索引测试
- ❌ `test_raw_node_start()` - RawNode 启动测试
- ❌ `test_raw_node_restart()` - RawNode 重启测试
- ❌ `test_raw_node_restart_from_snapshot()` - 从快照重启测试

#### Skip Bcast Commit 测试
- ❌ `test_skip_bcast_commit()` - 跳过广播提交测试

#### Priority 测试
- ❌ `test_set_priority()` - 设置优先级测试

#### Bounded Uncommitted Entries 测试
- ❌ `test_bounded_uncommitted_entries_growth_with_partition()` - 分区时未提交条目增长限制测试

#### Async Entries 测试
- ❌ `test_raw_node_with_async_entries()` - 异步条目测试
- ❌ `test_raw_node_with_async_entries_to_removed_node()` - 异步条目到已移除节点
- ❌ `test_raw_node_with_async_entries_on_follower()` - follower 异步条目
- ❌ `test_raw_node_async_entries_with_leader_change()` - leader 变更时异步条目

#### Async Ready 测试
- ❌ `test_raw_node_with_async_apply()` - 异步应用测试
- ❌ `test_raw_node_entries_after_snapshot()` - 快照后条目测试
- ❌ `test_raw_node_overwrite_entries()` - 覆盖条目测试
- ❌ `test_async_ready_leader()` - leader 异步就绪
- ❌ `test_async_ready_follower()` - follower 异步就绪
- ❌ `test_async_ready_multiple_snapshot()` - 多快照异步就绪

#### Committed Entries Pagination 测试
- ❌ `test_committed_entries_pagination()` - 提交条目分页测试
- ❌ `test_committed_entries_pagination_after_restart()` - 重启后提交条目分页

#### Disable Proposal Forwarding 测试
- ❌ `test_disable_proposal_forwarding()` - 禁用提案转发测试

### 3.3 raft_flow_control_test.cc (test_raft_flow_control.rs 对应)
raft-rs 的 test_raft_flow_control.rs 有约 292 行测试。

**raftpp 已实现的测试**:
- ✅ `msg app flow control full` - 流控满测试
- ✅ `msg app flow control move forward` - 流控前进测试
- ✅ `msg app flow control recv heartbeat` - 流控接收心跳测试
- ✅ `msg app flow control with freeing resources` - 流控释放资源测试
- ✅ `disable progress` - 禁用进度测试

**状态**: 流控测试完整 ✅

### 3.4 raft_paper_test.cc (test_raft_paper.rs 对应)
raft-rs 的 test_raft_paper.rs 有约 1052 行测试。

**raftpp 已实现的测试**:
- ✅ `follower update term from message` - follower 更新 term
- ✅ `candidate update term from message` - candidate 更新 term
- ✅ `leader update term from message` - leader 更新 term
- ✅ `start as follower` - 作为 follower 启动
- ✅ `leader bcast beat` - leader 广播心跳
- ✅ `follower start election` - follower 开始选举
- ✅ `candidate start new election` - candidate 开始新选举
- ✅ `leader election in one round rpc` - 一轮 RPC 选举
- ✅ `follower vote` - follower 投票
- ✅ `candidate fallback` - candidate 让步
- ✅ `follower election timeout randomized` - follower 选举超时随机化
- ✅ `candidate election timeout randomized` - candidate 选举超时随机化
- ✅ `follower election timeout nonconflict` - follower 选举超时无冲突
- ✅ `candidates election timeout nonconflict` - candidate 选举超时无冲突
- ✅ `leader start replication` - leader 开始复制
- ✅ `leader commit entry` - leader 提交条目
- ✅ `leader acknowledge commit` - leader 确认提交
- ✅ `leader commit preceding entries` - leader 提交前置条目
- ✅ `follower commit entry` - follower 提交条目
- ✅ `follower check msg append` - follower 检查追加消息
- ✅ `follower append entries` - follower 追加条目
- ✅ `leader sync follower log` - leader 同步 follower 日志
- ✅ `vote request` - 投票请求
- ✅ `voter` - 投票者
- ✅ `leader only commits log from current term` - leader 只提交当前 term 日志

**raftpp 缺失的测试** (来自 test_raft_paper.rs):

#### 日志复制测试
- ❌ `test_follower_commit_entry` - follower 提交条目测试

**状态**: Raft 论文测试基本完整 ✅

### 3.5 raft_snap_test.cc (test_raft_snap.rs 对应)
raft-rs 的 test_raft_snap.rs 有约 234 行测试。

**raftpp 已实现的测试**:
- ✅ `sending snapshot set pending snapshot` - 发送快照设置待处理快照
- ✅ `pending snapshot pause replication` - 待处理快照暂停复制
- ✅ `snapshot failure` - 快照失败
- ✅ `snapshot succeed` - 快照成功
- ✅ `snapshot abort` - 快照中止
- ✅ `snapshot with min term` - 最小 term 快照
- ✅ `request snapshot` - 请求快照

**状态**: 快照测试完整 ✅

### 3.6 其他测试文件

#### inflights_test.cc
- ✅ 已实现，对应 raft-rs 的 inflights 测试

#### log_test.cc
- ✅ 已实现，对应 raft-rs 的 log 测试

#### log_unstable_test.cc
- ✅ 已实现，对应 raft-rs 的 unstable log 测试

#### progress_test.cc
- ✅ 已实现，对应 raft-rs 的 progress 测试

#### storage_test.cc
- ✅ 已实现，对应 raft-rs 的 storage 测试

#### json_test.cc
- ✅ 已实现，JSON 序列化测试

#### datadriven 测试
- ✅ `confchange_test.cc` - 配置变更数据驱动测试
- ✅ `quorum_test.cc` - quorum 数据驱动测试

**状态**: 其他测试基本完整 ✅

## 4. 总结

### 4.1 已实现情况
raftpp 项目已经实现了 raft-rs 的大部分核心功能：
- ✅ 所有核心数据结构
- ✅ 所有消息类型
- ✅ 所有配置选项
- ✅ 基础选举和 Pre-Vote
- ✅ 日志复制和提交
- ✅ 配置变更（简单和联合）
- ✅ Learner 支持
- ✅ 领导权转移
- ✅ 快照机制
- ✅ 只读请求（Safe 和 Lease）
- ✅ 流控机制
- ✅ 批量追加
- ✅ 优先级机制
- ✅ 未提交大小限制
- ✅ 提交条目分页
- ✅ 异步条目获取
- ✅ 资源释放
- ✅ 动态调整 max_inflight
- ✅ Request Snapshot
- ✅ 禁用提案转发

### 4.2 主要缺失内容

#### 4.2.1 测试覆盖率不足
raftpp 的测试覆盖率远低于 raft-rs，特别是：

1. **raft_test.cc** (test_raft.rs)
   - 缺失约 60+ 个测试场景
   - 主要缺失：进度跟踪、选举、状态转换、leader stepdown、只读选项、配置变更、leader transfer、learner、pre-vote 等详细测试

2. **raw_node_test.cc** (test_raw_node.rs)
   - 缺失约 20+ 个测试场景
   - 主要缺失：RawNode 基础操作、异步条目、异步就绪、提交分页、禁用提案转发等测试

#### 4.2.2 具体缺失测试列表（优先级排序）

**高优先级**（核心功能测试）:
1. 选举相关测试
   - `test_leader_cycle()`
   - `test_leader_election_overwrite_newer_logs()`
   - `test_vote_from_any_state()`
   - `test_prevote_from_any_state()`

2. 日志复制测试
   - `test_log_replication()`
   - `test_single_node_commit()`
   - `test_cannot_commit_without_new_term_entry()`
   - `test_commit_without_new_term_entry()`

3. 状态转换测试
   - `test_state_transition()`
   - `test_all_server_stepdown()`
   - `test_leader_stepdown_when_quorum_active()`
   - `test_leader_stepdown_when_quorum_lost()`

4. 配置变更测试
   - `test_add_node()`
   - `test_remove_node()`
   - `test_remove_node_itself()`
   - `test_promotable()`

**中优先级**（重要功能测试）:
1. Leader Transfer 测试
   - `test_leader_transfer_to_uptodate_node()`
   - `test_leader_transfer_with_check_quorum()`
   - `test_leader_transfer_to_slow_follower()`
   - `test_leader_transfer_timeout()`
   - `test_transfer_non_member()`

2. Learner 测试
   - `test_learner_promotion()`
   - `test_learner_log_replication()`
   - `test_restore_with_learner()`
   - `test_add_learner()`
   - `test_remove_learner()`

3. 只读选项测试
   - `test_read_only_option_safe()`
   - `test_read_only_option_lease()`
   - `test_read_only_for_new_leader()`

**低优先级**（边缘情况测试）:
1. Pre-Vote 迁移测试
   - `test_prevote_migration_can_complete_election()`
   - `test_prevote_migration_with_free_stuck_pre_candidate()`

2. 异步操作测试
   - `test_raw_node_with_async_entries()`
   - `test_async_ready_leader()`
   - `test_async_ready_follower()`

3. 提交分页测试
   - `test_committed_entries_pagination()`
   - `test_committed_entries_pagination_after_restart()`

## 5. 实施建议

### 5.1 测试实施顺序
建议按以下顺序实施缺失的测试：

1. **第一阶段：核心功能测试**（高优先级）
   - 补充 raft_test.cc 中的核心测试
   - 确保选举、日志复制、状态转换等核心功能正确

2. **第二阶段：重要功能测试**（中优先级）
   - 补充 leader transfer、learner、只读选项等测试
   - 确保这些功能在各种场景下正确工作

3. **第三阶段：边缘情况测试**（低优先级）
   - 补充异步操作、提交分页等测试
   - 确保边缘情况处理正确

### 5.2 测试文件组织
建议保持当前测试文件结构：
- `tests/raft_test.cc` - 核心 Raft 测试
- `tests/raw_node_test.cc` - RawNode 测试
- `tests/raft_flow_control_test.cc` - 流控测试
- `tests/raft_paper_test.cc` - Raft 论文测试
- `tests/raft_snap_test.cc` - 快照测试

### 5.3 代码质量保证
在添加新测试时，建议：
1. 使用 doctest 框架（与现有测试一致）
2. 使用 `harness/network.h` 和 `harness/test_util.h` 辅助函数
3. 保持测试命名与 raft-rs 一致
4. 添加适当的注释说明测试目的

## 6. 结论

raftpp 项目在功能实现方面已经非常接近 raft-rs，核心功能基本完整。主要的差距在于测试覆盖率，特别是 `raft_test.cc` 和 `raw_node_test.cc` 中缺失大量测试场景。

建议优先补充核心功能测试，确保 Raft 算法的正确性，然后逐步补充重要功能和边缘情况的测试，最终达到与 raft-rs 相当的测试覆盖率。
