#pragma once

#include <cstdint>

#include "raftpp.capnp.h"
#include "raftpp/core/types.h"
#include "raftpp/raftor/wal/crc32c.h"

namespace raftpp::raftor {

inline bool IsChecksumExemptEntry(msg::Entry::Reader entry_reader) {
    return entry_reader.getEntryType() == EntryType::ENTRY_NORMAL &&
        entry_reader.getData().size() == 0 && entry_reader.getContext().size() == 0;
}

inline uint32_t ComputeEntryChecksum(msg::Entry::Reader entry_reader) {
    wal::CRC32C crc;
    const uint32_t entry_type = static_cast<uint32_t>(entry_reader.getEntryType());
    crc.Update(&entry_type, sizeof(entry_type));

    auto context = entry_reader.getContext();
    crc.Update(context.begin(), context.size());

    auto data = entry_reader.getData();
    crc.Update(data.begin(), data.size());
    return crc.Finalize();
}

inline void SetEntryChecksum(msg::Entry::Builder entry_builder) {
    entry_builder.setChecksum(ComputeEntryChecksum(entry_builder.asReader()));
}

}  // namespace raftpp::raftor
