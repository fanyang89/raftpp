#include "raftor/wal/segment_manager.h"

#include <algorithm>

#include <spdlog/spdlog.h>

namespace raftpp::wal {

SegmentManager::SegmentManager(const std::filesystem::path& dir, const WALConfig& config)
    : dir_(dir), config_(config) {}

Result<void> SegmentManager::Initialize() {
    // Create directory if it doesn't exist
    std::error_code ec;
    if (!std::filesystem::exists(dir_)) {
        std::filesystem::create_directories(dir_, ec);
        if (ec) {
            return RaftError(
                StorageErrorOther{fmt::format("failed to create WAL directory: {}", ec.message())}
            );
        }
    }

    // Scan for existing segment files
    std::vector<std::filesystem::path> segment_paths;
    for (const auto& entry : std::filesystem::directory_iterator(dir_, ec)) {
        if (ec) {
            return RaftError(
                StorageErrorOther{fmt::format("failed to iterate WAL directory: {}", ec.message())}
            );
        }

        if (entry.is_regular_file()) {
            auto segment_id = Segment::ParseSegmentId(entry.path());
            if (segment_id) {
                segment_paths.push_back(entry.path());
            }
        }
    }

    // Sort by path (which is sorted by segment_id due to naming convention)
    std::sort(segment_paths.begin(), segment_paths.end());

    // Open all segments
    for (const auto& path : segment_paths) {
        auto segment = Segment::Open(path);
        if (!segment) {
            // Log warning but continue - we might be able to recover partial data
            SPDLOG_WARN("failed to open segment {}: {}", path.string(), segment.error().ToString());
            continue;
        }

        uint64_t seg_id = (*segment)->segment_id();
        segments_[seg_id] = std::move(*segment);

        if (seg_id > current_segment_id_) {
            current_segment_id_ = seg_id;
        }
    }

    SPDLOG_DEBUG("initialized segment manager with {} segments", segments_.size());

    return {};
}

Result<Segment*> SegmentManager::GetCurrentSegment(uint64_t first_index_hint) {
    if (segments_.empty()) {
        // Create the first segment
        return RollToNewSegment(first_index_hint);
    }

    // Return the current segment
    auto it = segments_.find(current_segment_id_);
    if (it == segments_.end()) {
        return RaftError(StorageErrorCode::CurrentSegmentNotFound);
    }

    return it->second.get();
}

Result<Segment*> SegmentManager::RollToNewSegment(uint64_t first_index) {
    // Sync current segment before rolling
    if (!segments_.empty()) {
        auto it = segments_.find(current_segment_id_);
        if (it != segments_.end()) {
            auto result = it->second->Sync();
            if (!result) {
                SPDLOG_WARN("failed to sync segment before roll: {}", result.error().ToString());
            }
        }
    }

    // Create new segment
    uint64_t new_segment_id = current_segment_id_ + 1;
    auto path = dir_ / Segment::MakeSegmentFilename(new_segment_id);

    auto segment = Segment::Create(
        path, new_segment_id, first_index, config_.preallocate, config_.segment_size
    );
    if (!segment) {
        return segment.error();
    }

    Segment* segment_ptr = segment->get();
    segments_[new_segment_id] = std::move(*segment);
    current_segment_id_ = new_segment_id;

    SPDLOG_DEBUG("rolled to new segment {} with first_index={}", new_segment_id, first_index);

    return segment_ptr;
}

Segment* SegmentManager::GetSegmentForIndex(uint64_t index) {
    // Find the segment containing this index
    // Segments are ordered by segment_id, and each segment has a first_index
    // We need to find the segment where first_index <= index

    Segment* result = nullptr;
    for (auto& [seg_id, segment] : segments_) {
        if (segment->first_index() <= index) {
            result = segment.get();
        } else {
            // Segments are ordered, so we can stop
            break;
        }
    }

    return result;
}

Result<void> SegmentManager::RemoveSegmentsBefore(uint64_t segment_id) {
    std::vector<uint64_t> to_remove;
    for (const auto& [seg_id, segment] : segments_) {
        if (seg_id < segment_id) {
            to_remove.push_back(seg_id);
        }
    }

    for (uint64_t seg_id : to_remove) {
        auto result = RemoveSegment(seg_id);
        if (!result) {
            return result;
        }
    }

    return {};
}

Result<void> SegmentManager::RemoveSegment(uint64_t segment_id) {
    auto it = segments_.find(segment_id);
    if (it == segments_.end()) {
        return {};  // Already removed
    }

    auto path = it->second->path();

    // Close the segment
    auto close_result = it->second->Close();
    if (!close_result) {
        SPDLOG_WARN("failed to close segment before removal: {}", close_result.error().ToString());
    }

    // Remove from map
    segments_.erase(it);

    // Delete the file
    std::error_code ec;
    std::filesystem::remove(path, ec);
    if (ec) {
        return RaftError(
            StorageErrorOther{fmt::format("failed to remove segment file: {}", ec.message())}
        );
    }

    SPDLOG_DEBUG("removed segment {}", segment_id);

    return {};
}

std::vector<SegmentInfo> SegmentManager::ListSegments() const {
    std::vector<SegmentInfo> result;
    result.reserve(segments_.size());

    for (const auto& [seg_id, segment] : segments_) {
        result.push_back(
            SegmentInfo{
                .segment_id = seg_id,
                .first_index = segment->first_index(),
                .path = segment->path(),
            }
        );
    }

    return result;
}

Result<void> SegmentManager::SyncAll() {
    for (auto& [seg_id, segment] : segments_) {
        auto result = segment->Sync();
        if (!result) {
            return result;
        }
    }
    return {};
}

Result<void> SegmentManager::CloseAll() {
    for (auto& [seg_id, segment] : segments_) {
        auto result = segment->Close();
        if (!result) {
            return result;
        }
    }
    segments_.clear();
    return {};
}

uint64_t SegmentManager::NextSegmentId() const {
    return current_segment_id_ + 1;
}

}  // namespace raftpp::wal
