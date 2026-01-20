#pragma once

#include <memory>
#include <span>
#include <vector>

#include <capnp/any.h>
#include <capnp/message.h>
#include <capnp/serialize.h>
#include <kj/array.h>

namespace raftpp::capnp_util {

// Create an empty message with initialized root
template <typename T>
std::unique_ptr<capnp::MallocMessageBuilder> make() {
    auto builder = std::make_unique<capnp::MallocMessageBuilder>();
    builder->getRoot<T>();
    return builder;
}

// Create a message and initialize it with a lambda
template <typename T, typename Func>
std::unique_ptr<capnp::MallocMessageBuilder> make(Func&& func) {
    auto builder = std::make_unique<capnp::MallocMessageBuilder>();
    func(builder->getRoot<T>());
    return builder;
}

// Get Builder from a message
template <typename T>
typename T::Builder builder(capnp::MallocMessageBuilder& msg) {
    return msg.getRoot<T>();
}

// Get Builder from a unique_ptr to message
template <typename T>
typename T::Builder builder(const std::unique_ptr<capnp::MallocMessageBuilder>& msg) {
    return msg->getRoot<T>();
}

// Get Reader from a message
template <typename T>
typename T::Reader reader(const capnp::MallocMessageBuilder& msg) {
    return const_cast<capnp::MallocMessageBuilder&>(msg).getRoot<T>().asReader();
}

// Get Reader from a unique_ptr to message
template <typename T>
typename T::Reader reader(const std::unique_ptr<capnp::MallocMessageBuilder>& msg) {
    return msg->getRoot<T>().asReader();
}

// Clone from a Reader - efficient using setRoot
template <typename T>
std::unique_ptr<capnp::MallocMessageBuilder> clone(typename T::Reader src) {
    auto builder = std::make_unique<capnp::MallocMessageBuilder>();
    builder->setRoot(src);
    return builder;
}

// Clone from a message builder
template <typename T>
std::unique_ptr<capnp::MallocMessageBuilder> clone(const capnp::MallocMessageBuilder& src) {
    return clone<T>(const_cast<capnp::MallocMessageBuilder&>(src).getRoot<T>().asReader());
}

// Clone from a unique_ptr to message
template <typename T>
std::unique_ptr<capnp::MallocMessageBuilder> clone(
    const std::unique_ptr<capnp::MallocMessageBuilder>& src
) {
    return clone<T>(reader<T>(src));
}

// Compare two Readers for equality using AnyStruct comparison
template <typename T>
bool equal(typename T::Reader a, typename T::Reader b) {
    return capnp::AnyStruct::Reader(a) == capnp::AnyStruct::Reader(b);
}

// Serialize message to bytes
inline std::vector<uint8_t> toBytes(const capnp::MallocMessageBuilder& msg) {
    auto words = capnp::messageToFlatArray(const_cast<capnp::MallocMessageBuilder&>(msg));
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(words.begin());
    return std::vector<uint8_t>(bytes, bytes + words.size() * sizeof(capnp::word));
}

// Serialize message from unique_ptr to bytes
inline std::vector<uint8_t> toBytes(const std::unique_ptr<capnp::MallocMessageBuilder>& msg) {
    return toBytes(*msg);
}

// Parse message from bytes
template <typename T>
std::unique_ptr<capnp::MallocMessageBuilder> fromBytes(std::span<const uint8_t> bytes) {
    const capnp::word* words = reinterpret_cast<const capnp::word*>(bytes.data());
    size_t wordCount = bytes.size() / sizeof(capnp::word);
    capnp::FlatArrayMessageReader reader(kj::ArrayPtr<const capnp::word>(words, wordCount));

    auto builder = std::make_unique<capnp::MallocMessageBuilder>();
    builder->setRoot(reader.getRoot<T>());
    return builder;
}

// Serialize to word array
inline kj::Array<capnp::word> toWords(const capnp::MallocMessageBuilder& msg) {
    return capnp::messageToFlatArray(const_cast<capnp::MallocMessageBuilder&>(msg));
}

// Serialize message from unique_ptr to words
inline kj::Array<capnp::word> toWords(const std::unique_ptr<capnp::MallocMessageBuilder>& msg) {
    return toWords(*msg);
}

// Parse message from words
template <typename T>
std::unique_ptr<capnp::MallocMessageBuilder> fromWords(kj::ArrayPtr<const capnp::word> words) {
    capnp::FlatArrayMessageReader reader(words);
    auto builder = std::make_unique<capnp::MallocMessageBuilder>();
    builder->setRoot(reader.getRoot<T>());
    return builder;
}

// Serialize to string (for compatibility)
inline std::string toString(const capnp::MallocMessageBuilder& msg) {
    auto words = toWords(msg);
    const char* bytes = reinterpret_cast<const char*>(words.begin());
    size_t size = words.size() * sizeof(capnp::word);
    return std::string(bytes, size);
}

// Serialize message from unique_ptr to string
inline std::string toString(const std::unique_ptr<capnp::MallocMessageBuilder>& msg) {
    return toString(*msg);
}

// Parse message from string
template <typename T>
std::unique_ptr<capnp::MallocMessageBuilder> fromString(std::string_view str) {
    return fromBytes<T>(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(str.data()), str.size())
    );
}

template <typename T>
bool EqualMessages(
    const std::vector<std::unique_ptr<capnp::MallocMessageBuilder>>& a,
    const std::vector<std::unique_ptr<capnp::MallocMessageBuilder>>& b
) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (!capnp_util::equal<T>(capnp_util::reader<T>(a[i]), capnp_util::reader<T>(b[i]))) {
            return false;
        }
    }
    return true;
}

}  // namespace raftpp::capnp_util
