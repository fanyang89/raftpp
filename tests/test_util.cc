#include "test_util.h"

#include <absl/strings/str_join.h>
#include <google/protobuf/util/message_differencer.h>
#include <spdlog/fmt/fmt.h>

#include "raftpp/core/raft_core.h"
#include "raftpp/core/raftpp.pb.h"

namespace raftpp {

Snapshot NewSnapshot(const uint64_t index, const uint64_t term) {
    Snapshot snap;
    snap.mutable_metadata()->set_index(index);
    snap.mutable_metadata()->set_term(term);
    return snap;
}

doctest::String toString(const std::vector<Entry>& entries) {
    std::vector<std::string> entries_strings;
    entries_strings.reserve(entries.size());
    for (const auto& e : entries) {
        entries_strings.emplace_back(
            fmt::format(
                "Entry {{ index={} term={} size={} }}", e.index(), e.term(), e.data().size()
            )
        );
    }
    const auto s = absl::StrJoin(entries_strings, ",\n");
    if (s.empty()) {
        return "[]";
    }
    return fmt::format("[\n{}\n]", s).c_str();
}

doctest::String toString(const std::optional<std::vector<Entry>>& entries) {
    if (entries) {
        return toString(*entries);
    }
    return "None";
}

doctest::String toString(const RaftError& error) {
    return {error.ToString().c_str()};
}

doctest::String toString(const std::optional<uint64_t>& value) {
    if (value) {
        return fmt::format("{}", *value).c_str();
    }
    return "None";
}

}  // namespace raftpp
