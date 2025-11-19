#include "raftpp/readonly.h"

namespace raftpp {

ReadOnlyOption ReadOnly::option() const {
    return option_;
}

}  // namespace raftpp
