add_library(raftpp-proto generated/raftpp/raftpp.pb.cc)
target_include_directories(raftpp-proto PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/generated")
target_link_libraries(raftpp-proto protobuf::libprotobuf-lite)
