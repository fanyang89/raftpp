# Generate data files by [bin2cpp](https://github.com/end2endzone/bin2cpp)

add_library(raftpp-test-data-conf-change
  conf_change/generated/joint_autoleave.cpp
  conf_change/generated/joint_idempotency.cpp
  conf_change/generated/joint_learners_next.cpp
  conf_change/generated/joint_safety.cpp
  conf_change/generated/simple_idempotency.cpp
  conf_change/generated/simple_promote_demote.cpp
  conf_change/generated/simple_safety.cpp
  conf_change/generated/zero.cpp
)
target_include_directories(raftpp-test-data-conf-change PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/conf_change/generated")

add_library(raftpp-test-data-quorum
  quorum/generated/joint_commit.cpp
  quorum/generated/joint_group_commit.cpp
  quorum/generated/joint_vote.cpp
  quorum/generated/majority_commit.cpp
  quorum/generated/majority_vote.cpp
)
target_include_directories(raftpp-test-data-quorum PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/quorum/generated")
