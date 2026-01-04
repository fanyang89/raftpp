#include "test_util.h"

#include <google/protobuf/util/message_differencer.h>

namespace raftpp {

Entry NewEntry(const uint64_t index, const uint64_t term) {
    Entry ent;
    ent.set_term(term);
    ent.set_index(index);
    return ent;
}

bool operator==(const Entry& e1, const Entry& e2) {
    return google::protobuf::util::MessageDifferencer::Equals(e1, e2);
}

bool operator==(const Snapshot& e1, const Snapshot& e2) {
    return google::protobuf::util::MessageDifferencer::Equals(e1, e2);
}

}  // namespace raftpp
