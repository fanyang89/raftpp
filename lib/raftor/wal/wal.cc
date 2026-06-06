#include "raftpp/raftor/wal/wal.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <mutex>
#include <utility>

#include <capnp/common.h>
#include <kj/common.h>

#include "raftpp/core/capnp_util.h"
#include "raftpp/fmt.h"
#include "raftpp/logging.h"
#include "raftpp/raftor/wal/record.h"
#include "raftpp/raftor/wal/segment_manager.h"

namespace raftpp::raftor::wal {

namespace {

std::filesystem::path SnapshotPath(const std::filesystem::path& dir) {
    return dir / "snapshot";
}

std::filesystem::path SnapshotTmpPath(const std::filesystem::path& dir) {
    return dir / "snapshot.tmp";
}

Result<void> SyncDirectory(const std::filesystem::path& dir) {
    int dir_fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (dir_fd < 0) {
        return RaftError(StorageErrorOther{
            fmt::format("failed to open snapshot directory for sync: {}", std::strerror(errno))
        });
    }
    if (::fsync(dir_fd) < 0) {
        const int err = errno;
        ::close(dir_fd);
        return RaftError(StorageErrorOther{
            fmt::format("failed to sync snapshot directory: {}", std::strerror(err))
        });
    }
    ::close(dir_fd);
    return {};
}

Result<void> WriteAll(int fd, nonstd::span<const uint8_t> data) {
    size_t written = 0;
    while (written < data.size()) {
        const ssize_t n = ::write(fd, data.data() + written, data.size() - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return RaftError(
                StorageErrorOther{fmt::format("failed to write snapshot: {}", std::strerror(errno))}
            );
        }
        if (n == 0) {
            return RaftError(StorageErrorOther{"failed to write snapshot: zero-byte write"});
        }
        written += static_cast<size_t>(n);
    }
    return {};
}

Result<std::vector<uint8_t>> ReadFile(const std::filesystem::path& path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return RaftError(
            StorageErrorOther{fmt::format("failed to open snapshot: {}", std::strerror(errno))}
        );
    }

    off_t size = ::lseek(fd, 0, SEEK_END);
    if (size < 0) {
        const int err = errno;
        ::close(fd);
        return RaftError(
            StorageErrorOther{fmt::format("failed to get snapshot size: {}", std::strerror(err))}
        );
    }
    if (size > static_cast<off_t>(std::numeric_limits<uint32_t>::max())) {
        ::close(fd);
        return RaftError(
            StorageErrorOther{"failed to read snapshot: file size exceeds maximum limit"}
        );
    }
    if (::lseek(fd, 0, SEEK_SET) < 0) {
        const int err = errno;
        ::close(fd);
        return RaftError(
            StorageErrorOther{fmt::format("failed to rewind snapshot: {}", std::strerror(err))}
        );
    }

    std::vector<uint8_t> data(static_cast<size_t>(size));
    size_t read_bytes = 0;
    while (read_bytes < data.size()) {
        const ssize_t n = ::read(fd, data.data() + read_bytes, data.size() - read_bytes);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            const int err = errno;
            ::close(fd);
            return RaftError(
                StorageErrorOther{fmt::format("failed to read snapshot: {}", std::strerror(err))}
            );
        }
        if (n == 0) {
            ::close(fd);
            return RaftError(StorageErrorOther{"failed to read snapshot: unexpected EOF"});
        }
        read_bytes += static_cast<size_t>(n);
    }

    ::close(fd);
    return data;
}

Result<void> PersistSnapshotFile(const std::filesystem::path& dir, const Snapshot& snapshot) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return RaftError(
            StorageErrorOther{fmt::format("failed to create snapshot directory: {}", ec.message())}
        );
    }

    auto bytes = capnp_util::toBytes(snapshot);
    const auto tmp_path = SnapshotTmpPath(dir);
    const auto path = SnapshotPath(dir);

    int fd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return RaftError(StorageErrorOther{
            fmt::format("failed to create snapshot temp file: {}", std::strerror(errno))
        });
    }

    auto cleanup_tmp = [&tmp_path] {
        ::unlink(tmp_path.c_str());
    };
    if (auto result = WriteAll(fd, nonstd::span<const uint8_t>(bytes.data(), bytes.size()));
        !result) {
        ::close(fd);
        cleanup_tmp();
        return result.error();
    }

    if (::fsync(fd) < 0) {
        const int err = errno;
        ::close(fd);
        cleanup_tmp();
        return RaftError(StorageErrorOther{
            fmt::format("failed to sync snapshot temp file: {}", std::strerror(err))
        });
    }

    if (::close(fd) < 0) {
        const int err = errno;
        cleanup_tmp();
        return RaftError(StorageErrorOther{
            fmt::format("failed to close snapshot temp file: {}", std::strerror(err))
        });
    }

    if (::rename(tmp_path.c_str(), path.c_str()) < 0) {
        const int err = errno;
        cleanup_tmp();
        return RaftError(StorageErrorOther{
            fmt::format("failed to install snapshot file: {}", std::strerror(err))
        });
    }

    return SyncDirectory(dir);
}

}  // namespace

WAL::~WAL() {
    auto result = Close();
    if (!result) {
        RAFTPP_LOG_ERROR("failed to close WAL: {}", result.error().ToString());
    }
}

Result<std::unique_ptr<WAL>> WAL::Open(const WALConfig& config) {
    auto wal = std::unique_ptr<WAL>(new WAL());
    auto result = wal->Initialize(config);
    if (!result) {
        return result.error();
    }
    return wal;
}

Result<void> WAL::Initialize(const WALConfig& config) {
    config_ = config;

    auto selection = SelectSegmentIoBackend(config_);
    if (!selection) {
        return selection.error();
    }

    effective_io_backend_ = selection->effective_backend;
    io_backend_note_ = std::move(selection->note);

    // Allocate write buffer
    write_buffer_.resize(config_.write_buffer_size);
    write_buffer_used_ = 0;

    // Initialize metadata store
    metadata_store_ = std::make_unique<MetadataStore>(config_.dir);
    auto meta_result = metadata_store_->Initialize();
    if (!meta_result) {
        return meta_result;
    }

    // Initialize segment manager
    segment_manager_ =
        std::make_unique<SegmentManager>(config_.dir, config_, std::move(selection->io_factory));
    auto seg_result = segment_manager_->Initialize();
    if (!seg_result) {
        return seg_result;
    }

    // Recover state
    auto recover_result = Recover();
    if (!recover_result) {
        return recover_result;
    }

    RAFTPP_LOG_INFO(
        "WAL opened at {}: first_index={}, last_index={}, term={}, vote={}", config_.dir.string(),
        first_index_, LastIndexUnlocked(),
        capnp_util::reader<msg::HardState>(hard_state_).getTerm(),
        capnp_util::reader<msg::HardState>(hard_state_).getVote()
    );

    return {};
}

Result<void> WAL::Recover() {
    // Load metadata
    auto meta = metadata_store_->Load();
    if (!meta) {
        return meta.error();
    }

    hard_state_ = std::move(meta->hard_state);
    conf_state_ = std::move(meta->conf_state);
    first_index_ = meta->first_index;
    snapshot_index_ = meta->snapshot_index;
    snapshot_term_ = meta->snapshot_term;

    if (snapshot_index_ > 0) {
        auto hs_builder = capnp_util::builder<msg::HardState>(hard_state_);
        hs_builder.setTerm(std::max(hs_builder.getTerm(), snapshot_term_));
        if (hs_builder.getCommit() < snapshot_index_) {
            hs_builder.setCommit(snapshot_index_);
        }
    }

    index_.SetFirstIndex(first_index_);

    // Replay all segments
    auto segments = segment_manager_->ListSegments();
    for (const auto& seg_info : segments) {
        auto* segment = segment_manager_->GetSegmentForIndex(seg_info.first_index);
        if (segment) {
            auto result = ReplaySegment(segment);
            if (!result) {
                RAFTPP_LOG_WARN(
                    "failed to replay segment {}: {}", seg_info.segment_id,
                    result.error().ToString()
                );
                // Continue with partial recovery
            }
        }
    }

    // Verify consistency
    uint64_t last_idx = LastIndexUnlocked();
    auto hs_reader = capnp_util::reader<msg::HardState>(hard_state_);
    if (last_idx < hs_reader.getCommit()) {
        return RaftError(FatalError{fmt::format(
            "WAL inconsistent: last_index {} < committed {}", last_idx, hs_reader.getCommit()
        )});
    }

    RAFTPP_LOG_DEBUG(
        "WAL recovery complete: {} entries from index {} to {}", index_.size(), first_index_,
        LastIndexUnlocked()
    );

    return {};
}

Result<void> WAL::ReplaySegment(Segment* segment) {
    uint64_t offset = sizeof(SegmentHeader);
    uint64_t segment_size = segment->write_offset();

    while (offset < segment_size) {
        // Read record header
        if (offset + sizeof(RecordHeader) > segment_size) {
            // Incomplete header - truncate
            RAFTPP_LOG_WARN("incomplete record header at offset {}, truncating", offset);
            auto result = segment->Truncate(offset);
            if (!result) {
                return result;
            }
            break;
        }

        auto header_data = segment->Read(offset, sizeof(RecordHeader));
        if (!header_data) {
            return header_data.error();
        }

        RecordHeader header = RecordHeader::Deserialize(
            nonstd::span<const uint8_t, 16>(header_data->data(), sizeof(RecordHeader))
        );

        // Check for end of data (zero-filled preallocated space)
        if (header.length == 0 && header.type == 0) {
            auto result = segment->Truncate(offset);
            if (!result) {
                return result;
            }
            break;
        }

        uint32_t total_size = header.TotalSize();
        if (offset + total_size > segment_size) {
            // Incomplete record - truncate
            RAFTPP_LOG_WARN("incomplete record at offset {}, truncating", offset);
            auto result = segment->Truncate(offset);
            if (!result) {
                return result;
            }
            break;
        }

        // Read full record
        auto record_data = segment->Read(offset, total_size);
        if (!record_data) {
            return record_data.error();
        }

        // Parse and verify CRC
        RecordParser parser(*record_data);
        if (!parser.IsValid()) {
            // CRC mismatch - truncate
            RAFTPP_LOG_WARN("CRC mismatch at offset {}, truncating", offset);
            auto result = segment->Truncate(offset);
            if (!result) {
                return result;
            }
            break;
        }

        // Process record
        switch (parser.Type()) {
            case RecordType::Entry: {
                Entry entry;
                try {
                    auto payload = parser.Payload();
                    const ::capnp::word* words =
                        reinterpret_cast<const ::capnp::word*>(payload.data());
                    size_t word_count = payload.size() / sizeof(::capnp::word);
                    entry = capnp_util::fromWords<msg::Entry>(
                        kj::ArrayPtr<const ::capnp::word>(words, word_count)
                    );
                } catch (...) {
                    RAFTPP_LOG_WARN("failed to parse entry at offset {}", offset);
                    break;
                }

                auto entry_reader = capnp_util::reader<msg::Entry>(entry);
                // Only add entries >= first_index (entries before may have been compacted)
                if (entry_reader.getIndex() >= first_index_) {
                    index_.Insert(
                        entry_reader.getIndex(), segment->segment_id(), offset, total_size,
                        entry_reader.getTerm()
                    );
                }
                break;
            }
            case RecordType::EntryBatch: {
                // Parse multiple entries
                size_t pos = 0;
                auto payload = parser.Payload();
                while (pos < payload.size()) {
                    // Read entry length (4 bytes)
                    if (pos + 4 > payload.size())
                        break;
                    uint32_t entry_len;
                    std::memcpy(&entry_len, payload.data() + pos, sizeof(entry_len));
                    pos += 4;

                    if (pos + entry_len > payload.size())
                        break;

                    Entry entry;
                    try {
                        const ::capnp::word* words =
                            reinterpret_cast<const ::capnp::word*>(payload.data() + pos);
                        size_t word_count = entry_len / sizeof(::capnp::word);
                        entry = capnp_util::fromWords<msg::Entry>(
                            kj::ArrayPtr<const ::capnp::word>(words, word_count)
                        );

                        auto entry_reader = capnp_util::reader<msg::Entry>(entry);
                        if (entry_reader.getIndex() >= first_index_) {
                            // For batch, we store the batch offset but track individual entries
                            index_.Insert(
                                entry_reader.getIndex(), segment->segment_id(), offset, total_size,
                                entry_reader.getTerm()
                            );
                        }
                    } catch (...) {
                        // Skip invalid entry
                    }
                    pos += entry_len;
                }
                break;
            }
            case RecordType::HardState: {
                HardState hs;
                try {
                    auto payload = parser.Payload();
                    const ::capnp::word* words =
                        reinterpret_cast<const ::capnp::word*>(payload.data());
                    size_t word_count = payload.size() / sizeof(::capnp::word);
                    hs = capnp_util::fromWords<msg::HardState>(
                        kj::ArrayPtr<const ::capnp::word>(words, word_count)
                    );

                    // Only update if this is newer
                    if (capnp_util::reader<msg::HardState>(hs).getTerm() >=
                        capnp_util::reader<msg::HardState>(hard_state_).getTerm()) {
                        hard_state_ = std::move(hs);
                    }
                } catch (...) {
                    // Skip invalid hard state
                }
                break;
            }
        }

        offset += total_size;
    }

    return {};
}

Result<void> WAL::Append(nonstd::span<const Entry> entries) {
    if (entries.empty()) {
        return {};
    }

    std::unique_lock lock(mutex_);

    // Verify entries are continuous
    uint64_t expected_index = LastIndexUnlocked() + 1;
    auto first_entry_reader = capnp_util::reader<msg::Entry>(entries.front());
    if (first_entry_reader.getIndex() != expected_index) {
        // Handle truncation case - entries may be replacing existing ones
        if (first_entry_reader.getIndex() < expected_index &&
            first_entry_reader.getIndex() >= first_index_) {
            // Truncate the index
            index_.TruncateFrom(first_entry_reader.getIndex());
        } else if (first_entry_reader.getIndex() != expected_index) {
            return RaftError(FatalError{fmt::format(
                "non-continuous entries: expected {}, got {}", expected_index,
                first_entry_reader.getIndex()
            )});
        }
    }

    auto segment_result = GetCurrentSegmentForAppend(first_entry_reader.getIndex());
    if (!segment_result) {
        return segment_result.error();
    }
    Segment* segment = *segment_result;

    // Write all entries to buffer first
    for (const auto& entry : entries) {
        auto serialized = capnp_util::toBytes(entry);

        // Build record
        RecordBuilder builder;
        builder.SetType(RecordType::Entry);
        builder.SetPayload(
            std::string(reinterpret_cast<const char*>(serialized.data()), serialized.size())
        );
        auto record = builder.Build();

        auto entry_reader = capnp_util::reader<msg::Entry>(entry);

        auto roll_result = MaybeRollSegmentForAppend(entry_reader.getIndex(), segment);
        if (!roll_result) {
            return roll_result;
        }

        // If a single record does not fit into the write buffer, write it directly.
        if (record.size() > write_buffer_.size()) {
            auto flush_result = FlushWriteBuffer();
            if (!flush_result) {
                return flush_result;
            }

            auto segment_result = GetCurrentSegmentForAppend(entry_reader.getIndex());
            if (!segment_result) {
                return segment_result.error();
            }
            segment = *segment_result;

            uint64_t record_offset = segment->write_offset();
            auto write_result = segment->Append(record);
            if (!write_result) {
                return write_result;
            }

            index_.Insert(
                entry_reader.getIndex(), segment->segment_id(), record_offset,
                static_cast<uint32_t>(record.size()), entry_reader.getTerm()
            );
            continue;
        }

        // Check if buffer has space
        if (write_buffer_used_ + record.size() > write_buffer_.size()) {
            auto flush_result = FlushWriteBuffer();
            if (!flush_result) {
                return flush_result;
            }
        }

        // Copy record to buffer
        std::memcpy(write_buffer_.data() + write_buffer_used_, record.data(), record.size());
        write_buffer_used_ += record.size();

        // Cache pending index information
        pending_entries_.push_back(PendingEntry{
            entry_reader.getIndex(), entry_reader.getTerm(),
            static_cast<uint32_t>(write_buffer_used_ - record.size()),
            static_cast<uint32_t>(record.size())
        });

        auto flush_if_needed_result = FlushWriteBufferIfNeeded();
        if (!flush_if_needed_result) {
            return flush_if_needed_result;
        }
    }

    // Always flush buffer to ensure index is updated
    // This ensures entries are visible via LastIndex() immediately
    auto flush_result = FlushWriteBuffer();
    if (!flush_result) {
        return flush_result;
    }

    // Sync to disk only if configured
    if (config_.sync_on_write) {
        auto sync_result = segment_manager_->SyncAll();
        if (!sync_result) {
            return sync_result;
        }
    }

    return {};
}

Result<Segment*> WAL::GetCurrentSegmentForAppend(uint64_t first_index_hint) {
    auto segment_result = segment_manager_->GetCurrentSegment(first_index_hint);
    if (!segment_result) {
        return segment_result.error();
    }

    Segment* segment = *segment_result;
    auto roll_result = MaybeRollSegmentForAppend(first_index_hint, segment);
    if (!roll_result) {
        return roll_result.error();
    }

    return segment;
}

Result<void> WAL::MaybeRollSegmentForAppend(uint64_t first_index, Segment*& segment) {
    if (segment == nullptr) {
        return RaftError(StorageErrorCode::CurrentSegmentNotFound);
    }

    if (!segment->IsFull(config_.segment_size)) {
        return {};
    }

    auto flush_result = FlushWriteBuffer();
    if (!flush_result) {
        return flush_result;
    }

    auto roll_result = segment_manager_->RollToNewSegment(first_index);
    if (!roll_result) {
        return roll_result.error();
    }
    segment = *roll_result;
    return {};
}

Result<void> WAL::SaveHardState(const HardState& hs) {
    std::unique_lock lock(mutex_);

    hard_state_ = CloneHardState(hs);

    auto serialized = capnp_util::toBytes(hs);
    RecordBuilder builder;
    builder.SetType(RecordType::HardState);
    builder.SetPayload(
        std::string(reinterpret_cast<const char*>(serialized.data()), serialized.size())
    );
    auto record = builder.Build();

    // HardState is persisted immediately for clarity and correctness.
    // Flush any buffered writes first to preserve write order.
    auto flush_result = FlushWriteBuffer();
    if (!flush_result) {
        return flush_result;
    }

    auto segment_result = GetCurrentSegmentForAppend(first_index_);
    if (!segment_result) {
        return segment_result.error();
    }

    auto write_result = (*segment_result)->Append(record);
    if (!write_result) {
        return write_result;
    }

    // Save to metadata file for durability
    auto meta = CreateMetadata();
    auto save_result = metadata_store_->Save(meta);
    if (!save_result) {
        return save_result;
    }

    if (config_.sync_on_write) {
        auto sync_result = segment_manager_->SyncAll();
        if (!sync_result) {
            return sync_result;
        }
    }

    return {};
}

Result<void> WAL::SaveConfState(const ConfState& cs) {
    std::unique_lock lock(mutex_);

    conf_state_ = CloneConfState(cs);

    auto meta = CreateMetadata();
    return metadata_store_->Save(meta);
}

Result<std::vector<Entry>> WAL::ReadEntries(
    uint64_t low, uint64_t high, std::optional<uint64_t> max_size
) const {
    std::shared_lock lock(mutex_);

    if (low < first_index_) {
        return RaftError(StorageErrorCode::Compacted);
    }

    uint64_t last_idx = LastIndexUnlocked();
    if (low > last_idx + 1) {
        return RaftError(StorageErrorCode::Unavailable);
    }

    if (high > last_idx + 1) {
        high = last_idx + 1;
    }

    if (low >= high) {
        return std::vector<Entry>{};
    }

    std::vector<Entry> result;
    uint64_t total_size = 0;

    for (uint64_t idx = low; idx < high; ++idx) {
        auto entry_info = index_.Lookup(idx);
        if (!entry_info) {
            return RaftError(StorageErrorCode::Unavailable);
        }

        auto* segment = segment_manager_->GetSegmentForIndex(idx);
        if (!segment) {
            return RaftError(StorageErrorCode::Unavailable);
        }

        auto record_data = segment->Read(entry_info->offset, entry_info->length);
        if (!record_data) {
            return record_data.error();
        }

        RecordParser parser(*record_data);
        if (!parser.IsValid()) {
            return RaftError(StorageErrorCode::CorruptEntryRecord);
        }

        Entry entry;
        try {
            auto payload = parser.Payload();
            const ::capnp::word* words = reinterpret_cast<const ::capnp::word*>(payload.data());
            size_t word_count = payload.size() / sizeof(::capnp::word);
            entry = capnp_util::fromWords<msg::Entry>(
                kj::ArrayPtr<const ::capnp::word>(words, word_count)
            );
        } catch (...) {
            return RaftError(StorageErrorCode::EntryParseError);
        }

        uint64_t entry_size = parser.Payload().size();
        result.push_back(std::move(entry));
        total_size += entry_size;

        // Check size limit (always include at least one entry)
        if (max_size && total_size >= *max_size && result.size() >= 1) {
            break;
        }
    }

    return result;
}

Result<uint64_t> WAL::Term(uint64_t index) const {
    std::shared_lock lock(mutex_);

    // Special case: snapshot term
    if (index == snapshot_index_ && snapshot_index_ > 0) {
        return snapshot_term_;
    }

    if (index < first_index_) {
        return RaftError(StorageErrorCode::Compacted);
    }

    auto term = index_.Term(index);
    if (!term) {
        return RaftError(StorageErrorCode::Unavailable);
    }

    return *term;
}

uint64_t WAL::FirstIndex() const {
    std::shared_lock lock(mutex_);
    return FirstIndexUnlocked();
}

uint64_t WAL::FirstIndexUnlocked() const {
    return first_index_;
}

uint64_t WAL::LastIndex() const {
    std::shared_lock lock(mutex_);
    return LastIndexUnlocked();
}

uint64_t WAL::LastIndexUnlocked() const {
    if (index_.empty()) {
        return snapshot_index_;
    }
    return index_.last_index();
}

const HardState& WAL::GetHardState() const {
    std::shared_lock lock(mutex_);
    return hard_state_;
}

const ConfState& WAL::GetConfState() const {
    std::shared_lock lock(mutex_);
    return conf_state_;
}

uint64_t WAL::SnapshotIndex() const {
    std::shared_lock lock(mutex_);
    return snapshot_index_;
}

Result<Snapshot> WAL::LoadSnapshot() const {
    std::shared_lock lock(mutex_);
    if (snapshot_index_ == 0) {
        return RaftError(StorageErrorCode::SnapshotTemporarilyUnavailable);
    }

    auto data = ReadFile(SnapshotPath(config_.dir));
    if (!data) {
        return data.error();
    }

    Snapshot snapshot;
    try {
        snapshot = capnp_util::fromBytes<msg::Snapshot>(
            nonstd::span<const uint8_t>(data->data(), data->size())
        );
    } catch (...) {
        return RaftError(StorageErrorOther{"failed to parse snapshot file"});
    }

    auto snap_reader = capnp_util::reader<msg::Snapshot>(snapshot);
    auto meta = snap_reader.getMetadata();
    if (meta.getIndex() != snapshot_index_ || meta.getTerm() != snapshot_term_) {
        return RaftError(StorageErrorOther{fmt::format(
            "snapshot file metadata mismatch: file=({},{}) wal=({},{})", meta.getIndex(),
            meta.getTerm(), snapshot_index_, snapshot_term_
        )});
    }

    return snapshot;
}

uint64_t WAL::LogSizeBytes() const {
    std::shared_lock lock(mutex_);
    if (!segment_manager_ || !metadata_store_) {
        return 0;
    }
    return segment_manager_->TotalSizeBytes() + metadata_store_->SizeBytes();
}

WALIoBackend WAL::EffectiveIoBackend() const {
    std::shared_lock lock(mutex_);
    return effective_io_backend_;
}

std::string_view WAL::IoBackendNote() const {
    std::shared_lock lock(mutex_);
    return io_backend_note_;
}

Result<void> WAL::Compact(uint64_t compact_index) {
    std::unique_lock lock(mutex_);

    if (compact_index <= first_index_) {
        return {};  // Already compacted
    }

    uint64_t last_idx = LastIndexUnlocked();
    if (compact_index > last_idx) {
        return RaftError(
            FatalError{fmt::format("compact index {} > last index {}", compact_index, last_idx)}
        );
    }

    // Update first_index
    first_index_ = compact_index;

    // Truncate index
    index_.TruncateBefore(compact_index);

    // Find segments that can be removed
    std::vector<uint64_t> segments_to_remove;
    for (const auto& [seg_id, segment] : segment_manager_->segments()) {
        // A segment can be removed if all its entries are before compact_index
        // This is a simplification - in practice we'd track the last index per segment
        if (segment->first_index() < compact_index) {
            // Check if there are any entries from this segment still in the index
            bool has_entries = false;
            for (uint64_t idx = first_index_; idx <= last_idx; ++idx) {
                auto entry_info = index_.Lookup(idx);
                if (entry_info && entry_info->segment_id == seg_id) {
                    has_entries = true;
                    break;
                }
            }
            if (!has_entries) {
                segments_to_remove.push_back(seg_id);
            }
        }
    }

    // Save metadata first (for crash safety)
    auto meta = CreateMetadata();
    auto save_result = metadata_store_->Save(meta);
    if (!save_result) {
        return save_result;
    }

    // Remove old segments
    for (uint64_t seg_id : segments_to_remove) {
        auto remove_result = segment_manager_->RemoveSegment(seg_id);
        if (!remove_result) {
            RAFTPP_LOG_WARN(
                "failed to remove segment {}: {}", seg_id, remove_result.error().ToString()
            );
        }
    }

    RAFTPP_LOG_DEBUG("compacted WAL to index {}", compact_index);

    return {};
}

Result<void> WAL::ApplySnapshot(const Snapshot& snapshot) {
    std::unique_lock lock(mutex_);

    auto snap_reader = capnp_util::reader<msg::Snapshot>(snapshot);
    const auto& meta = snap_reader.getMetadata();

    if (meta.getIndex() <= snapshot_index_) {
        return RaftError(StorageErrorCode::SnapshotOutOfDate);
    }

    auto snapshot_result = PersistSnapshotFile(config_.dir, snapshot);
    if (!snapshot_result) {
        return snapshot_result.error();
    }

    const uint64_t new_snapshot_index = meta.getIndex();
    const uint64_t new_snapshot_term = meta.getTerm();
    auto new_conf_state = CloneConfState(meta.getConfState());
    auto new_hard_state = CloneHardState(hard_state_);
    auto hs_builder = capnp_util::builder<msg::HardState>(new_hard_state);
    hs_builder.setTerm(std::max(hs_builder.getTerm(), new_snapshot_term));
    if (hs_builder.getCommit() < new_snapshot_index) {
        hs_builder.setCommit(new_snapshot_index);
    }

    WALMetadata wal_meta;
    wal_meta.hard_state = CloneHardState(new_hard_state);
    wal_meta.conf_state = CloneConfState(new_conf_state);
    wal_meta.first_index = new_snapshot_index + 1;
    wal_meta.snapshot_index = new_snapshot_index;
    wal_meta.snapshot_term = new_snapshot_term;
    auto save_result = metadata_store_->Save(wal_meta);
    if (!save_result) {
        return save_result.error();
    }

    // Update memory only after the snapshot file and metadata are durable.
    snapshot_index_ = new_snapshot_index;
    snapshot_term_ = new_snapshot_term;
    hard_state_ = std::move(new_hard_state);
    conf_state_ = std::move(new_conf_state);
    index_.Clear();
    first_index_ = snapshot_index_ + 1;
    index_.SetFirstIndex(first_index_);

    // Remove all segments (they're all before the snapshot)
    auto remove_result = segment_manager_->RemoveAllSegments();
    if (!remove_result) {
        RAFTPP_LOG_WARN(
            "failed to remove segments after snapshot: {}", remove_result.error().ToString()
        );
    }

    // Re-initialize segment manager
    auto init_result = segment_manager_->Initialize();
    if (!init_result) {
        return init_result;
    }

    RAFTPP_LOG_INFO("applied snapshot at index={}, term={}", snapshot_index_, snapshot_term_);

    return {};
}

Result<void> WAL::Sync() {
    std::unique_lock lock(mutex_);

    auto flush_result = FlushWriteBuffer();
    if (!flush_result) {
        return flush_result;
    }

    return segment_manager_->SyncAll();
}

Result<void> WAL::Close() {
    std::unique_lock lock(mutex_);

    if (segment_manager_) {
        auto flush_result = FlushWriteBuffer();
        if (!flush_result) {
            RAFTPP_LOG_WARN(
                "failed to flush write buffer on close: {}", flush_result.error().ToString()
            );
        }

        auto sync_result = segment_manager_->SyncAll();
        if (!sync_result) {
            RAFTPP_LOG_WARN("failed to sync on close: {}", sync_result.error().ToString());
        }

        auto close_result = segment_manager_->CloseAll();
        if (!close_result) {
            return close_result;
        }
    }

    return {};
}

Result<void> WAL::WriteRecord(RecordType type, nonstd::span<const uint8_t> data) {
    RecordBuilder builder;
    builder.SetType(type);
    builder.SetPayload(data);
    auto record = builder.Build();

    // Check if it fits in write buffer
    if (write_buffer_used_ + record.size() > write_buffer_.size()) {
        auto result = FlushWriteBuffer();
        if (!result) {
            return result;
        }
    }

    // Add to write buffer
    std::memcpy(write_buffer_.data() + write_buffer_used_, record.data(), record.size());
    write_buffer_used_ += record.size();

    return {};
}

Result<void> WAL::FlushWriteBuffer() {
    if (write_buffer_used_ == 0) {
        return {};
    }

    auto segment_result = segment_manager_->GetCurrentSegment(first_index_);
    if (!segment_result) {
        return segment_result.error();
    }
    Segment* segment = *segment_result;

    uint64_t flush_start_offset = segment->write_offset();

    auto write_result =
        segment->Append(nonstd::span<const uint8_t>(write_buffer_.data(), write_buffer_used_));
    if (!write_result) {
        return write_result;
    }

    for (const auto& pending : pending_entries_) {
        uint64_t actual_offset = flush_start_offset + pending.offset_in_buffer;
        index_.Insert(
            pending.index, segment->segment_id(), actual_offset, pending.record_length, pending.term
        );
    }

    pending_entries_.clear();
    write_buffer_used_ = 0;
    return {};
}

Result<void> WAL::MaybeRollSegment() {
    auto segment_result = segment_manager_->GetCurrentSegment(first_index_);
    if (!segment_result) {
        return segment_result.error();
    }

    if ((*segment_result)->IsFull(config_.segment_size)) {
        auto flush_result = FlushWriteBuffer();
        if (!flush_result) {
            return flush_result;
        }

        uint64_t next_index = LastIndexUnlocked() + 1;
        auto roll_result = segment_manager_->RollToNewSegment(next_index);
        if (!roll_result) {
            return roll_result.error();
        }
    }

    return {};
}

bool WAL::ShouldFlushBuffer(Segment* segment) const {
    if (write_buffer_used_ >= config_.write_buffer_size) {
        return true;
    }

    if (segment) {
        uint64_t available = 0;
        if (segment->write_offset() < config_.segment_size) {
            available = config_.segment_size - segment->write_offset();
        }
        if (write_buffer_used_ >= available) {
            return true;
        }
    }

    return false;
}

Result<void> WAL::FlushWriteBufferIfNeeded() {
    auto segment_result = segment_manager_->GetCurrentSegment(first_index_);
    if (!segment_result) {
        return segment_result.error();
    }

    if (ShouldFlushBuffer(*segment_result)) {
        return FlushWriteBuffer();
    }

    return {};
}

}  // namespace raftpp::raftor::wal
