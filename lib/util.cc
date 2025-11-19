#include "raftpp/util.h"

namespace raftpp {

size_t EntryApproximateSize(const Entry& ent) {
    // TODO(fanyang) check the 12
    return ent.data().size() + ent.context().size() + 12;
}

}  // namespace raftpp
