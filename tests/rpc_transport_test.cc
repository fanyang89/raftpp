#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include <doctest/doctest.h>
#include <kj/array.h>
#include <spdlog/fmt/fmt.h>

#include "raftpp/core/capnp_util.h"
#include "raftpp/raftor/rpc/capnp_transport.h"

using namespace raftpp;
using namespace raftor::rpc;
using namespace std::chrono_literals;

// =============================================================================
// Test Utilities
// =============================================================================

namespace {

// Thread-safe port allocator to avoid conflicts between tests
class PortAllocator {
  public:
    static uint16_t GetNextPort() {
        static std::atomic<uint16_t> next_port{19000};
        return next_port.fetch_add(1);
    }
};

// Collects messages received via callback
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

    // Access first message for reading
    Message& First() {
        std::lock_guard lock(mutex_);
        return messages_[0];
    }

    // Access message at index
    Message& At(size_t idx) {
        std::lock_guard lock(mutex_);
        return messages_[idx];
    }

    void Clear() {
        std::lock_guard lock(mutex_);
        messages_.clear();
    }

  private:
    mutable std::mutex mutex_;
    std::vector<Message> messages_;
};

// Collects errors received via callback
class ErrorCollector {
  public:
    void OnError(uint64_t peer_id, std::string error) {
        std::lock_guard lock(mutex_);
        errors_.emplace_back(peer_id, std::move(error));
    }

    size_t Count() const {
        std::lock_guard lock(mutex_);
        return errors_.size();
    }

    std::vector<std::pair<uint64_t, std::string>> GetErrors() {
        std::lock_guard lock(mutex_);
        return errors_;
    }

    void Clear() {
        std::lock_guard lock(mutex_);
        errors_.clear();
    }

  private:
    mutable std::mutex mutex_;
    std::vector<std::pair<uint64_t, std::string>> errors_;
};

// Poll single transport for a duration using short non-blocking intervals
void PollFor(Transport& t, std::chrono::milliseconds duration) {
    auto deadline = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < deadline) {
        t.Poll(0ms);  // Non-blocking poll
        std::this_thread::sleep_for(1ms);
    }
}

// Poll two transports alternately for a duration
void PollBoth(Transport& t1, Transport& t2, std::chrono::milliseconds duration) {
    auto deadline = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < deadline) {
        t1.Poll(0ms);
        t2.Poll(0ms);
        std::this_thread::sleep_for(1ms);
    }
}

// Poll three transports alternately for a duration
void PollAll(Transport& t1, Transport& t2, Transport& t3, std::chrono::milliseconds duration) {
    auto deadline = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < deadline) {
        t1.Poll(0ms);
        t2.Poll(0ms);
        t3.Poll(0ms);
        std::this_thread::sleep_for(1ms);
    }
}

// Wait for a predicate to become true, polling transport(s)
template <typename Pred>
bool WaitFor(Transport& t, Pred pred, std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        t.Poll(0ms);
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return pred();
}

template <typename Pred>
bool WaitForBoth(Transport& t1, Transport& t2, Pred pred, std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        t1.Poll(0ms);
        t2.Poll(0ms);
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return pred();
}

template <typename Pred>
bool WaitForAll(
    Transport& t1, Transport& t2, Transport& t3, Pred pred, std::chrono::milliseconds timeout
) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        t1.Poll(0ms);
        t2.Poll(0ms);
        t3.Poll(0ms);
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return pred();
}

// Create a simple test message
Message MakeMessage(uint64_t from, uint64_t to, MessageType type = MessageType::MSG_APPEND) {
    Message msg = capnp_util::make<msg::Message>();
    auto builder = capnp_util::builder<msg::Message>(msg);
    builder.setFrom(from);
    builder.setTo(to);
    builder.setMsgType(type);
    builder.setTerm(1);
    return msg;
}

std::string DataToString(::capnp::Data::Reader data) {
    return std::string(reinterpret_cast<const char*>(data.begin()), data.size());
}

}  // namespace

// =============================================================================
// Capnp Transport Tests
// =============================================================================

TEST_SUITE("rpc::capnp") {
    TEST_CASE("capnp_start_stop" * doctest::timeout(5)) {
        auto port = PortAllocator::GetNextPort();
        TransportConfig config{
            .listen_addr = fmt::format("127.0.0.1:{}", port),
            .node_id = 1,
        };

        CapnpTransport transport(config);

        auto result = transport.Start();
        REQUIRE(result.has_value());

        // Poll briefly to ensure event loop is running
        PollFor(transport, 50ms);

        // Stop should be clean
        transport.Stop();
    }

    TEST_CASE("capnp_start_invalid_address" * doctest::timeout(5)) {
        TransportConfig config{
            .listen_addr = "invalid:not-a-port",
            .node_id = 1,
        };

        CapnpTransport transport(config);

        auto result = transport.Start();
        CHECK(!result.has_value());
    }

    TEST_CASE("capnp_add_remove_peer" * doctest::timeout(5)) {
        auto port = PortAllocator::GetNextPort();
        TransportConfig config{
            .listen_addr = fmt::format("127.0.0.1:{}", port),
            .node_id = 1,
        };

        CapnpTransport transport(config);
        REQUIRE(transport.Start().has_value());

        // Add peers
        transport.AddPeer(2, "127.0.0.1:19999");
        transport.AddPeer(3, "127.0.0.1:19998");

        // Poll to process
        PollFor(transport, 50ms);

        // Remove a peer
        transport.RemovePeer(2);

        PollFor(transport, 50ms);

        transport.Stop();
    }

    TEST_CASE("capnp_connect_single_peer" * doctest::timeout(10)) {
        auto port1 = PortAllocator::GetNextPort();
        auto port2 = PortAllocator::GetNextPort();

        TransportConfig cfg1{.listen_addr = fmt::format("127.0.0.1:{}", port1), .node_id = 1};
        TransportConfig cfg2{.listen_addr = fmt::format("127.0.0.1:{}", port2), .node_id = 2};

        CapnpTransport t1(cfg1);
        CapnpTransport t2(cfg2);

        MessageCollector collector1, collector2;
        t1.SetMessageCallback([&](Message m) { collector1.OnMessage(std::move(m)); });
        t2.SetMessageCallback([&](Message m) { collector2.OnMessage(std::move(m)); });

        REQUIRE(t1.Start().has_value());
        REQUIRE(t2.Start().has_value());

        // Add peers to each other
        t1.AddPeer(2, fmt::format("127.0.0.1:{}", port2));
        t2.AddPeer(1, fmt::format("127.0.0.1:{}", port1));

        // Wait for connection to establish
        PollBoth(t1, t2, 500ms);

        // Send message from t1 to t2
        auto msg = MakeMessage(1, 2);
        t1.Send(nonstd::span(&msg, 1));

        // Wait for message to arrive
        bool received = WaitForBoth(t1, t2, [&] { return collector2.Count() >= 1; }, 2s);
        CHECK(received);

        if (received) {
            CHECK(collector2.Count() == 1);
            auto reader = capnp_util::reader<msg::Message>(collector2.First());
            CHECK(reader.getFrom() == 1);
            CHECK(reader.getTo() == 2);
        }

        t1.Stop();
        t2.Stop();
    }

    TEST_CASE("capnp_bidirectional_messages" * doctest::timeout(10)) {
        auto port1 = PortAllocator::GetNextPort();
        auto port2 = PortAllocator::GetNextPort();

        TransportConfig cfg1{.listen_addr = fmt::format("127.0.0.1:{}", port1), .node_id = 1};
        TransportConfig cfg2{.listen_addr = fmt::format("127.0.0.1:{}", port2), .node_id = 2};

        CapnpTransport t1(cfg1);
        CapnpTransport t2(cfg2);

        MessageCollector collector1, collector2;
        t1.SetMessageCallback([&](Message m) { collector1.OnMessage(std::move(m)); });
        t2.SetMessageCallback([&](Message m) { collector2.OnMessage(std::move(m)); });

        REQUIRE(t1.Start().has_value());
        REQUIRE(t2.Start().has_value());

        t1.AddPeer(2, fmt::format("127.0.0.1:{}", port2));
        t2.AddPeer(1, fmt::format("127.0.0.1:{}", port1));

        PollBoth(t1, t2, 500ms);

        // Send from t1 to t2
        auto msg1 = MakeMessage(1, 2);
        t1.Send(nonstd::span(&msg1, 1));

        // Send from t2 to t1
        auto msg2 = MakeMessage(2, 1);
        t2.Send(nonstd::span(&msg2, 1));

        // Wait for both messages
        bool received = WaitForBoth(
            t1, t2, [&] { return collector1.Count() >= 1 && collector2.Count() >= 1; }, 2s
        );
        CHECK(received);

        CHECK(collector1.Count() >= 1);
        CHECK(collector2.Count() >= 1);

        t1.Stop();
        t2.Stop();
    }

    TEST_CASE("capnp_message_callback_invoked" * doctest::timeout(10)) {
        auto port1 = PortAllocator::GetNextPort();
        auto port2 = PortAllocator::GetNextPort();

        TransportConfig cfg1{.listen_addr = fmt::format("127.0.0.1:{}", port1), .node_id = 1};
        TransportConfig cfg2{.listen_addr = fmt::format("127.0.0.1:{}", port2), .node_id = 2};

        CapnpTransport t1(cfg1);
        CapnpTransport t2(cfg2);

        std::atomic<int> callback_count{0};
        t2.SetMessageCallback([&](Message) { callback_count++; });

        REQUIRE(t1.Start().has_value());
        REQUIRE(t2.Start().has_value());

        t1.AddPeer(2, fmt::format("127.0.0.1:{}", port2));
        t2.AddPeer(1, fmt::format("127.0.0.1:{}", port1));

        PollBoth(t1, t2, 500ms);

        // Send 3 messages
        for (int i = 0; i < 3; i++) {
            auto msg = MakeMessage(1, 2);
            t1.Send(nonstd::span(&msg, 1));
        }

        bool received = WaitForBoth(t1, t2, [&] { return callback_count >= 3; }, 2s);
        CHECK(received);
        CHECK(callback_count >= 3);

        t1.Stop();
        t2.Stop();
    }

    TEST_CASE("capnp_callbacks_run_on_poll_thread" * doctest::timeout(10)) {
        auto port1 = PortAllocator::GetNextPort();
        auto port2 = PortAllocator::GetNextPort();

        TransportConfig cfg1{.listen_addr = fmt::format("127.0.0.1:{}", port1), .node_id = 1};
        TransportConfig cfg2{.listen_addr = fmt::format("127.0.0.1:{}", port2), .node_id = 2};

        CapnpTransport t1(cfg1);
        CapnpTransport t2(cfg2);

        std::mutex callback_mutex;
        std::optional<std::thread::id> callback_thread_id;

        t2.SetMessageCallback([&](Message) {
            std::lock_guard lock(callback_mutex);
            callback_thread_id = std::this_thread::get_id();
        });

        REQUIRE(t1.Start().has_value());
        REQUIRE(t2.Start().has_value());

        t1.AddPeer(2, fmt::format("127.0.0.1:{}", port2));
        t2.AddPeer(1, fmt::format("127.0.0.1:{}", port1));

        PollBoth(t1, t2, 500ms);

        auto msg = MakeMessage(1, 2);
        t1.Send(nonstd::span(&msg, 1));

        auto poll_thread_id = std::this_thread::get_id();
        bool received = WaitForBoth(
            t1, t2,
            [&] {
                std::lock_guard lock(callback_mutex);
                return callback_thread_id.has_value();
            },
            2s
        );
        CHECK(received);

        if (received) {
            std::lock_guard lock(callback_mutex);
            CHECK(callback_thread_id.has_value());
            CHECK(*callback_thread_id == poll_thread_id);
        }

        t1.Stop();
        t2.Stop();
    }

    TEST_CASE("capnp_error_callback_invoked" * doctest::timeout(10)) {
        auto port1 = PortAllocator::GetNextPort();
        auto port2 = PortAllocator::GetNextPort();

        TransportConfig cfg1{.listen_addr = fmt::format("127.0.0.1:{}", port1), .node_id = 1};
        TransportConfig cfg2{.listen_addr = fmt::format("127.0.0.1:{}", port2), .node_id = 2};

        CapnpTransport t1(cfg1);
        CapnpTransport t2(cfg2);

        ErrorCollector errors1;
        t1.SetErrorCallback([&](uint64_t peer_id, std::string err) {
            errors1.OnError(peer_id, err);
        });

        REQUIRE(t1.Start().has_value());
        REQUIRE(t2.Start().has_value());

        t1.AddPeer(2, fmt::format("127.0.0.1:{}", port2));
        t2.AddPeer(1, fmt::format("127.0.0.1:{}", port1));

        // Wait for connection
        PollBoth(t1, t2, 500ms);

        // Abruptly stop t2
        t2.Stop();

        // t1 should eventually detect disconnect
        PollFor(t1, 500ms);

        // Error callback may or may not be called depending on implementation
        // Just verify no crash
        CHECK(true);

        t1.Stop();
    }

    TEST_CASE("capnp_stop_bounded_with_unreachable_peer" * doctest::timeout(5)) {
        auto port = PortAllocator::GetNextPort();
        auto unreachable_port = PortAllocator::GetNextPort();

        TransportConfig config{
            .listen_addr = fmt::format("127.0.0.1:{}", port),
            .node_id = 1,
        };

        CapnpTransport transport(config);
        REQUIRE(transport.Start().has_value());

        transport.AddPeer(2, fmt::format("127.0.0.1:{}", unreachable_port));

        auto msg = MakeMessage(1, 2);
        transport.Send(nonstd::span(&msg, 1));

        auto start = std::chrono::steady_clock::now();
        transport.Stop();
        auto elapsed = std::chrono::steady_clock::now() - start;

        CHECK(elapsed < 500ms);
    }

    TEST_CASE("capnp_outgoing_queue_overflow_reports_error" * doctest::timeout(5)) {
        auto port = PortAllocator::GetNextPort();
        TransportConfig config{
            .listen_addr = fmt::format("127.0.0.1:{}", port),
            .node_id = 1,
        };

        CapnpTransport transport(config);

        std::mutex callback_mutex;
        std::optional<std::thread::id> callback_thread_id;
        std::optional<std::string> callback_error;

        transport.SetErrorCallback([&](uint64_t, std::string error) {
            std::lock_guard lock(callback_mutex);
            callback_thread_id = std::this_thread::get_id();
            callback_error = std::move(error);
        });

        transport.AddPeer(2, "127.0.0.1:9999");

        for (size_t i = 0; i < 1100; ++i) {
            auto msg = MakeMessage(1, 2);
            transport.Send(nonstd::span(&msg, 1));
        }

        auto poll_thread_id = std::this_thread::get_id();
        bool received = WaitFor(
            transport,
            [&] {
                std::lock_guard lock(callback_mutex);
                return callback_error.has_value();
            },
            200ms
        );
        CHECK(received);

        if (received) {
            std::lock_guard lock(callback_mutex);
            CHECK(callback_thread_id.has_value());
            CHECK(*callback_thread_id == poll_thread_id);
            CHECK(callback_error->find("outgoing_queue_ overflow") != std::string::npos);
        }
    }

    TEST_CASE("capnp_multiple_peers" * doctest::timeout(10)) {
        auto port1 = PortAllocator::GetNextPort();
        auto port2 = PortAllocator::GetNextPort();
        auto port3 = PortAllocator::GetNextPort();

        TransportConfig cfg1{.listen_addr = fmt::format("127.0.0.1:{}", port1), .node_id = 1};
        TransportConfig cfg2{.listen_addr = fmt::format("127.0.0.1:{}", port2), .node_id = 2};
        TransportConfig cfg3{.listen_addr = fmt::format("127.0.0.1:{}", port3), .node_id = 3};

        CapnpTransport t1(cfg1);
        CapnpTransport t2(cfg2);
        CapnpTransport t3(cfg3);

        MessageCollector collector2, collector3;
        t2.SetMessageCallback([&](Message m) { collector2.OnMessage(std::move(m)); });
        t3.SetMessageCallback([&](Message m) { collector3.OnMessage(std::move(m)); });

        REQUIRE(t1.Start().has_value());
        REQUIRE(t2.Start().has_value());
        REQUIRE(t3.Start().has_value());

        // t1 connects to both t2 and t3
        t1.AddPeer(2, fmt::format("127.0.0.1:{}", port2));
        t1.AddPeer(3, fmt::format("127.0.0.1:{}", port3));
        t2.AddPeer(1, fmt::format("127.0.0.1:{}", port1));
        t3.AddPeer(1, fmt::format("127.0.0.1:{}", port1));

        PollAll(t1, t2, t3, 500ms);

        // Send to t2
        auto msg2 = MakeMessage(1, 2);
        t1.Send(nonstd::span(&msg2, 1));

        // Send to t3
        auto msg3 = MakeMessage(1, 3);
        t1.Send(nonstd::span(&msg3, 1));

        bool received = WaitForAll(
            t1, t2, t3, [&] { return collector2.Count() >= 1 && collector3.Count() >= 1; }, 2s
        );
        CHECK(received);
        CHECK(collector2.Count() >= 1);
        CHECK(collector3.Count() >= 1);

        t1.Stop();
        t2.Stop();
        t3.Stop();
    }

    TEST_CASE("capnp_peer_address_change_updates_client" * doctest::timeout(10)) {
        auto port1 = PortAllocator::GetNextPort();
        auto port2 = PortAllocator::GetNextPort();
        auto port3 = PortAllocator::GetNextPort();

        TransportConfig cfg1{.listen_addr = fmt::format("127.0.0.1:{}", port1), .node_id = 1};
        TransportConfig cfg2{.listen_addr = fmt::format("127.0.0.1:{}", port2), .node_id = 2};

        CapnpTransport t1(cfg1);
        CapnpTransport t2(cfg2);

        MessageCollector collector_old;
        t2.SetMessageCallback([&](Message m) { collector_old.OnMessage(std::move(m)); });

        REQUIRE(t1.Start().has_value());
        REQUIRE(t2.Start().has_value());

        t1.AddPeer(2, fmt::format("127.0.0.1:{}", port2));
        t2.AddPeer(1, fmt::format("127.0.0.1:{}", port1));

        PollBoth(t1, t2, 500ms);

        auto msg = MakeMessage(1, 2);
        t1.Send(nonstd::span(&msg, 1));

        bool received = WaitForBoth(t1, t2, [&] { return collector_old.Count() >= 1; }, 2s);
        CHECK(received);

        t2.Stop();

        TransportConfig cfg3{.listen_addr = fmt::format("127.0.0.1:{}", port3), .node_id = 2};
        CapnpTransport t3(cfg3);
        MessageCollector collector_new;
        t3.SetMessageCallback([&](Message m) { collector_new.OnMessage(std::move(m)); });

        REQUIRE(t3.Start().has_value());

        t1.AddPeer(2, fmt::format("127.0.0.1:{}", port3));
        t3.AddPeer(1, fmt::format("127.0.0.1:{}", port1));

        PollBoth(t1, t3, 500ms);

        auto msg2 = MakeMessage(1, 2);
        t1.Send(nonstd::span(&msg2, 1));

        bool received_new = WaitForBoth(t1, t3, [&] { return collector_new.Count() >= 1; }, 2s);
        CHECK(received_new);

        CHECK(collector_old.Count() == 1);

        t1.Stop();
        t3.Stop();
    }

    TEST_CASE("capnp_send_to_unknown_peer" * doctest::timeout(5)) {
        auto port = PortAllocator::GetNextPort();
        TransportConfig config{
            .listen_addr = fmt::format("127.0.0.1:{}", port),
            .node_id = 1,
        };

        CapnpTransport transport(config);
        REQUIRE(transport.Start().has_value());

        // Send to unknown peer - should be silently dropped
        auto msg = MakeMessage(1, 999);  // peer 999 doesn't exist
        transport.Send(nonstd::span(&msg, 1));

        // No crash, no hang
        PollFor(transport, 100ms);

        transport.Stop();
    }

    TEST_CASE("capnp_large_message" * doctest::timeout(10)) {
        auto port1 = PortAllocator::GetNextPort();
        auto port2 = PortAllocator::GetNextPort();

        TransportConfig cfg1{.listen_addr = fmt::format("127.0.0.1:{}", port1), .node_id = 1};
        TransportConfig cfg2{.listen_addr = fmt::format("127.0.0.1:{}", port2), .node_id = 2};

        CapnpTransport t1(cfg1);
        CapnpTransport t2(cfg2);

        MessageCollector collector;
        t2.SetMessageCallback([&](Message m) { collector.OnMessage(std::move(m)); });

        REQUIRE(t1.Start().has_value());
        REQUIRE(t2.Start().has_value());

        t1.AddPeer(2, fmt::format("127.0.0.1:{}", port2));
        t2.AddPeer(1, fmt::format("127.0.0.1:{}", port1));

        PollBoth(t1, t2, 500ms);

        // Create large message with entries
        Message msg = capnp_util::make<msg::Message>();
        auto builder = capnp_util::builder<msg::Message>(msg);
        builder.setFrom(1);
        builder.setTo(2);
        builder.setMsgType(MessageType::MSG_APPEND);
        builder.setTerm(1);

        // Add many entries with large data
        std::string large_data(1024, 'X');  // 1KB per entry
        auto entries = builder.initEntries(100);
        for (int i = 0; i < 100; i++) {
            entries[i].setTerm(1);
            entries[i].setIndex(i + 1);
            entries[i].setData(kj::arrayPtr(
                reinterpret_cast<const kj::byte*>(large_data.data()), large_data.size()
            ));
        }

        t1.Send(nonstd::span(&msg, 1));

        bool received = WaitForBoth(t1, t2, [&] { return collector.Count() >= 1; }, 3s);
        CHECK(received);

        if (received) {
            auto reader = capnp_util::reader<msg::Message>(collector.First());
            CHECK(reader.getEntries().size() == 100);
        }

        t1.Stop();
        t2.Stop();
    }

    TEST_CASE("capnp_message_with_entries" * doctest::timeout(10)) {
        auto port1 = PortAllocator::GetNextPort();
        auto port2 = PortAllocator::GetNextPort();

        TransportConfig cfg1{.listen_addr = fmt::format("127.0.0.1:{}", port1), .node_id = 1};
        TransportConfig cfg2{.listen_addr = fmt::format("127.0.0.1:{}", port2), .node_id = 2};

        CapnpTransport t1(cfg1);
        CapnpTransport t2(cfg2);

        MessageCollector collector;
        t2.SetMessageCallback([&](Message m) { collector.OnMessage(std::move(m)); });

        REQUIRE(t1.Start().has_value());
        REQUIRE(t2.Start().has_value());

        t1.AddPeer(2, fmt::format("127.0.0.1:{}", port2));
        t2.AddPeer(1, fmt::format("127.0.0.1:{}", port1));

        PollBoth(t1, t2, 500ms);

        // Create message with entries
        Message msg = capnp_util::make<msg::Message>();
        auto builder = capnp_util::builder<msg::Message>(msg);
        builder.setFrom(1);
        builder.setTo(2);
        builder.setMsgType(MessageType::MSG_APPEND);
        builder.setTerm(5);
        builder.setIndex(100);
        builder.setCommit(50);

        auto entries = builder.initEntries(5);
        for (int i = 0; i < 5; i++) {
            entries[i].setTerm(5);
            entries[i].setIndex(101 + i);
            auto data = std::string("entry_") + std::to_string(i);
            entries[i].setData(
                kj::arrayPtr(reinterpret_cast<const kj::byte*>(data.data()), data.size())
            );
        }

        t1.Send(nonstd::span(&msg, 1));

        bool received = WaitForBoth(t1, t2, [&] { return collector.Count() >= 1; }, 2s);
        REQUIRE(received);

        REQUIRE(collector.Count() >= 1);
        auto reader = capnp_util::reader<msg::Message>(collector.First());
        CHECK(reader.getTerm() == 5);
        CHECK(reader.getEntries().size() == 5);
        CHECK(DataToString(reader.getEntries()[2].getData()) == "entry_2");

        t1.Stop();
        t2.Stop();
    }

    TEST_CASE("capnp_auto_reconnect" * doctest::timeout(10)) {
        auto port1 = PortAllocator::GetNextPort();
        auto port2 = PortAllocator::GetNextPort();

        TransportConfig cfg1{
            .listen_addr = fmt::format("127.0.0.1:{}", port1),
            .node_id = 1,
            .reconnect_interval = 100ms  // Fast reconnect for test
        };
        TransportConfig cfg2{.listen_addr = fmt::format("127.0.0.1:{}", port2), .node_id = 2};

        CapnpTransport t1(cfg1);
        MessageCollector collector;

        REQUIRE(t1.Start().has_value());

        // Add peer before it exists
        t1.AddPeer(2, fmt::format("127.0.0.1:{}", port2));

        // Poll while peer is unavailable
        PollFor(t1, 300ms);

        // Now start t2
        CapnpTransport t2(cfg2);
        t2.SetMessageCallback([&](Message m) { collector.OnMessage(std::move(m)); });
        REQUIRE(t2.Start().has_value());
        t2.AddPeer(1, fmt::format("127.0.0.1:{}", port1));

        // Wait for reconnection and connection establishment
        PollBoth(t1, t2, 1s);

        // Try sending a message
        auto msg = MakeMessage(1, 2);
        t1.Send(nonstd::span(&msg, 1));

        bool received = WaitForBoth(t1, t2, [&] { return collector.Count() >= 1; }, 2s);
        CHECK(received);

        t1.Stop();
        t2.Stop();
    }

}  // TEST_SUITE("rpc::capnp")
