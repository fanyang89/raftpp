#include "test_util.h"

#include <absl/strings/str_join.h>
#include <google/protobuf/util/message_differencer.h>
#include <spdlog/fmt/fmt.h>

namespace raftpp {

Entry NewEntry(const uint64_t index, const uint64_t term) {
    Entry ent;
    ent.set_term(term);
    ent.set_index(index);
    return ent;
}

Snapshot NewSnapshot(const uint64_t index, const uint64_t term) {
    Snapshot snap;
    snap.mutable_metadata()->set_index(index);
    snap.mutable_metadata()->set_term(term);
    return snap;
}

bool operator==(const Entry& e1, const Entry& e2) {
    return google::protobuf::util::MessageDifferencer::Equals(e1, e2);
}

bool operator==(const Snapshot& e1, const Snapshot& e2) {
    return google::protobuf::util::MessageDifferencer::Equals(e1, e2);
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

}  // namespace raftpp
