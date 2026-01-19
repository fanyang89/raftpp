#pragma once

#include <capnp/message.h>
#include <capnp/serialize.h>
#include <kj/array.h>

#include <memory>
#include <span>
#include <string>
#include <vector>

namespace raftpp {

// OwnedMessage wraps a Cap'n Proto message builder and provides convenient
// access to both builder and reader interfaces. This handles the complexity
// of Cap'n Proto's builder/reader pattern compared to Protobuf's mutable messages.
template <typename T>
class OwnedMessage {
  public:
    OwnedMessage() : message_(std::make_unique<capnp::MallocMessageBuilder>()) {
        message_->getRoot<T>();
    }

    // Moveable and copyable (copy performs a deep clone)
    OwnedMessage(const OwnedMessage& other) : message_(std::make_unique<capnp::MallocMessageBuilder>()) {
        message_->setRoot(other.reader());
    }
    OwnedMessage& operator=(const OwnedMessage& other) {
        if (this == &other) {
            return *this;
        }
        auto builder = std::make_unique<capnp::MallocMessageBuilder>();
        builder->setRoot(other.reader());
        message_ = std::move(builder);
        return *this;
    }
    OwnedMessage(OwnedMessage&&) noexcept = default;
    OwnedMessage& operator=(OwnedMessage&&) noexcept = default;

    // Private constructor for internal use
  private:
    explicit OwnedMessage(std::unique_ptr<capnp::MallocMessageBuilder> builder)
        : message_(std::move(builder)) {}

  public:

    // Get a builder for modifying the message
    typename T::Builder builder() {
        return message_->getRoot<T>();
    }

    // Get a reader for reading the message
    typename T::Reader reader() const {
        return message_->getRoot<T>().asReader();
    }

    // Serialize to a flat byte array
    kj::Array<capnp::word> serializeAsWords() const {
        return capnp::messageToFlatArray(*message_);
    }

    // Serialize to a byte vector
    std::vector<uint8_t> serializeAsBytes() const {
        auto words = serializeAsWords();
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(words.begin());
        size_t size = words.size() * sizeof(capnp::word);
        return std::vector<uint8_t>(bytes, bytes + size);
    }

    // Serialize to a string (compatible with old Protobuf SerializeAsString)
    std::string serializeAsString() const {
        auto words = serializeAsWords();
        const char* bytes = reinterpret_cast<const char*>(words.begin());
        size_t size = words.size() * sizeof(capnp::word);
        return std::string(bytes, size);
    }

    // Create from serialized data
    static OwnedMessage<T> parseFromWords(kj::ArrayPtr<const capnp::word> words) {
        capnp::FlatArrayMessageReader reader(words);
        auto root = reader.getRoot<T>();

        auto builder = std::make_unique<capnp::MallocMessageBuilder>();
        builder->setRoot(root);
        return OwnedMessage<T>(std::move(builder));
    }

    static OwnedMessage<T> parseFromBytes(std::span<const uint8_t> bytes) {
        // Convert bytes to words
        const capnp::word* words = reinterpret_cast<const capnp::word*>(bytes.data());
        size_t wordCount = (bytes.size() + sizeof(capnp::word) - 1) / sizeof(capnp::word);
        return parseFromWords(kj::ArrayPtr<const capnp::word>(words, wordCount));
    }

    static OwnedMessage<T> parseFromString(std::string_view str) {
        return parseFromBytes(std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(str.data()), str.size()));
    }

    // Deep copy
    OwnedMessage<T> clone() const {
        auto reader = this->reader();
        auto builder = std::make_unique<capnp::MallocMessageBuilder>();
        builder->setRoot(reader);
        return OwnedMessage<T>(std::move(builder));
    }

  private:
    std::unique_ptr<capnp::MallocMessageBuilder> message_;
};

// Helper function to create an OwnedMessage with initial values
template <typename T, typename Func>
OwnedMessage<T> makeMessage(Func&& func) {
    OwnedMessage<T> msg;
    func(msg.builder());
    return msg;
}

// Helper function to compare two Cap'n Proto messages for equality
// Note: This uses canonical comparison which may be slower but handles
// all cases correctly (including unset fields, different ordering, etc.)
template <typename T>
bool messagesEqual(typename T::Reader a, typename T::Reader b) {
    // Serialize both and compare bytes
    // This is not the most efficient but is correct and simple
    capnp::MallocMessageBuilder builderA, builderB;
    builderA.setRoot(a);
    builderB.setRoot(b);

    auto wordsA = capnp::messageToFlatArray(builderA);
    auto wordsB = capnp::messageToFlatArray(builderB);

    if (wordsA.size() != wordsB.size()) {
        return false;
    }

    return std::memcmp(wordsA.begin(), wordsB.begin(), wordsA.size() * sizeof(capnp::word))
           == 0;
}

// Helper to convert a reader to an owned message
template <typename T>
OwnedMessage<T> copyToOwned(typename T::Reader reader) {
    capnp::MallocMessageBuilder builder;
    builder.setRoot(reader);
    auto words = capnp::messageToFlatArray(builder);
    return OwnedMessage<T>::parseFromWords(words.asPtr());
}

}  // namespace raftpp
