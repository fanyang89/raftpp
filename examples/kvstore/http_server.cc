#include "http_server.h"

#include <functional>
#include <iostream>

#include <nlohmann/json.hpp>

#include "kv_store_state_machine.h"

namespace kvstore {

HttpServer::HttpServer(raftpp::raftor::Raftor* raftor, uint16_t port)
    : raftor_(raftor), port_(port) {}

HttpServer::~HttpServer() {
    Stop();
}

void HttpServer::Start() {
    server_ = std::make_unique<httplib::Server>();
    setupRoutes();

    running_ = true;
    std::function<void()> server_loop = [this]() {
        server_->listen("0.0.0.0", port_);
    };
    server_thread_ = std::thread(server_loop);

    std::cout << "HTTP server listening on port " << port_ << std::endl;
}

void HttpServer::Stop() {
    if (running_.load()) {
        running_ = false;
        if (server_) {
            server_->stop();
        }
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
    }
}

void HttpServer::setupRoutes() {
    server_->Put("/kv", [this](const httplib::Request& req, httplib::Response& res) {
        handlePut(req, res);
    });

    server_->Get("/kv/(.*)", [this](const httplib::Request& req, httplib::Response& res) {
        handleGet(req, res);
    });

    server_->Delete("/kv/(.*)", [this](const httplib::Request& req, httplib::Response& res) {
        handleDelete(req, res);
    });

    server_->Get("/leader", [this](const httplib::Request& req, httplib::Response& res) {
        handleLeader(req, res);
    });

    server_->Get("/health", [this](const httplib::Request& req, httplib::Response& res) {
        handleHealth(req, res);
    });
}

void HttpServer::handlePut(const httplib::Request& req, httplib::Response& res) {
    try {
        auto body = nlohmann::json::parse(req.body);
        std::string key = body.at("key").get<std::string>();
        std::string value = body.at("value").get<std::string>();

        nlohmann::json cmd;
        cmd["op"] = "put";
        cmd["key"] = key;
        cmd["value"] = value;

        sendCommand(cmd.dump(), res);
    } catch (const std::exception& e) {
        res.status = 500;
        nlohmann::json err;
        err["success"] = false;
        err["error"] = e.what();
        res.set_content(err.dump(), "application/json");
    }
}

void HttpServer::handleGet(const httplib::Request& req, httplib::Response& res) {
    try {
        auto key = req.path_params.at("1");

        nlohmann::json cmd;
        cmd["op"] = "get";
        cmd["key"] = key;

        sendCommand(cmd.dump(), res);
    } catch (const std::exception& e) {
        res.status = 500;
        nlohmann::json err;
        err["success"] = false;
        err["error"] = e.what();
        res.set_content(err.dump(), "application/json");
    }
}

void HttpServer::handleDelete(const httplib::Request& req, httplib::Response& res) {
    try {
        auto key = req.path_params.at("1");

        nlohmann::json cmd;
        cmd["op"] = "del";
        cmd["key"] = key;

        sendCommand(cmd.dump(), res);
    } catch (const std::exception& e) {
        res.status = 500;
        nlohmann::json err;
        err["success"] = false;
        err["error"] = e.what();
        res.set_content(err.dump(), "application/json");
    }
}

void HttpServer::handleLeader(const httplib::Request& req, httplib::Response& res) {
    (void)req;
    auto status = raftor_->GetStatus();
    nlohmann::json json_resp;
    json_resp["leader_id"] = status.leader_id;
    json_resp["is_leader"] = (status.id == status.leader_id);
    res.set_content(json_resp.dump(), "application/json");
}

void HttpServer::handleHealth(const httplib::Request& req, httplib::Response& res) {
    (void)req;
    auto status = raftor_->GetStatus();
    nlohmann::json json_resp;
    json_resp["status"] = "healthy";
    json_resp["term"] = status.term;
    json_resp["commit_index"] = status.commit_index;
    json_resp["applied_index"] = status.applied_index;
    res.set_content(json_resp.dump(), "application/json");
}

void HttpServer::sendCommand(const std::string& cmd_json, httplib::Response& res) {
    auto future = raftor_->ProposeAsync(cmd_json);
    auto result = future.get();

    if (result.has_value()) {
        res.set_content(result.value(), "application/json");
    } else {
        res.status = 500;
        nlohmann::json err;
        err["success"] = false;
        err["error"] = result.error().ToString();
        res.set_content(err.dump(), "application/json");
    }
}

}  // namespace kvstore
