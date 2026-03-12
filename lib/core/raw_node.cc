#include "raftpp/core/raw_node.h"

#include "raftpp/core/util.h"
#include "raftpp/logging.h"

namespace raftpp {

bool IsLocalMessage(const MessageType t) {
    switch (t) {
        case MessageType::MSG_HUP:
        case MessageType::MSG_BEAT:
        case MessageType::MSG_UNREACHABLE:
        case MessageType::MSG_SNAP_STATUS:
        case MessageType::MSG_CHECK_QUORUM:
            return true;
        default:
            return false;
    }
}

bool IsResponseMessage(const MessageType t) {
    switch (t) {
        case MessageType::MSG_APPEND_RESPONSE:
        case MessageType::MSG_REQUEST_VOTE_RESPONSE:
        case MessageType::MSG_HEARTBEAT_RESPONSE:
        case MessageType::MSG_UNREACHABLE:
        case MessageType::MSG_REQUEST_PRE_VOTE_RESPONSE:
            return true;
        default:
            return false;
    }
}

const std::vector<Message>& Ready::Messages() const {
    if (!is_persisted_msg) {
        return light.messages;
    }
    static std::vector<Message> empty;
    return empty;
}

RawNode::RawNode(const Config& config, const std::shared_ptr<Storage>& store)
    : raft_(config, std::move(store)), max_number_(0), commit_since_index_(config.applied) {
    ASSERT(config.id, "config.id must not be zero");
    prev_hs_ = raft_.hard_state();
    prev_ss_ = raft_.soft_state();
    RAFTPP_LOG_INFO("RawNode created with id {}", raft_.id());
}

void RawNode::SetPriority(uint64_t priority) {
    raft_.SetPriority(priority);
}

Result<void> RawNode::RequestSnapshot() {
    return raft_.RequestSnapshot();
}

void RawNode::TransferLeader(const uint64_t transferee) {
    Message m = capnp_util::make<msg::Message>();
    auto builder = capnp_util::builder<msg::Message>(m);
    builder.setMsgType(static_cast<MessageType>(static_cast<int>(MessageType::MSG_TRANSFER_LEADER))
    );
    builder.setFrom(transferee);
    std::ignore = raft_.Step(m);
}

void RawNode::ReadIndex(const std::string& ctx) {
    Message m = capnp_util::make<msg::Message>();
    auto builder = capnp_util::builder<msg::Message>(m);
    builder.setMsgType(static_cast<MessageType>(static_cast<int>(MessageType::MSG_READ_INDEX)));

    auto entries = builder.initEntries(1);
    entries[0].setData(kj::arrayPtr(reinterpret_cast<const kj::byte*>(ctx.data()), ctx.size()));

    std::ignore = raft_.Step(m);
}

Status RawNode::GetStatus() {
    Status s;
    s.id = raft_.id();
    s.hs = raft_.hard_state();
    s.ss = raft_.soft_state();
    s.applied = raft_.raft_log().applied();
    if (s.ss.raft_state == StateRole::Leader) {
        s.progress = raft_.progress_tracker();
    }
    return s;
}

void RawNode::ReportUnreachable(uint64_t id) {
    Message m = capnp_util::make<msg::Message>();
    auto builder = capnp_util::builder<msg::Message>(m);
    builder.setMsgType(static_cast<MessageType>(static_cast<int>(MessageType::MSG_UNREACHABLE)));
    builder.setFrom(id);
    std::ignore = raft_.Step(m);
}

void RawNode::ReportSnapshot(const uint64_t id, const SnapshotStatus status) {
    const auto reject = status == SnapshotStatus::Failure;
    Message m = capnp_util::make<msg::Message>();
    auto builder = capnp_util::builder<msg::Message>(m);
    builder.setMsgType(static_cast<MessageType>(static_cast<int>(MessageType::MSG_SNAP_STATUS)));
    builder.setFrom(id);
    builder.setReject(reject);
    std::ignore = raft_.Step(m);
}

Ready RawNode::GetReady() {
    ++max_number_;

    Ready rd;
    rd.number = max_number_;

    ReadyRecord rd_record;
    rd_record.number = max_number_;

    if (prev_ss_.raft_state != StateRole::Leader && raft_.state() == StateRole::Leader) {
        const auto records = records_;
        records_.clear();
        for (auto& r : records) {
            ASSERT(r.number, std::nullopt);
            ASSERT(r.snapshot, std::nullopt);
        }
    }

    const auto ss = raft_.soft_state();
    if (ss != prev_ss_) {
        rd.ss = ss;
    }

    const auto& hs = raft_.hard_state();
    if (!capnp_util::equal<msg::HardState>(
            capnp_util::reader<msg::HardState>(hs), capnp_util::reader<msg::HardState>(prev_hs_)
        )) {
        auto hs_reader = capnp_util::reader<msg::HardState>(hs);
        auto prev_reader = capnp_util::reader<msg::HardState>(prev_hs_);
        if (hs_reader.getVote() != prev_reader.getVote() ||
            hs_reader.getTerm() != prev_reader.getTerm()) {
            rd.must_sync = true;
        }
        rd.hs = CloneHardState(hs);
    }

    if (!raft_.read_states().empty()) {
        rd.read_states = raft_.read_states();
        raft_.read_states().clear();
    }

    if (const auto snapshot = raft_.raft_log().unstable().snapshot()) {
        rd.snapshot = CloneSnapshot(snapshot->get());
        auto snap_meta = capnp_util::reader<msg::Snapshot>(rd.snapshot).getMetadata();
        ASSERT(commit_since_index_ <= snap_meta.getIndex());
        commit_since_index_ = snap_meta.getIndex();
        ASSERT(
            !raft_.raft_log().HasNextEntriesSince(commit_since_index_),
            "has snapshot but also has committed entries since {}", commit_since_index_
        );
        rd_record.snapshot = {snap_meta.getIndex(), snap_meta.getTerm()};
        rd.must_sync = true;
    }

    const auto& unstable_entries = raft_.raft_log().unstable().entries();
    rd.entries.reserve(unstable_entries.size());
    for (const auto& entry : unstable_entries) {
        rd.entries.push_back(CloneEntry(entry));
    }
    if (!rd.entries.empty()) {
        rd.must_sync = true;
        const auto& last = rd.entries.back();
        auto last_reader = capnp_util::reader<msg::Entry>(last);
        rd_record.last_entry = {last_reader.getIndex(), last_reader.getTerm()};
    }

    rd.is_persisted_msg = raft_.state() != StateRole::Leader;
    rd.light = GetLightReady();
    records_.emplace_back(rd_record);
    return rd;
}

bool RawNode::HasReady() const {
    if (!raft_.messages().empty()) {
        return true;
    }

    if (raft_.soft_state() != prev_ss_) {
        return true;
    }

    if (!capnp_util::equal<msg::HardState>(
            capnp_util::reader<msg::HardState>(raft_.hard_state()),
            capnp_util::reader<msg::HardState>(prev_hs_)
        )) {
        return true;
    }

    if (!raft_.read_states().empty()) {
        return true;
    }

    if (!raft_.raft_log().unstable().entries().empty()) {
        return true;
    }

    if (const auto& snapshot = raft_.snapshot()) {
        auto meta = capnp_util::reader<msg::Snapshot>(*snapshot).getMetadata();
        if (meta.getIndex() > 0) {
            return true;
        }
    }

    if (raft_.raft_log().HasNextEntriesSince(commit_since_index_)) {
        return true;
    }

    return false;
}

void RawNode::OnPersistReady(uint64_t number) {
    uint64_t index = 0;
    uint64_t term = 0;
    uint64_t snap_index = 0;

    while (!records_.empty()) {
        const auto record = records_.front();
        records_.pop_front();

        if (record.number > number) {
            break;
        }

        if (const auto snapshot = record.snapshot) {
            snap_index = snapshot->first;
            index = 0;
            term = 0;
        }

        if (const auto last_entry = record.last_entry) {
            index = last_entry->first;
            term = last_entry->second;
        }
    }

    if (snap_index != 0) {
        raft_.OnPersistSnapshot(snap_index);
    }

    if (index != 0) {
        raft_.OnPersistEntries(index, term);
    }
}

LightReady RawNode::AdvanceAppend(const Ready& rd) {
    CommitReady(rd);
    OnPersistReady(max_number_);
    LightReady light_rd = GetLightReady();

    if (raft_.state() != StateRole::Leader && !light_rd.messages.empty()) {
        PANIC("not leader but has new msg after advance");
    }

    const auto& hard_state = raft_.hard_state();
    auto hs_reader = capnp_util::reader<msg::HardState>(hard_state);
    auto prev_reader = capnp_util::reader<msg::HardState>(prev_hs_);

    if (hs_reader.getCommit() > prev_reader.getCommit()) {
        light_rd.commit_index = hs_reader.getCommit();
        capnp_util::builder<msg::HardState>(prev_hs_).setCommit(hs_reader.getCommit());
    } else {
        ASSERT(hs_reader.getCommit() == prev_reader.getCommit());
        light_rd.commit_index = {};
    }

    ASSERT(
        capnp_util::equal<msg::HardState>(
            capnp_util::reader<msg::HardState>(hard_state),
            capnp_util::reader<msg::HardState>(prev_hs_)
        ),
        "hard state != prev_hs"
    );
    return light_rd;
}

void RawNode::AdvanceApplyTo(const uint64_t applied) {
    raft_.CommitApply(applied);
}

void RawNode::AdvanceAppendAsync(const Ready& rd) {
    CommitReady(rd);
}

void RawNode::CommitReady(const Ready& rd) {
    if (const auto& ss = rd.ss) {
        prev_ss_ = *ss;
    }

    if (const auto& hs = rd.hs) {
        prev_hs_ = CloneHardState(*hs);
    }

    const auto rd_record = records_.back();
    ASSERT(rd_record.number == rd.number);

    if (const auto snapshot = rd_record.snapshot) {
        const auto index = snapshot->first;
        raft_.raft_log().StableSnapshot(index);
    }

    if (const auto last_entry = rd_record.last_entry) {
        const auto index = last_entry->first;
        const auto term = last_entry->second;
        raft_.raft_log().StableEntries(index, term);
    }
}

void RawNode::AdvanceApply() {
    raft_.CommitApply(commit_since_index_);
}

LightReady RawNode::Advance(const Ready& rd) {
    const auto applied = commit_since_index_;
    auto light_rd = AdvanceAppend(rd);
    AdvanceApplyTo(applied);
    return light_rd;
}

LightReady RawNode::GetLightReady() {
    LightReady rd;
    const auto max_size = raft_.max_committed_size_per_ready();

    auto committed_entries = raft_.raft_log().NextEntriesSince(commit_since_index_, max_size);
    if (committed_entries) {
        rd.committed_entries = std::move(*committed_entries);
    }

    raft_.ReduceUncommittedSize(rd.committed_entries);

    if (!rd.committed_entries.empty()) {
        const auto& e = rd.committed_entries.back();
        ASSERT(commit_since_index_ < capnp_util::reader<msg::Entry>(e).getIndex());
        commit_since_index_ = capnp_util::reader<msg::Entry>(e).getIndex();
    }

    if (!raft_.messages().empty()) {
        rd.messages = std::move(raft_.messages());
        raft_.messages().clear();
    }

    return rd;
}

bool RawNode::Tick() {
    return raft_.Tick();
}

Result<void> RawNode::Campaign() {
    Message m = capnp_util::make<msg::Message>();
    auto builder = capnp_util::builder<msg::Message>(m);
    builder.setMsgType(static_cast<MessageType>(static_cast<int>(MessageType::MSG_HUP)));
    return raft_.Step(m);
}

Result<void> RawNode::Propose(const std::string& ctx, const std::string& data) {
    Message m = capnp_util::make<msg::Message>();
    auto m_builder = capnp_util::builder<msg::Message>(m);
    m_builder.setMsgType(static_cast<MessageType>(static_cast<int>(MessageType::MSG_PROPOSE)));
    m_builder.setFrom(raft_.id());

    auto entries = m_builder.initEntries(1);
    auto entry_builder = entries[0];
    entry_builder.setData(kj::arrayPtr(reinterpret_cast<const kj::byte*>(data.data()), data.size())
    );
    entry_builder.setContext(kj::arrayPtr(reinterpret_cast<const kj::byte*>(ctx.data()), ctx.size())
    );

    return raft_.Step(m);
}

void RawNode::Ping() {
    return raft_.Ping();
}

Result<void> RawNode::ProposeConfChange(const std::string& ctx, const ConfChangeV2& cc) {
    Message m = capnp_util::make<msg::Message>();
    auto m_builder = capnp_util::builder<msg::Message>(m);
    m_builder.setMsgType(MessageType::MSG_PROPOSE);

    auto entries = m_builder.initEntries(1);
    auto entry_builder = entries[0];
    entry_builder.setEntryType(
        static_cast<EntryType>(static_cast<int>(EntryType::ENTRY_CONF_CHANGE_V2))
    );

    const std::string serialized = capnp_util::toString(cc);
    entry_builder.setData(
        kj::arrayPtr(reinterpret_cast<const kj::byte*>(serialized.data()), serialized.size())
    );
    entry_builder.setContext(kj::arrayPtr(reinterpret_cast<const kj::byte*>(ctx.data()), ctx.size())
    );

    return raft_.Step(m);
}

Result<ConfState> RawNode::ApplyConfChange(const ConfChangeV2& cc) {
    return raft_.ApplyConfChange(cc);
}

Result<void> RawNode::Step(Message m) {
    auto reader = capnp_util::reader<msg::Message>(m);
    if (IsLocalMessage(reader.getMsgType())) {
        return RaftError(RaftErrorCode::StepLocalMsg);
    }

    if (raft_.progress_tracker().get(reader.getFrom()) != nullptr ||
        !IsResponseMessage(reader.getMsgType())) {
        return raft_.Step(m);
    }

    return RaftError(RaftErrorCode::StepPeerNotFound);
}

void RawNode::OnEntriesFetched(const GetEntriesContext& ctx) {
    switch (ctx.what) {
        case GetEntriesFor::SendAppend: {
            const auto to = ctx.payload.send_append.to;
            const auto term = ctx.payload.send_append.term;
            const auto aggressively = ctx.payload.send_append.aggressively;
            if (raft_.term() != term || raft_.state() != StateRole::Leader) {
                return;
            }

            if (raft_.progress_tracker().get(to) == nullptr) {
                return;
            }

            if (aggressively) {
                raft_.SendAppendAggressively(to);
            } else {
                raft_.SendAppend(to);
            }

            break;
        }

        case GetEntriesFor::Empty:
            break;

        default:
            PANIC("shouldn't call callback on non-async context");
    }
}

}  // namespace raftpp
