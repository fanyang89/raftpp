#include <httplib.h>

#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cli_options.h"

namespace {

std::string normalizeAddr(const std::string& addr) {
    if (addr.find("://") == std::string::npos) {
        return "http://" + addr;
    }
    return addr;
}

std::string makeRequest(
    const std::string& method, const std::string& path, const std::string& body,
    const std::vector<std::string>& peers
) {
    std::string last_error;

    for (const auto& peer : peers) {
        auto addr = normalizeAddr(peer);
        auto cli = httplib::Client(addr);
        cli.set_connection_timeout(3);
        cli.set_read_timeout(3);

        httplib::Result res;
        if (method == "GET") {
            res = cli.Get(path.c_str());
        } else if (method == "PUT") {
            res = cli.Put(path.c_str(), body, "application/json");
        } else if (method == "DELETE") {
            res = cli.Delete(path.c_str());
        } else {
            continue;
        }

        if (res) {
            if (res->status == 200 || res->status == 404) {
                return res->body;
            }
            last_error = "HTTP " + std::to_string(res->status);
        } else {
            last_error = "connection failed";
        }
    }

    if (last_error.empty()) {
        last_error = "no peers available";
    }
    throw std::runtime_error("Request failed: " + last_error);
}

std::string makeRequestToNode(
    const std::string& node, const std::string& method, const std::string& path,
    const std::string& body
) {
    auto addr = normalizeAddr(node);
    auto cli = httplib::Client(addr);
    cli.set_connection_timeout(3);
    cli.set_read_timeout(3);

    httplib::Result res;
    if (method == "GET") {
        res = cli.Get(path.c_str());
    } else if (method == "PUT") {
        res = cli.Put(path.c_str(), body, "application/json");
    } else if (method == "DELETE") {
        res = cli.Delete(path.c_str());
    } else {
        throw std::runtime_error("unknown method: " + method);
    }

    if (!res) {
        throw std::runtime_error("connection failed to " + node);
    }
    if (res->status >= 400) {
        throw std::runtime_error("HTTP error: " + std::to_string(res->status));
    }
    return res->body;
}

}  // namespace

int main(int argc, char** argv) {
    auto opts = kvstore::cli::parseArgs(argc, argv);

    std::string response;

    try {
        if (opts.command == "put") {
            nlohmann::json body;
            body["key"] = opts.key;
            body["value"] = opts.value;

            if (!opts.node.empty()) {
                response = makeRequestToNode(opts.node, "PUT", "/kv", body.dump());
            } else {
                response = makeRequest("PUT", "/kv", body.dump(), opts.peers);
            }

            auto j = nlohmann::json::parse(response);
            if (opts.json_output) {
                std::cout << j.dump(2) << std::endl;
            } else if (j["success"] == true) {
                std::cout << "OK" << std::endl;
            } else {
                std::cerr << "Error: " << j["error"] << std::endl;
                return 1;
            }
        } else if (opts.command == "get") {
            std::string path = "/kv/" + opts.key;

            if (!opts.node.empty()) {
                response = makeRequestToNode(opts.node, "GET", path, "");
            } else {
                response = makeRequest("GET", path, "", opts.peers);
            }

            auto j = nlohmann::json::parse(response);
            if (opts.json_output) {
                std::cout << j.dump(2) << std::endl;
            } else if (j["success"] == true) {
                std::cout << j["value"] << std::endl;
            } else {
                std::cerr << "Error: " << j["error"] << std::endl;
                return 1;
            }
        } else if (opts.command == "del") {
            std::string path = "/kv/" + opts.key;

            if (!opts.node.empty()) {
                response = makeRequestToNode(opts.node, "DELETE", path, "");
            } else {
                response = makeRequest("DELETE", path, "", opts.peers);
            }

            auto j = nlohmann::json::parse(response);
            if (opts.json_output) {
                std::cout << j.dump(2) << std::endl;
            } else if (j["success"] == true) {
                std::cout << "OK" << std::endl;
            } else {
                std::cerr << "Error: " << j["error"] << std::endl;
                return 1;
            }
        } else if (opts.command == "leader") {
            if (!opts.node.empty()) {
                response = makeRequestToNode(opts.node, "GET", "/leader", "");
            } else {
                response = makeRequest("GET", "/leader", "", opts.peers);
            }

            auto j = nlohmann::json::parse(response);
            if (opts.json_output) {
                std::cout << j.dump(2) << std::endl;
            } else {
                std::cout << "Leader ID: " << j["leader_id"] << std::endl;
                std::cout << "Is Leader: " << (j["is_leader"] ? "yes" : "no") << std::endl;
            }
        } else if (opts.command == "health") {
            if (!opts.node.empty()) {
                response = makeRequestToNode(opts.node, "GET", "/health", "");
            } else {
                response = makeRequest("GET", "/health", "", opts.peers);
            }

            auto j = nlohmann::json::parse(response);
            if (opts.json_output) {
                std::cout << j.dump(2) << std::endl;
            } else {
                std::cout << "Status: " << j["status"] << std::endl;
                std::cout << "Term: " << j["term"] << std::endl;
                std::cout << "Commit Index: " << j["commit_index"] << std::endl;
                std::cout << "Applied Index: " << j["applied_index"] << std::endl;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
