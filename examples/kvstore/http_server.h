#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace httplib {
class Server;
struct Request;
struct Response;
}  // namespace httplib

namespace kvstore {
class IKVStore;
}

namespace raftpp::raftor {
class Raftor;
}

namespace kvstore {

class HttpServer {
  public:
    HttpServer(raftpp::raftor::Raftor* raftor, IKVStore* kv_store, uint16_t port);
    ~HttpServer();

    void Start();
    void Stop();

    bool IsRunning() const { return running_.load(); }

  private:
    void setupRoutes();

    void handlePut(const httplib::Request& req, httplib::Response& res);
    void handleGet(const httplib::Request& req, httplib::Response& res);
    void handleDelete(const httplib::Request& req, httplib::Response& res);
    void handleLeader(const httplib::Request& req, httplib::Response& res);
    void handleHealth(const httplib::Request& req, httplib::Response& res);

    void sendCommand(const std::string& cmd_json, httplib::Response& res);

    raftpp::raftor::Raftor* raftor_;
    IKVStore* kv_store_;
    uint16_t port_;
    std::unique_ptr<httplib::Server> server_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> read_counter_{0};
    std::thread server_thread_;
};

}  // namespace kvstore
