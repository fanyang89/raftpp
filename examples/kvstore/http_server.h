#pragma once

#include <httplib.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

#include "raftpp/raftor/raftor.h"

namespace kvstore {

class HttpServer {
  public:
    HttpServer(raftpp::raftor::Raftor* raftor, uint16_t port);
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
    uint16_t port_;
    std::unique_ptr<httplib::Server> server_;
    std::atomic<bool> running_{false};
    std::thread server_thread_;
};

}  // namespace kvstore
