#pragma once

#include <memory>
#include <span>
#include <string>
#include <vector>

#include <capnp/any.h>
#include <capnp/message.h>
#include <capnp/serialize.h>
#include <kj/array.h>
#include <kj/exception.h>

#include "raftpp/core/error.h"

namespace raftpp {

// OwnedMessage wraps a Cap'n Proto message builder and provides convenient
// access to both builder and reader interfaces. This handles the complexity
// of Cap'n Proto's builder/reader pattern compared to Protobuf's mutable messages.
template <typename T>
class OwnedMessage {
  public:
    OwnedMessage() = default;

    // Construct from a reader (deep copy)
    explicit OwnedMessage(T::Reader reader) {
        ensureBuilder();
        message_->setRoot(reader);
    }

    // Moveable and copyable (copy performs a deep clone)
    OwnedMessage(const OwnedMessage& other) {
        ensureBuilder();
        message_->setRoot(other.reader());
    }

    OwnedMessage& operator=(const OwnedMessage& other) {
        if (this == &other) {
            return *this;
        }
        ensureBuilder();
        message_->setRoot(other.reader());
        return *this;
    }

    OwnedMessage(OwnedMessage&&) noexcept = default;
    OwnedMessage& operator=(OwnedMessage&&) noexcept = default;

    // Get a builder for modifying the message
    T::Builder builder() {
        ensureBuilder();
        return message_->getRoot<T>();
    }

    // Get a reader for reading the message
    T::Reader reader() const {
        ensureBuilder();
        return message_->getRoot<T>().asReader();
    }

    // Serialize to a flat byte array.
    kj::Array<capnp::word> serializeAsWords() const {
        ensureBuilder();
        return capnp::messageToFlatArray(*message_);
    }

    // Serialize to a byte vector
    std::vector<uint8_t> serializeAsBytes() const {
        ensureBuilder();
        const auto r = capnp::messageToFlatArray(*message_);
        return {r.asBytes().begin(), r.asBytes().end()};
    }

    // Serialize to a string (compatible with old Protobuf SerializeAsString)
    std::string serializeAsString() const {
        ensureBuilder();
        const auto r = capnp::messageToFlatArray(*message_);
        return {r.asBytes().begin(), r.asBytes().end()};
    }

    // Create from serialized data
    static OwnedMessage parseFromWords(kj::ArrayPtr<const capnp::word> words) {
        capnp::FlatArrayMessageReader reader(words);
        auto root = reader.getRoot<T>();

        OwnedMessage message;
        message.ensureBuilder();
        message.message_->setRoot(root);
        return message;
    }

    static Result<OwnedMessage> parseFromWordsResult(kj::ArrayPtr<const capnp::word> words) {
        if (words.size() == 0) {
            return CapnpError{"empty message buffer"}.ToError();
        }
        try {
            capnp::FlatArrayMessageReader reader(words);
            auto root = reader.getRoot<T>();
            (void)root.totalSize();

            OwnedMessage message;
            message.ensureBuilder();
            message.message_->setRoot(root);
            return message;
        } catch (const kj::Exception& ex) {
            return CapnpError{std::string(ex.getDescription().cStr())}.ToError();
        }
    }

    static OwnedMessage parseFromBytes(std::span<const uint8_t> bytes) {
        kj::ArrayPtr<const capnp::word> wordsPtr;
        auto owned = alignBytesToWords(bytes, wordsPtr);
        (void)owned;  // Keep alive until parseFromWords completes
        return parseFromWords(wordsPtr);
    }

    static Result<OwnedMessage> parseFromBytesResult(std::span<const uint8_t> bytes) {
        if (bytes.empty()) {
            return CapnpError{"empty message buffer"}.ToError();
        }
        kj::ArrayPtr<const capnp::word> wordsPtr;
        auto owned = alignBytesToWords(bytes, wordsPtr);
        (void)owned;  // Keep alive until parseFromWordsResult completes
        return parseFromWordsResult(wordsPtr);
    }

    static OwnedMessage parseFromString(std::string_view str) {
        return parseFromBytes(std::span(reinterpret_cast<const uint8_t*>(str.data()), str.size()));
    }

    // Deep copy
    OwnedMessage clone() const { return OwnedMessage(reader()); }

  private:
    void ensureBuilder() const {
        if (!message_) {
            message_ = std::make_unique<capnp::MallocMessageBuilder>();
        }
    }

    // Convert bytes to word-aligned array. Returns empty array if bytes are
    // already aligned and can be used directly via the out parameter.
    static kj::Array<capnp::word> alignBytesToWords(
        std::span<const uint8_t> bytes, kj::ArrayPtr<const capnp::word>& out
    ) {
        constexpr size_t kWordSize = sizeof(capnp::word);
        const auto addr = reinterpret_cast<uintptr_t>(bytes.data());
        const bool aligned = addr % alignof(capnp::word) == 0;
        const bool wholeWords = bytes.size() % kWordSize == 0;

        if (aligned && wholeWords) {
            const auto* words = reinterpret_cast<const capnp::word*>(bytes.data());
            out = kj::ArrayPtr(words, bytes.size() / kWordSize);
            return {};
        }

        const size_t wordCount = (bytes.size() + kWordSize - 1) / kWordSize;
        auto owned = kj::heapArray<capnp::word>(wordCount);
        std::memset(owned.begin(), 0, wordCount * kWordSize);
        if (!bytes.empty()) {
            std::memcpy(owned.begin(), bytes.data(), bytes.size());
        }
        out = owned.asPtr();
        return owned;
    }

    mutable std::unique_ptr<capnp::MallocMessageBuilder> message_;
};

// Helper function to create an OwnedMessage with initial values
template <typename T, typename Func>
OwnedMessage<T> makeMessage(Func&& func) {
    OwnedMessage<T> msg;
    func(msg.builder());
    return msg;
}

// Helper function to compare two Cap'n Proto messages for equality.
template <typename T>
bool messagesEqual(typename T::Reader a, typename T::Reader b) {
    capnp::AnyStruct::Reader left = a;
    capnp::AnyStruct::Reader right = b;
    return left == right;
}

// Helper to convert a reader to an owned message
template <typename T>
OwnedMessage<T> copyToOwned(typename T::Reader reader) {
    return OwnedMessage<T>(reader);
}

}  // namespace raftpp
