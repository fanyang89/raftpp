add_library(raftpp-proto generated/raftpp/core/raftpp.pb.cc)
target_include_directories(raftpp-proto
                           PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/generated")
target_link_libraries(raftpp-proto protobuf::libprotobuf)

get_target_property(PROTOBUF_INCLUDE_DIRS protobuf::libprotobuf
                    INTERFACE_INCLUDE_DIRECTORIES)

target_include_directories(raftpp-proto SYSTEM PRIVATE ${PROTOBUF_INCLUDE_DIRS})
