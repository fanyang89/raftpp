#include "test_util.h"

namespace raftpp {

Entry NewEntry(const uint64_t index, const uint64_t term) {
    Entry ent;
    ent.set_term(term);
    ent.set_index(index);
    return ent;
}

}  // namespace raftpp
