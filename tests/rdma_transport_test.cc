#include "raftpp/raftor/rpc/rdma_transport.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <doctest/doctest.h>

#include "raftpp/core/capnp_util.h"

#if RAFTPP_WITH_RDMA
#include <infiniband/verbs.h>
#endif

using namespace raftpp;
using namespace raftpp::raftor::rpc;
using namespace std::chrono_literals;

namespace {

class MessageCollector {
  public:
    void OnMessage(Message msg) {
        std::lock_guard lock(mutex_);
        messages_.push_back(std::move(msg));
    }

    size_t Count() const {
        std::lock_guard lock(mutex_);
        return messages_.size();
    }

    Message& First() {
        std::lock_guard lock(mutex_);
        return messages_.front();
    }

  private:
    mutable std::mutex mutex_;
    std::vector<Message> messages_;
};

Message MakeMessage(uint64_t from, uint64_t to, MessageType type = MessageType::MSG_APPEND) {
    Message msg = capnp_util::make<msg::Message>();
    auto builder = capnp_util::builder<msg::Message>(msg);
    builder.setFrom(from);
    builder.setTo(to);
    builder.setMsgType(type);
    builder.setTerm(1);
    return msg;
}

bool RdmaDevicesAvailable() {
#if RAFTPP_WITH_RDMA
    int count = 0;
    ibv_device** list = ibv_get_device_list(&count);
    if (!list) {
        return false;
    }
    ibv_free_device_list(list);
    return count > 0;
#else
    return false;
#endif
}

bool RdmaTestEnabled(std::string* addr1, std::string* addr2) {
    const char* enabled = std::getenv("RAFTPP_RDMA_TEST");
    if (!enabled || std::string(enabled) != "1") {
        return false;
    }
    const char* env_addr1 = std::getenv("RAFTPP_RDMA_ADDR1");
    const char* env_addr2 = std::getenv("RAFTPP_RDMA_ADDR2");
    if (!env_addr1 || !env_addr2) {
        return false;
    }
    *addr1 = env_addr1;
    *addr2 = env_addr2;
    return true;
}

void PollBoth(Transport& t1, Transport& t2, std::chrono::milliseconds duration) {
    auto deadline = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < deadline) {
        t1.Poll(0ms);
        t2.Poll(0ms);
        std::this_thread::sleep_for(1ms);
    }
}

}  // namespace

TEST_SUITE("rpc::rdma") {
    TEST_CASE("rdma_send_receive" * doctest::timeout(10)) {
        std::string addr1;
        std::string addr2;
        if (!RdmaTestEnabled(&addr1, &addr2)) {
            return;
        }
        if (!RdmaDevicesAvailable()) {
            return;
        }

        TransportConfig cfg1{
            .listen_addr = addr1,
            .node_id = 1,
            .max_message_size = 1024 * 1024,
        };
        TransportConfig cfg2{
            .listen_addr = addr2,
            .node_id = 2,
            .max_message_size = 1024 * 1024,
        };

        RdmaConfig rdma_cfg;
        rdma_cfg.recv_buffer_count = 32;
        rdma_cfg.send_buffer_count = 32;
        rdma_cfg.buffer_size = 1024 * 1024;
        rdma_cfg.cq_depth = 64;
        rdma_cfg.qp_depth = 64;

        RdmaTransport t1(cfg1, rdma_cfg);
        RdmaTransport t2(cfg2, rdma_cfg);

        MessageCollector collector2;
        t2.SetMessageCallback([&](Message m) { collector2.OnMessage(std::move(m)); });

        t1.AddPeer(2, addr2);
        t2.AddPeer(1, addr1);

        auto r1 = t1.Start();
        auto r2 = t2.Start();
        REQUIRE(r1.has_value());
        REQUIRE(r2.has_value());

        auto msg = MakeMessage(1, 2);
        auto deadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < deadline && collector2.Count() == 0) {
            t1.Send(std::span(&msg, 1));
            PollBoth(t1, t2, 20ms);
            std::this_thread::sleep_for(5ms);
        }

        CHECK(collector2.Count() >= 1);
        if (collector2.Count() > 0) {
            auto reader = capnp_util::reader<msg::Message>(collector2.First());
            CHECK(reader.getFrom() == 1);
            CHECK(reader.getTo() == 2);
        }

        t1.Stop();
        t2.Stop();
    }
}
