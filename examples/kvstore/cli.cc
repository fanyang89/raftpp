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
            if (res->status == 200) {
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

std::string dispatchRequest(
    const kvstore::cli::Options& opts, const std::string& method, const std::string& path,
    const std::string& body
) {
    if (!opts.node.empty()) {
        return makeRequestToNode(opts.node, method, path, body);
    } else {
        return makeRequest(method, path, body, opts.peers);
    }
}

void printResponse(
    const std::string& response, bool json_output,
    std::function<void(const nlohmann::json&)> pretty_print
) {
    auto j = nlohmann::json::parse(response);
    if (json_output) {
        std::cout << j.dump(2) << std::endl;
    } else {
        pretty_print(j);
    }
}

void printKvResponse(
    const std::string& response, bool json_output, const std::string& success_msg
) {
    auto j = nlohmann::json::parse(response);
    if (json_output) {
        std::cout << j.dump(2) << std::endl;
    } else if (j.value("success", false)) {
        std::cout << success_msg << std::endl;
    } else {
        std::cerr << "Error: " << j.value("error", "unknown error") << std::endl;
        std::exit(1);
    }
}

void printInfoResponse(
    const std::string& response, bool json_output,
    std::function<void(const nlohmann::json&)> printer
) {
    auto j = nlohmann::json::parse(response);
    if (json_output) {
        std::cout << j.dump(2) << std::endl;
    } else {
        printer(j);
    }
}

void printGetResponse(const std::string& response, bool json_output) {
    auto j = nlohmann::json::parse(response);
    if (json_output) {
        std::cout << j.dump(2) << std::endl;
    } else if (j.value("success", false)) {
        std::cout << j["value"].get<std::string>() << std::endl;
    } else {
        std::cerr << "Error: " << j.value("error", "unknown error") << std::endl;
        std::exit(1);
    }
}

}  // namespace

int main(int argc, char** argv) {
    auto opts = kvstore::cli::parseArgs(argc, argv);

    try {
        if (opts.command == "put") {
            nlohmann::json body;
            body["key"] = opts.key;
            body["value"] = opts.value;
            auto response = dispatchRequest(opts, "PUT", "/kv", body.dump());
            printKvResponse(response, opts.json_output, "OK");
        } else if (opts.command == "get") {
            std::string path = "/kv/" + opts.key;
            auto response = dispatchRequest(opts, "GET", path, "");
            printGetResponse(response, opts.json_output);
        } else if (opts.command == "del") {
            std::string path = "/kv/" + opts.key;
            auto response = dispatchRequest(opts, "DELETE", path, "");
            printKvResponse(response, opts.json_output, "OK");
        } else if (opts.command == "leader") {
            auto response = dispatchRequest(opts, "GET", "/leader", "");
            printInfoResponse(response, opts.json_output, [](const nlohmann::json& j) {
                std::cout << "Leader ID: " << j["leader_id"] << std::endl;
                std::cout << "Is Leader: " << (j["is_leader"] ? "yes" : "no") << std::endl;
            });
        } else if (opts.command == "health") {
            auto response = dispatchRequest(opts, "GET", "/health", "");
            printInfoResponse(response, opts.json_output, [](const nlohmann::json& j) {
                std::cout << "Status: " << j["status"] << std::endl;
                std::cout << "Term: " << j["term"] << std::endl;
                std::cout << "Commit Index: " << j["commit_index"] << std::endl;
                std::cout << "Applied Index: " << j["applied_index"] << std::endl;
            });
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
