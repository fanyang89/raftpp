#include "harness/interface.h"

#include "raftpp/raft.h"

namespace raftpp {

Interface::Interface(std::unique_ptr<Raft> raft) : raft_(std::move(raft)) {}

Result<void> Interface::Step(Message& m) {
    if (!raft_) {
        return {};
    }
    return raft_->Step(m);
}

std::vector<Message> Interface::ReadMessages() {
    if (!raft_) {
        return {};
    }
    std::vector<Message> msgs;
    msgs.swap(raft_->messages());
    return msgs;
}

void Interface::Persist() {
    // Placeholder implementation
}

Raft& Interface::operator*() {
    return *raft_;
}

Raft* Interface::operator->() {
    return raft_.get();
}

const Raft& Interface::operator*() const {
    return *raft_;
}

const Raft* Interface::operator->() const {
    return raft_.get();
}

bool Interface::HasRaft() const {
    return raft_ != nullptr;
}

}  // namespace raftpp
