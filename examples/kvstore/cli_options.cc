#include "cli_options.h"

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

namespace kvstore::cli {

void printHelp() {
    std::cout << R"(Usage: kvstore-cli <command> [arguments]

Commands:
  put <key> <value>    Set a key-value pair
  get <key>            Get value by key
  del <key>            Delete a key
  leader               Get current leader information
  health               Get cluster health status

Options:
  --node, -n <addr>    Connect to a specific node (e.g., localhost:8080)
  --peers, -p <list>   Comma-separated list of peer addresses
  --json, -j           Output in JSON format
  --help, -h           Show this help message

Examples:
  kvstore-cli put foo bar
  kvstore-cli get foo
  kvstore-cli del foo
  kvstore-cli put foo bar --node localhost:8080
  kvstore-cli get foo --peers "localhost:8080,localhost:8081,localhost:8082"
  kvstore-cli health --json
)" << std::endl;
}

Options parseArgs(int argc, char** argv) {
    Options opts;

    if (argc < 2) {
        printHelp();
        std::exit(1);
    }

    std::string arg = argv[1];
    if (arg == "--help" || arg == "-h") {
        printHelp();
        std::exit(0);
    }

    if (arg == "put" || arg == "get" || arg == "del") {
        opts.command = arg;
        if (argc < 3) {
            std::cerr << "Error: missing key for '" << arg << "' command" << std::endl;
            printHelp();
            std::exit(1);
        }
        opts.key = argv[2];

        if (arg == "put" && argc < 4) {
            std::cerr << "Error: missing value for 'put' command" << std::endl;
            printHelp();
            std::exit(1);
        }
        if (arg == "put") {
            opts.value = argv[3];
        }
    } else if (arg == "leader" || arg == "health") {
        opts.command = arg;
    } else {
        std::cerr << "Error: unknown command '" << arg << "'" << std::endl;
        printHelp();
        std::exit(1);
    }

    int start_index = 2;
    if (opts.command == "put") {
        start_index = 4;
    } else if (opts.command == "get" || opts.command == "del") {
        start_index = 3;
    }

    for (int i = start_index; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--node" || a == "-n") {
            if (i + 1 >= argc) {
                std::cerr << "Error: missing value for --node" << std::endl;
                std::exit(1);
            }
            opts.node = argv[++i];
        } else if (a == "--peers" || a == "-p") {
            if (i + 1 >= argc) {
                std::cerr << "Error: missing value for --peers" << std::endl;
                std::exit(1);
            }
            std::string peers_str = argv[++i];
            std::stringstream ss(peers_str);
            std::string peer;
            while (std::getline(ss, peer, ',')) {
                if (!peer.empty()) {
                    opts.peers.push_back(peer);
                }
            }
        } else if (a == "--json" || a == "-j") {
            opts.json_output = true;
        } else if (a == "--help" || a == "-h") {
            printHelp();
            std::exit(0);
        } else {
            std::cerr << "Error: unknown option '" << a << "'" << std::endl;
            printHelp();
            std::exit(1);
        }
    }

    return opts;
}

}  // namespace kvstore::cli
