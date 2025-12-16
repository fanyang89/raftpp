#pragma once

namespace raftpp::raftor {

class StateMachine {
  public:
    virtual void TakeSnapshot() = 0;
};

}  // namespace raftpp::raftor
