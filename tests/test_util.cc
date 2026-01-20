#include "test_util.h"

#include <absl/strings/str_join.h>
#include <spdlog/fmt/fmt.h>

#include "raftpp/core/capnp_util.h"
#include "raftpp/core/raft_core.h"
#include "raftpp/core/types.h"

namespace raftpp {

Snapshot NewSnapshot(const uint64_t index, const uint64_t term) {
    Snapshot snap = capnp_util::make<msg::Snapshot>();
    auto builder = capnp_util::builder<msg::Snapshot>(snap);
    auto meta_builder = builder.initMetadata();
    meta_builder.setIndex(index);
    meta_builder.setTerm(term);
    return snap;
}

doctest::String toString(const std::vector<Entry>& entries) {
    std::vector<std::string> entries_strings;
    entries_strings.reserve(entries.size());
    for (const auto& e : entries) {
        auto reader = capnp_util::reader<msg::Entry>(e);
        auto data = reader.getData();
        entries_strings.emplace_back(
            fmt::format(
                "Entry {{ index={} term={} size={} }}", reader.getIndex(), reader.getTerm(),
                data.size()
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
