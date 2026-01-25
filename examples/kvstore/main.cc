#include <cstdlib>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "http_server.h"
#include "kv_store_state_machine.h"
#include "raftpp/raftor/raftor.h"
#include "raftpp/raftor/raftor_config.h"

namespace {

void printHelp() {
    std::cout << "Usage: kvstore-example [OPTIONS]" << std::endl;
    std::cout << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  --node-id <id>       Node ID (default: 1)" << std::endl;
    std::cout << "  --port <port>        HTTP listen port (default: 8080)" << std::endl;
    std::cout << "  --raft-port <port>   Raft transport port (default: 9000)" << std::endl;
    std::cout << "  --peers <list>       Comma-separated peer list including THIS node, e.g., "
                 "\"1:localhost:9000,2:localhost:9001,3:localhost:9002\""
              << std::endl;
    std::cout
        << "                       (required for first startup, ignored if WAL already exists)"
        << std::endl;
    std::cout << "  --data-dir <dir>     Data directory for WAL and snapshots (default: ./kv_data)"
              << std::endl;
    std::cout << "  --help               Show this help message" << std::endl;
}

struct Options {
    uint64_t node_id = 1;
    uint16_t port = 8080;
    uint16_t raft_port = 9000;
    std::string peers;
    std::string data_dir = "./kv_data";
};

Options parseArgs(int argc, char** argv) {
    Options opts;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--help") {
            printHelp();
            std::exit(0);
        } else if (arg == "--node-id" && i + 1 < argc) {
            try {
                opts.node_id = std::stoull(argv[++i]);
            } catch (const std::exception& e) {
                std::cerr << "Error: invalid node-id: " << e.what() << std::endl;
                std::exit(1);
            }
        } else if (arg == "--port" && i + 1 < argc) {
            try {
                opts.port = static_cast<uint16_t>(std::stoi(argv[++i]));
            } catch (const std::exception& e) {
                std::cerr << "Error: invalid port: " << e.what() << std::endl;
                std::exit(1);
            }
        } else if (arg == "--raft-port" && i + 1 < argc) {
            try {
                opts.raft_port = static_cast<uint16_t>(std::stoi(argv[++i]));
            } catch (const std::exception& e) {
                std::cerr << "Error: invalid raft-port: " << e.what() << std::endl;
                std::exit(1);
            }
        } else if (arg == "--peers" && i + 1 < argc) {
            opts.peers = argv[++i];
        } else if (arg == "--data-dir" && i + 1 < argc) {
            opts.data_dir = argv[++i];
        }
    }
    return opts;
}

std::vector<raftpp::raftor::PeerConfig> parsePeers(const std::string& peers_str) {
    std::vector<raftpp::raftor::PeerConfig> peers;
    if (peers_str.empty()) {
        return peers;
    }

    size_t start = 0;
    while (start < peers_str.size()) {
        size_t comma = peers_str.find(',', start);
        std::string peer = peers_str.substr(start, comma - start);
        size_t colon = peer.find(':');
        if (colon != std::string::npos) {
            try {
                raftpp::raftor::PeerConfig config;
                config.id = std::stoull(peer.substr(0, colon));
                config.addr = peer.substr(colon + 1);
                peers.push_back(config);
            } catch (const std::exception& e) {
                std::cerr << "Warning: skipping invalid peer '" << peer << "': " << e.what()
                          << std::endl;
            }
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return peers;
}

}  // namespace

int main(int argc, char** argv) {
    auto opts = parseArgs(argc, argv);

    raftpp::raftor::RaftorConfig config;
    config.node_id = opts.node_id;
    config.listen_addr = "0.0.0.0:" + std::to_string(opts.raft_port);
    config.data_dir = opts.data_dir;
    config.initial_peers = parsePeers(opts.peers);

    auto validation = config.Validate();
    if (!validation.has_value()) {
        std::cerr << "Configuration error: " << validation.error().ToString() << std::endl;
        return 1;
    }

    auto state_machine = std::make_unique<kvstore::KvStoreStateMachine>();
    auto* kv_store = state_machine.get();
    auto raftor_result = raftpp::raftor::Raftor::Create(config, std::move(state_machine));

    if (!raftor_result.has_value()) {
        std::cerr << "Failed to create Raftor: " << raftor_result.error().ToString() << std::endl;
        return 1;
    }

    auto raftor = std::move(*raftor_result);

    auto start_result = raftor->Start();
    if (!start_result.has_value()) {
        std::cerr << "Failed to start Raftor: " << start_result.error().ToString() << std::endl;
        return 1;
    }

    kvstore::HttpServer http_server(raftor.get(), kv_store, opts.port);
    http_server.Start();

    std::cout << "KV Store started. Node ID: " << opts.node_id << ", HTTP Port: " << opts.port
              << ", Raft Port: " << opts.raft_port << std::endl;

    raftor->Run();

    http_server.Stop();
    raftor->Stop();

    return 0;
}
