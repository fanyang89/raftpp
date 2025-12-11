#include "datadriven/datadriven.h"

#include <sstream>

#include <gtest/gtest.h>

using namespace raftpp::datadriven;

// 辅助函数：计算斐波那契数列
int Fibonacci(int n) {
    if (n <= 1)
        return n;
    return Fibonacci(n - 1) + Fibonacci(n - 2);
}

// 数学运算测试函数
std::string TestMathOperations(const TestData& data) {
    std::string result;

    if (data.cmd == "fibonacci") {
        for (const auto& arg : data.cmd_args) {
            if (arg.HasValue()) {
                int n = std::stoi(arg.GetValue());
                int fib = Fibonacci(n);
                result += arg.key + "=" + std::to_string(fib) + "\n";
            }
        }
    } else if (data.cmd == "add") {
        int sum = 0;
        for (const auto& arg : data.cmd_args) {
            if (arg.HasValue()) {
                sum += std::stoi(arg.GetValue());
            }
        }
        result += std::to_string(sum);
    } else if (data.cmd == "subtract") {
        auto a = data.GetValue("a");
        auto b = data.GetValue("b");
        if (a && b) {
            int result_val = std::stoi(*a) - std::stoi(*b);
            result += std::to_string(result_val);
        }
    } else if (data.cmd == "multiply") {
        int product = 1;
        for (const auto& arg : data.cmd_args) {
            if (arg.HasValue()) {
                product *= std::stoi(arg.GetValue());
            }
        }
        result += std::to_string(product);
    } else if (data.cmd == "divide") {
        auto a = data.GetValue("a");
        auto b = data.GetValue("b");
        if (a && b) {
            int result_val = std::stoi(*a) / std::stoi(*b);
            result += std::to_string(result_val);
        }
    } else if (data.cmd == "sum") {
        for (const auto& arg : data.cmd_args) {
            if (arg.HasValue()) {
                int sum = 0;
                auto values = arg.GetValues();
                for (const auto& val : values) {
                    std::stringstream ss(val);
                    int num;
                    while (ss >> num) {
                        sum += num;
                        if (ss.peek() == ',') {
                            ss.ignore();
                        }
                    }
                }
                result += arg.key + "=" + std::to_string(sum) + "\n";
            }
        }
    } else if (data.cmd == "count") {
        for (const auto& arg : data.cmd_args) {
            int count = arg.HasValue() ? 1 : 0;
            result += arg.key + "=" + std::to_string(count) + "\n";
        }
    }

    return result;
}

// 字符串操作测试函数
std::string TestStringOperations(const TestData& data) {
    std::string result;

    if (data.cmd == "concat") {
        std::string concatenated;
        for (const auto& arg : data.cmd_args) {
            if (arg.HasValue()) {
                concatenated += arg.GetValue();
            }
        }
        result = concatenated;
    }

    return result;
}

// Raft 算法测试函数
std::string TestRaftAlgorithm(const TestData& data) {
    std::string result;

    if (data.cmd == "leader_election") {
        auto nodes = data.GetValues("nodes");
        auto votes = data.GetValues("votes");
        auto term = data.GetValue("term");

        if (nodes && votes && term) {
            int vote_count = votes->size();
            int node_count = nodes->size();
            int current_term = std::stoi(*term);

            // 简单的选举逻辑：获得多数票的节点成为领导者
            bool has_majority = vote_count > (node_count / 2);

            if (has_majority) {
                result += "leader=" + (*votes)[0] + "\n";
                result += "term=" + *term + "\n";
                result += "votes=" + std::to_string(vote_count) + "\n";
            }
        }
    } else if (data.cmd == "network_partition") {
        auto partition1 = data.GetValues("partition1");
        auto partition2 = data.GetValues("partition2");
        auto current_term = data.GetValue("current_term");

        if (partition1 && partition2 && current_term) {
            result += "partition1_leader=" + (*partition1)[0] + "\n";
            result += "partition2_leader=" + (*partition2)[0] + "\n";
            result += "term_conflict=true\n";
        }
    } else if (data.cmd == "higher_term_election") {
        auto current_leader = data.GetValue("current_leader");
        auto new_term = data.GetValue("new_term");
        auto votes = data.GetValues("votes");

        if (current_leader && new_term && votes) {
            result += "new_leader=1\n";
            result += "term=" + *new_term + "\n";
            result += "old_leader_step_down=true\n";
        }
    } else if (data.cmd == "split_brain") {
        auto majority1 = data.GetValues("majority1");
        auto majority2 = data.GetValues("majority2");
        auto term = data.GetValue("term");

        if (majority1 && majority2 && term) {
            result += "split_detected=true\n";
            result += "no_majority=true\n";
            result += "requires_intervention=true\n";
        }
    } else if (data.cmd == "log_replication") {
        auto leader = data.GetValue("leader");
        auto followers = data.GetValues("followers");
        auto entries = data.GetValues("entries");
        auto term = data.GetValue("term");

        if (leader && followers && entries && term) {
            result += "success=true\n";
            result += "replicated=" + std::to_string(followers->size()) + "\n";
            result += "term=" + *term + "\n";
        }
    } else if (data.cmd == "detailed_replication") {
        auto leader = data.GetValue("leader");
        auto followers = data.GetValues("followers");
        auto entries = data.GetValues("entries");
        auto term = data.GetValue("term");

        if (leader && followers && entries && term) {
            result += "leader: " + *leader + "\n";
            result += "followers: [";
            for (size_t i = 0; i < followers->size(); ++i) {
                if (i > 0)
                    result += ", ";
                result += (*followers)[i];
            }
            result += "]\n";
            result += "entries: [";
            for (size_t i = 0; i < entries->size(); ++i) {
                if (i > 0)
                    result += ", ";
                result += (*entries)[i];
            }
            result += "]\n";
            result += "term: " + *term + "\n";
            result += "replication_status: success\n";
            result += "replicated_count: " + std::to_string(followers->size()) + "\n";
        }
    } else if (data.cmd == "partitioned_replication") {
        auto leader = data.GetValue("leader");
        auto available = data.GetValues("available_followers");
        auto unreachable = data.GetValues("unreachable_followers");

        if (leader && available && unreachable) {
            int total_followers = available->size() + unreachable->size();
            int replicated_count = available->size();
            bool quorum_achieved = replicated_count > (total_followers / 2);

            result += "success=true\n";
            result += "replicated=" + std::to_string(replicated_count) + "\n";
            result += "failed=" + std::to_string(unreachable->size()) + "\n";
            result += "quorum_achieved=" + std::string(quorum_achieved ? "true" : "false") + "\n";
            result += "partial_replication=true\n";
        }
    } else if (data.cmd == "log_conflict") {
        auto leader = data.GetValue("leader");
        auto conflicting_term = data.GetValue("conflicting_term");
        auto current_term = data.GetValue("current_term");
        auto entries = data.GetValues("entries");

        if (leader && conflicting_term && current_term && entries) {
            result += "conflict_detected=true\n";
            result += "conflict_term=" + *conflicting_term + "\n";
            result += "current_term=" + *current_term + "\n";
            result += "entries_replaced=true\n";
            result += "new_entries=[";
            for (size_t i = 0; i < entries->size(); ++i) {
                if (i > 0)
                    result += ", ";
                result += (*entries)[i];
            }
            result += "]\n";
            result += "resolution=success\n";
        }
    }

    return result;
}

TEST(DataDrivenTest, Mathoperations) {
    DataDrivenTest::RunTest("tests/testdata/math_operations", TestMathOperations);
}

TEST(DataDrivenTest, StringOperations) {
    DataDrivenTest::RunTest("tests/testdata/string_operations", TestStringOperations);
}

TEST(DataDrivenTest, Raft) {
    DataDrivenTest::RunTest("tests/testdata/raft_algorithm", TestRaftAlgorithm);
}
