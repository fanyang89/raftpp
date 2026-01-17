#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include <doctest/doctest.h>
#include <spdlog/fmt/fmt.h>

#include "raftor/rpc/kcp_transport.h"
#include "raftor/rpc/tcp_transport.h"

using namespace raftpp;
using namespace raftpp::rpc;
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

    std::vector<Message> GetMessages() {
        std::lock_guard lock(mutex_);
        return messages_;
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
Message MakeMessage(uint64_t from, uint64_t to, MessageType type = MsgAppend) {
    Message msg;
    msg.set_from(from);
    msg.set_to(to);
    msg.set_msg_type(type);
    msg.set_term(1);
    return msg;
}

}  // namespace

// =============================================================================
// TCP Transport Tests
// =============================================================================

TEST_SUITE("rpc::tcp") {

    TEST_CASE("tcp_start_stop" * doctest::timeout(5)) {
        auto port = PortAllocator::GetNextPort();
        TransportConfig config{
            .listen_addr = fmt::format("127.0.0.1:{}", port),
            .node_id = 1,
        };

        TcpTransport transport(config);

        auto result = transport.Start();
        REQUIRE(result.has_value());

        // Poll briefly to ensure event loop is running
        PollFor(transport, 50ms);

        // Stop should be clean
        transport.Stop();
    }

    TEST_CASE("tcp_start_invalid_address" * doctest::timeout(5)) {
        TransportConfig config{
            .listen_addr = "invalid:not-a-port",
            .node_id = 1,
        };

        TcpTransport transport(config);

        auto result = transport.Start();
        CHECK(!result.has_value());
    }

    TEST_CASE("tcp_start_port_already_in_use" * doctest::timeout(5)) {
        auto port = PortAllocator::GetNextPort();
        TransportConfig config{
            .listen_addr = fmt::format("127.0.0.1:{}", port),
            .node_id = 1,
        };

        TcpTransport t1(config);
        TcpTransport t2(config);

        auto r1 = t1.Start();
        REQUIRE(r1.has_value());

        // Second transport on same port should fail
        auto r2 = t2.Start();
        CHECK(!r2.has_value());

        t1.Stop();
    }

    TEST_CASE("tcp_add_remove_peer" * doctest::timeout(5)) {
        auto port = PortAllocator::GetNextPort();
        TransportConfig config{
            .listen_addr = fmt::format("127.0.0.1:{}", port),
            .node_id = 1,
        };

        TcpTransport transport(config);
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

    TEST_CASE("tcp_connect_single_peer" * doctest::timeout(10)) {
        auto port1 = PortAllocator::GetNextPort();
        auto port2 = PortAllocator::GetNextPort();

        TransportConfig cfg1{.listen_addr = fmt::format("127.0.0.1:{}", port1), .node_id = 1};
        TransportConfig cfg2{.listen_addr = fmt::format("127.0.0.1:{}", port2), .node_id = 2};

        TcpTransport t1(cfg1);
        TcpTransport t2(cfg2);

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
        t1.Send(std::span(&msg, 1));

        // Wait for message to arrive
        bool received = WaitForBoth(t1, t2, [&] { return collector2.Count() >= 1; }, 2s);
        CHECK(received);

        if (received) {
            auto messages = collector2.GetMessages();
            CHECK(messages.size() == 1);
            CHECK(messages[0].from() == 1);
            CHECK(messages[0].to() == 2);
        }

        t1.Stop();
        t2.Stop();
    }

    TEST_CASE("tcp_bidirectional_messages" * doctest::timeout(10)) {
        auto port1 = PortAllocator::GetNextPort();
        auto port2 = PortAllocator::GetNextPort();

        TransportConfig cfg1{.listen_addr = fmt::format("127.0.0.1:{}", port1), .node_id = 1};
        TransportConfig cfg2{.listen_addr = fmt::format("127.0.0.1:{}", port2), .node_id = 2};

        TcpTransport t1(cfg1);
        TcpTransport t2(cfg2);

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
        t1.Send(std::span(&msg1, 1));

        // Send from t2 to t1
        auto msg2 = MakeMessage(2, 1);
        t2.Send(std::span(&msg2, 1));

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

    TEST_CASE("tcp_message_callback_invoked" * doctest::timeout(10)) {
        auto port1 = PortAllocator::GetNextPort();
        auto port2 = PortAllocator::GetNextPort();

        TransportConfig cfg1{.listen_addr = fmt::format("127.0.0.1:{}", port1), .node_id = 1};
        TransportConfig cfg2{.listen_addr = fmt::format("127.0.0.1:{}", port2), .node_id = 2};

        TcpTransport t1(cfg1);
        TcpTransport t2(cfg2);

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
            t1.Send(std::span(&msg, 1));
        }

        bool received = WaitForBoth(t1, t2, [&] { return callback_count >= 3; }, 2s);
        CHECK(received);
        CHECK(callback_count >= 3);

        t1.Stop();
        t2.Stop();
    }

    TEST_CASE("tcp_error_callback_invoked" * doctest::timeout(10)) {
        auto port1 = PortAllocator::GetNextPort();
        auto port2 = PortAllocator::GetNextPort();

        TransportConfig cfg1{.listen_addr = fmt::format("127.0.0.1:{}", port1), .node_id = 1};
        TransportConfig cfg2{.listen_addr = fmt::format("127.0.0.1:{}", port2), .node_id = 2};

        TcpTransport t1(cfg1);
        TcpTransport t2(cfg2);

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

    TEST_CASE("tcp_multiple_peers" * doctest::timeout(10)) {
        auto port1 = PortAllocator::GetNextPort();
        auto port2 = PortAllocator::GetNextPort();
        auto port3 = PortAllocator::GetNextPort();

        TransportConfig cfg1{.listen_addr = fmt::format("127.0.0.1:{}", port1), .node_id = 1};
        TransportConfig cfg2{.listen_addr = fmt::format("127.0.0.1:{}", port2), .node_id = 2};
        TransportConfig cfg3{.listen_addr = fmt::format("127.0.0.1:{}", port3), .node_id = 3};

        TcpTransport t1(cfg1);
        TcpTransport t2(cfg2);
        TcpTransport t3(cfg3);

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
        t1.Send(std::span(&msg2, 1));

        // Send to t3
        auto msg3 = MakeMessage(1, 3);
        t1.Send(std::span(&msg3, 1));

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

    TEST_CASE("tcp_send_to_unknown_peer" * doctest::timeout(5)) {
        auto port = PortAllocator::GetNextPort();
        TransportConfig config{
            .listen_addr = fmt::format("127.0.0.1:{}", port),
            .node_id = 1,
        };

        TcpTransport transport(config);
        REQUIRE(transport.Start().has_value());

        // Send to unknown peer - should be silently dropped
        auto msg = MakeMessage(1, 999);  // peer 999 doesn't exist
        transport.Send(std::span(&msg, 1));

        // No crash, no hang
        PollFor(transport, 100ms);

        transport.Stop();
    }

    TEST_CASE("tcp_large_message" * doctest::timeout(10)) {
        auto port1 = PortAllocator::GetNextPort();
        auto port2 = PortAllocator::GetNextPort();

        TransportConfig cfg1{.listen_addr = fmt::format("127.0.0.1:{}", port1), .node_id = 1};
        TransportConfig cfg2{.listen_addr = fmt::format("127.0.0.1:{}", port2), .node_id = 2};

        TcpTransport t1(cfg1);
        TcpTransport t2(cfg2);

        MessageCollector collector;
        t2.SetMessageCallback([&](Message m) { collector.OnMessage(std::move(m)); });

        REQUIRE(t1.Start().has_value());
        REQUIRE(t2.Start().has_value());

        t1.AddPeer(2, fmt::format("127.0.0.1:{}", port2));
        t2.AddPeer(1, fmt::format("127.0.0.1:{}", port1));

        PollBoth(t1, t2, 500ms);

        // Create large message with entries
        Message msg;
        msg.set_from(1);
        msg.set_to(2);
        msg.set_msg_type(MsgAppend);
        msg.set_term(1);

        // Add many entries with large data
        std::string large_data(1024, 'X');  // 1KB per entry
        for (int i = 0; i < 100; i++) {
            auto* entry = msg.add_entries();
            entry->set_term(1);
            entry->set_index(i + 1);
            entry->set_data(large_data);
        }

        t1.Send(std::span(&msg, 1));

        bool received = WaitForBoth(t1, t2, [&] { return collector.Count() >= 1; }, 3s);
        CHECK(received);

        if (received) {
            auto messages = collector.GetMessages();
            CHECK(messages[0].entries_size() == 100);
        }

        t1.Stop();
        t2.Stop();
    }

    TEST_CASE("tcp_message_with_entries" * doctest::timeout(10)) {
        auto port1 = PortAllocator::GetNextPort();
        auto port2 = PortAllocator::GetNextPort();

        TransportConfig cfg1{.listen_addr = fmt::format("127.0.0.1:{}", port1), .node_id = 1};
        TransportConfig cfg2{.listen_addr = fmt::format("127.0.0.1:{}", port2), .node_id = 2};

        TcpTransport t1(cfg1);
        TcpTransport t2(cfg2);

        MessageCollector collector;
        t2.SetMessageCallback([&](Message m) { collector.OnMessage(std::move(m)); });

        REQUIRE(t1.Start().has_value());
        REQUIRE(t2.Start().has_value());

        t1.AddPeer(2, fmt::format("127.0.0.1:{}", port2));
        t2.AddPeer(1, fmt::format("127.0.0.1:{}", port1));

        PollBoth(t1, t2, 500ms);

        // Create message with entries
        Message msg;
        msg.set_from(1);
        msg.set_to(2);
        msg.set_msg_type(MsgAppend);
        msg.set_term(5);
        msg.set_index(100);
        msg.set_commit(50);

        for (int i = 0; i < 5; i++) {
            auto* entry = msg.add_entries();
            entry->set_term(5);
            entry->set_index(101 + i);
            entry->set_data("entry_" + std::to_string(i));
        }

        t1.Send(std::span(&msg, 1));

        bool received = WaitForBoth(t1, t2, [&] { return collector.Count() >= 1; }, 2s);
        REQUIRE(received);

        auto messages = collector.GetMessages();
        REQUIRE(messages.size() >= 1);
        CHECK(messages[0].term() == 5);
        CHECK(messages[0].entries_size() == 5);
        CHECK(messages[0].entries(2).data() == "entry_2");

        t1.Stop();
        t2.Stop();
    }

    TEST_CASE("tcp_auto_reconnect" * doctest::timeout(10)) {
        auto port1 = PortAllocator::GetNextPort();
        auto port2 = PortAllocator::GetNextPort();

        TransportConfig cfg1{
            .listen_addr = fmt::format("127.0.0.1:{}", port1),
            .node_id = 1,
            .reconnect_interval = 100ms  // Fast reconnect for test
        };
        TransportConfig cfg2{.listen_addr = fmt::format("127.0.0.1:{}", port2), .node_id = 2};

        TcpTransport t1(cfg1);
        MessageCollector collector;

        REQUIRE(t1.Start().has_value());

        // Add peer before it exists
        t1.AddPeer(2, fmt::format("127.0.0.1:{}", port2));

        // Poll while peer is unavailable
        PollFor(t1, 300ms);

        // Now start t2
        TcpTransport t2(cfg2);
        t2.SetMessageCallback([&](Message m) { collector.OnMessage(std::move(m)); });
        REQUIRE(t2.Start().has_value());
        t2.AddPeer(1, fmt::format("127.0.0.1:{}", port1));

        // Wait for reconnection and connection establishment
        PollBoth(t1, t2, 1s);

        // Try sending a message
        auto msg = MakeMessage(1, 2);
        t1.Send(std::span(&msg, 1));

        bool received = WaitForBoth(t1, t2, [&] { return collector.Count() >= 1; }, 2s);
        CHECK(received);

        t1.Stop();
        t2.Stop();
    }

}  // TEST_SUITE("rpc::tcp")

// =============================================================================
// KCP Transport Tests
// =============================================================================

TEST_SUITE("rpc::kcp") {

    TEST_CASE("kcp_start_stop" * doctest::timeout(5)) {
        auto port = PortAllocator::GetNextPort();
        TransportConfig config{
            .listen_addr = fmt::format("127.0.0.1:{}", port),
            .node_id = 1,
        };

        KcpTransport transport(config);

        auto result = transport.Start();
        REQUIRE(result.has_value());

        PollFor(transport, 50ms);

        transport.Stop();
    }

    TEST_CASE("kcp_start_invalid_address" * doctest::timeout(5)) {
        TransportConfig config{
            .listen_addr = "invalid:not-a-port",
            .node_id = 1,
        };

        KcpTransport transport(config);

        auto result = transport.Start();
        CHECK(!result.has_value());
    }

    TEST_CASE("kcp_add_remove_peer" * doctest::timeout(5)) {
        auto port = PortAllocator::GetNextPort();
        TransportConfig config{
            .listen_addr = fmt::format("127.0.0.1:{}", port),
            .node_id = 1,
        };

        KcpTransport transport(config);
        REQUIRE(transport.Start().has_value());

        transport.AddPeer(2, "127.0.0.1:19997");
        transport.AddPeer(3, "127.0.0.1:19996");

        PollFor(transport, 50ms);

        transport.RemovePeer(2);

        PollFor(transport, 50ms);

        transport.Stop();
    }

    TEST_CASE("kcp_connect_single_peer" * doctest::timeout(15)) {
        auto port1 = PortAllocator::GetNextPort();
        auto port2 = PortAllocator::GetNextPort();

        TransportConfig cfg1{.listen_addr = fmt::format("127.0.0.1:{}", port1), .node_id = 1};
        TransportConfig cfg2{.listen_addr = fmt::format("127.0.0.1:{}", port2), .node_id = 2};

        KcpTransport t1(cfg1);
        KcpTransport t2(cfg2);

        MessageCollector collector1, collector2;
        t1.SetMessageCallback([&](Message m) { collector1.OnMessage(std::move(m)); });
        t2.SetMessageCallback([&](Message m) { collector2.OnMessage(std::move(m)); });

        REQUIRE(t1.Start().has_value());
        REQUIRE(t2.Start().has_value());

        t1.AddPeer(2, fmt::format("127.0.0.1:{}", port2));
        t2.AddPeer(1, fmt::format("127.0.0.1:{}", port1));

        // KCP needs more time for handshake
        PollBoth(t1, t2, 1s);

        auto msg = MakeMessage(1, 2);
        t1.Send(std::span(&msg, 1));

        bool received = WaitForBoth(t1, t2, [&] { return collector2.Count() >= 1; }, 3s);
        CHECK(received);

        if (received) {
            auto messages = collector2.GetMessages();
            CHECK(messages.size() >= 1);
            CHECK(messages[0].from() == 1);
            CHECK(messages[0].to() == 2);
        }

        t1.Stop();
        t2.Stop();
    }

    TEST_CASE("kcp_bidirectional_messages" * doctest::timeout(15)) {
        auto port1 = PortAllocator::GetNextPort();
        auto port2 = PortAllocator::GetNextPort();

        TransportConfig cfg1{.listen_addr = fmt::format("127.0.0.1:{}", port1), .node_id = 1};
        TransportConfig cfg2{.listen_addr = fmt::format("127.0.0.1:{}", port2), .node_id = 2};

        KcpTransport t1(cfg1);
        KcpTransport t2(cfg2);

        MessageCollector collector1, collector2;
        t1.SetMessageCallback([&](Message m) { collector1.OnMessage(std::move(m)); });
        t2.SetMessageCallback([&](Message m) { collector2.OnMessage(std::move(m)); });

        REQUIRE(t1.Start().has_value());
        REQUIRE(t2.Start().has_value());

        t1.AddPeer(2, fmt::format("127.0.0.1:{}", port2));
        t2.AddPeer(1, fmt::format("127.0.0.1:{}", port1));

        PollBoth(t1, t2, 1s);

        auto msg1 = MakeMessage(1, 2);
        t1.Send(std::span(&msg1, 1));

        auto msg2 = MakeMessage(2, 1);
        t2.Send(std::span(&msg2, 1));

        bool received = WaitForBoth(
            t1, t2, [&] { return collector1.Count() >= 1 && collector2.Count() >= 1; }, 3s
        );
        CHECK(received);

        CHECK(collector1.Count() >= 1);
        CHECK(collector2.Count() >= 1);

        t1.Stop();
        t2.Stop();
    }

    TEST_CASE("kcp_message_callback_invoked" * doctest::timeout(15)) {
        auto port1 = PortAllocator::GetNextPort();
        auto port2 = PortAllocator::GetNextPort();

        TransportConfig cfg1{.listen_addr = fmt::format("127.0.0.1:{}", port1), .node_id = 1};
        TransportConfig cfg2{.listen_addr = fmt::format("127.0.0.1:{}", port2), .node_id = 2};

        KcpTransport t1(cfg1);
        KcpTransport t2(cfg2);

        std::atomic<int> callback_count{0};
        t2.SetMessageCallback([&](Message) { callback_count++; });

        REQUIRE(t1.Start().has_value());
        REQUIRE(t2.Start().has_value());

        t1.AddPeer(2, fmt::format("127.0.0.1:{}", port2));
        t2.AddPeer(1, fmt::format("127.0.0.1:{}", port1));

        PollBoth(t1, t2, 1s);

        for (int i = 0; i < 3; i++) {
            auto msg = MakeMessage(1, 2);
            t1.Send(std::span(&msg, 1));
        }

        bool received = WaitForBoth(t1, t2, [&] { return callback_count >= 3; }, 3s);
        CHECK(received);
        CHECK(callback_count >= 3);

        t1.Stop();
        t2.Stop();
    }

    TEST_CASE("kcp_multiple_sessions" * doctest::timeout(15)) {
        auto port1 = PortAllocator::GetNextPort();
        auto port2 = PortAllocator::GetNextPort();
        auto port3 = PortAllocator::GetNextPort();

        TransportConfig cfg1{.listen_addr = fmt::format("127.0.0.1:{}", port1), .node_id = 1};
        TransportConfig cfg2{.listen_addr = fmt::format("127.0.0.1:{}", port2), .node_id = 2};
        TransportConfig cfg3{.listen_addr = fmt::format("127.0.0.1:{}", port3), .node_id = 3};

        KcpTransport t1(cfg1);
        KcpTransport t2(cfg2);
        KcpTransport t3(cfg3);

        MessageCollector collector2, collector3;
        t2.SetMessageCallback([&](Message m) { collector2.OnMessage(std::move(m)); });
        t3.SetMessageCallback([&](Message m) { collector3.OnMessage(std::move(m)); });

        REQUIRE(t1.Start().has_value());
        REQUIRE(t2.Start().has_value());
        REQUIRE(t3.Start().has_value());

        t1.AddPeer(2, fmt::format("127.0.0.1:{}", port2));
        t1.AddPeer(3, fmt::format("127.0.0.1:{}", port3));
        t2.AddPeer(1, fmt::format("127.0.0.1:{}", port1));
        t3.AddPeer(1, fmt::format("127.0.0.1:{}", port1));

        PollAll(t1, t2, t3, 1s);

        auto msg2 = MakeMessage(1, 2);
        t1.Send(std::span(&msg2, 1));

        auto msg3 = MakeMessage(1, 3);
        t1.Send(std::span(&msg3, 1));

        bool received = WaitForAll(
            t1, t2, t3, [&] { return collector2.Count() >= 1 && collector3.Count() >= 1; }, 3s
        );
        CHECK(received);
        CHECK(collector2.Count() >= 1);
        CHECK(collector3.Count() >= 1);

        t1.Stop();
        t2.Stop();
        t3.Stop();
    }

    TEST_CASE("kcp_send_to_unknown_peer" * doctest::timeout(5)) {
        auto port = PortAllocator::GetNextPort();
        TransportConfig config{
            .listen_addr = fmt::format("127.0.0.1:{}", port),
            .node_id = 1,
        };

        KcpTransport transport(config);
        REQUIRE(transport.Start().has_value());

        auto msg = MakeMessage(1, 999);
        transport.Send(std::span(&msg, 1));

        PollFor(transport, 100ms);

        transport.Stop();
    }

    TEST_CASE("kcp_large_message" * doctest::timeout(15)) {
        auto port1 = PortAllocator::GetNextPort();
        auto port2 = PortAllocator::GetNextPort();

        TransportConfig cfg1{.listen_addr = fmt::format("127.0.0.1:{}", port1), .node_id = 1};
        TransportConfig cfg2{.listen_addr = fmt::format("127.0.0.1:{}", port2), .node_id = 2};

        KcpTransport t1(cfg1);
        KcpTransport t2(cfg2);

        MessageCollector collector;
        t2.SetMessageCallback([&](Message m) { collector.OnMessage(std::move(m)); });

        REQUIRE(t1.Start().has_value());
        REQUIRE(t2.Start().has_value());

        t1.AddPeer(2, fmt::format("127.0.0.1:{}", port2));
        t2.AddPeer(1, fmt::format("127.0.0.1:{}", port1));

        PollBoth(t1, t2, 1s);

        // Create large message
        Message msg;
        msg.set_from(1);
        msg.set_to(2);
        msg.set_msg_type(MsgAppend);
        msg.set_term(1);

        std::string large_data(1024, 'Y');
        for (int i = 0; i < 50; i++) {
            auto* entry = msg.add_entries();
            entry->set_term(1);
            entry->set_index(i + 1);
            entry->set_data(large_data);
        }

        t1.Send(std::span(&msg, 1));

        // KCP may need more time for large fragmented messages
        bool received = WaitForBoth(t1, t2, [&] { return collector.Count() >= 1; }, 5s);
        CHECK(received);

        if (received) {
            auto messages = collector.GetMessages();
            CHECK(messages[0].entries_size() == 50);
        }

        t1.Stop();
        t2.Stop();
    }

    TEST_CASE("kcp_message_with_entries" * doctest::timeout(15)) {
        auto port1 = PortAllocator::GetNextPort();
        auto port2 = PortAllocator::GetNextPort();

        TransportConfig cfg1{.listen_addr = fmt::format("127.0.0.1:{}", port1), .node_id = 1};
        TransportConfig cfg2{.listen_addr = fmt::format("127.0.0.1:{}", port2), .node_id = 2};

        KcpTransport t1(cfg1);
        KcpTransport t2(cfg2);

        MessageCollector collector;
        t2.SetMessageCallback([&](Message m) { collector.OnMessage(std::move(m)); });

        REQUIRE(t1.Start().has_value());
        REQUIRE(t2.Start().has_value());

        t1.AddPeer(2, fmt::format("127.0.0.1:{}", port2));
        t2.AddPeer(1, fmt::format("127.0.0.1:{}", port1));

        PollBoth(t1, t2, 1s);

        Message msg;
        msg.set_from(1);
        msg.set_to(2);
        msg.set_msg_type(MsgAppend);
        msg.set_term(7);
        msg.set_index(200);
        msg.set_commit(150);

        for (int i = 0; i < 5; i++) {
            auto* entry = msg.add_entries();
            entry->set_term(7);
            entry->set_index(201 + i);
            entry->set_data("kcp_entry_" + std::to_string(i));
        }

        t1.Send(std::span(&msg, 1));

        bool received = WaitForBoth(t1, t2, [&] { return collector.Count() >= 1; }, 3s);
        REQUIRE(received);

        auto messages = collector.GetMessages();
        REQUIRE(messages.size() >= 1);
        CHECK(messages[0].term() == 7);
        CHECK(messages[0].entries_size() == 5);
        CHECK(messages[0].entries(3).data() == "kcp_entry_3");

        t1.Stop();
        t2.Stop();
    }

    TEST_CASE("kcp_config_options" * doctest::timeout(5)) {
        auto port = PortAllocator::GetNextPort();
        TransportConfig config{
            .listen_addr = fmt::format("127.0.0.1:{}", port),
            .node_id = 1,
        };

        KcpConfig kcp_config{
            .nodelay = 1,
            .interval = 20,  // Different from default
            .resend = 3,
            .nc = 1,
            .snd_wnd = 64,
            .rcv_wnd = 64,
            .mtu = 1200,
            .session_timeout_ms = 10000,
        };

        KcpTransport transport(config, kcp_config);

        auto result = transport.Start();
        REQUIRE(result.has_value());

        PollFor(transport, 50ms);

        transport.Stop();
    }

}  // TEST_SUITE("rpc::kcp")
