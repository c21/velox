/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once
#include "velox/common/memory/StreamArena.h"
#include "velox/type/Type.h"

namespace facebook::velox {

struct ByteRange {
 public:
  template <typename T>
  int32_t available() {
    return (size - position) / sizeof(T);
  }

  uint8_t* buffer;
  int32_t size;
  int32_t position;
};

template <>
inline int32_t ByteRange::available<bool>() {
  return size * 8 - position;
}

// Stream over a chain of ByteRanges. Provides read, write and
// comparison for equality between stream contents and memory. Used
// for streams in repartitioning or for complex variable length data
// in hash tables.
class ByteStream {
  using Position = std::tuple<ByteRange*, int32_t>;

 public:
  // For input.
  ByteStream() : isBits_(false), isReverseBitOrder_(false) {}
  virtual ~ByteStream() = default;

  // For output.
  ByteStream(
      StreamArena* arena,
      bool isBits = false,
      bool isReverseBitOrder = false)
      : arena_(arena), isBits_(isBits), isReverseBitOrder_(isReverseBitOrder) {}

  ByteStream(const ByteStream& other) = delete;

  void operator=(const ByteStream& other) = delete;

  void resetInput(std::vector<ByteRange>&& ranges) {
    ranges_ = std::move(ranges);
    current_ = &ranges_[0];
  }

  void setRange(ByteRange range) {
    ranges_.resize(1);
    ranges_[0] = range;
    current_ = ranges_.data();
  }

  const std::vector<ByteRange>& ranges() const {
    return ranges_;
  }

  void startWrite(int32_t initialSize) {
    extend(initialSize);
  }

  void seek(int32_t range, int32_t position) {
    current_ = &ranges_[range];
    current_->position = position;
  }

  Position tellp() const {
    return std::make_tuple(current_, current_->position);
  }

  void seekp(Position position) {
    current_ = std::get<0>(position);
    current_->position = std::get<1>(position);
  }

  size_t size() const {
    size_t total = 0;
    for (auto& range : ranges_) {
      total += range.position;
    }
    return total;
  }

  // For input. Returns true if all input has been read.
  bool atEnd() const {
    if (current_->position < current_->size) {
      return false;
    }

    size_t position = current_ - ranges_.data();
    VELOX_CHECK(position >= 0 && position < ranges_.size());
    if (position == ranges_.size() - 1) {
      return true;
    }

    return false;
  }

  // Sets 'current_' to point to the next range of input.  // The
  // input is consecutive ByteRanges in 'ranges_' for the base class
  // but any view over external buffers can be made by specialization.
  virtual void next(bool throwIfPastEnd = true) {
    size_t position = current_ - &ranges_[0];
    VELOX_CHECK(position >= 0 && position < ranges_.size());
    if (position == ranges_.size() - 1) {
      if (throwIfPastEnd) {
        throw std::runtime_error("Reading past end of ByteStream");
      }
      return;
    }
    ++current_;
    current_->position = 0;
  }

  uint8_t readByte() {
    if (current_->position < current_->size) {
      return current_->buffer[current_->position++];
    }
    next();
    return readByte();
  }

  template <typename T>
  T read() {
    if (current_->position + sizeof(T) <= current_->size) {
      current_->position += sizeof(T);
      return *reinterpret_cast<const T*>(
          current_->buffer + current_->position - sizeof(T));
    }
    // The number straddles two buffers. We read byte by byte and make
    // a little-endian uint64_t. The bytes can be cast to any integer
    // or floating point type since the wire format has the machine byte order.
    static_assert(sizeof(T) <= sizeof(uint64_t));
    uint64_t value = 0;
    for (int32_t i = 0; i < sizeof(T); ++i) {
      value |= static_cast<uint64_t>(readByte()) << (i * 8);
    }
    return *reinterpret_cast<const T*>(&value);
  }

  template <typename Char>
  void readBytes(Char* data, int32_t size) {
    readBytes(reinterpret_cast<uint8_t*>(data), size);
  }

  void readBytes(uint8_t* bytes, int32_t size) {
    int32_t offset = 0;
    for (;;) {
      int32_t available = current_->size - current_->position;
      int32_t numUsed = std::min(available, size);
      memcpy(bytes + offset, current_->buffer + current_->position, numUsed);
      offset += numUsed;
      size -= numUsed;
      current_->position += numUsed;
      if (!size) {
        return;
      }
      next();
    }
  }

  // Returns a view over the read buffer for up to 'size' next
  // bytes. The size of the value may be less if the current byte
  // range ends within 'size' bytes from the current position.  The
  // size will be 0 if at end.
  std::string_view nextView(int32_t size) {
    if (current_->position == current_->size) {
      if (current_ == &ranges_.back()) {
        return std::string_view(nullptr, 0);
      }
      next();
    }
    VELOX_CHECK(current_->size);
    auto position = current_->position;
    auto viewSize = std::min(current_->size - current_->position, size);
    current_->position += viewSize;
    return std::string_view(
        reinterpret_cast<char*>(current_->buffer) + position, viewSize);
  }

  void skip(int32_t size) {
    for (;;) {
      int32_t available = current_->size - current_->position;
      int32_t numUsed = std::min(available, size);
      size -= numUsed;
      current_->position += numUsed;
      if (!size) {
        return;
      }
      next();
    }
  }

  template <typename T>
  void append(folly::Range<const T*> values) {
    if (current_->position + sizeof(T) * values.size() > current_->size) {
      appendStringPiece(folly::StringPiece(
          reinterpret_cast<const char*>(&values[0]),
          values.size() * sizeof(T)));
      return;
    }
    auto target = reinterpret_cast<T*>(current_->buffer + current_->position);
    auto end = target + values.size();
    auto valuePtr = &values[0];
    while (target != end) {
      *target = *valuePtr;
      ++target;
      ++valuePtr;
    }
    current_->position += sizeof(T) * values.size();
  }

  void appendBool(bool value, int32_t count) {
    if (count == 1 && current_->size * 8 - current_->position) {
      bits::setBit(
          reinterpret_cast<uint64_t*>(current_->buffer),
          current_->position,
          value);
      ++current_->position;
      return;
    }
    int32_t offset = 0;
    VELOX_DCHECK(isBits_);
    for (;;) {
      int32_t bitsFit =
          std::min(count - offset, current_->size * 8 - current_->position);
      bits::fillBits(
          reinterpret_cast<uint64_t*>(current_->buffer),
          current_->position,
          current_->position + bitsFit,
          value);
      current_->position += bitsFit;
      offset += bitsFit;
      if (offset == count) {
        return;
      }
      extend(bits::nbytes(count - offset));
    }
  }

  void appendStringPiece(folly::StringPiece value) {
    int32_t bytes = value.size();
    int32_t offset = 0;
    for (;;) {
      int32_t bytesFit =
          std::min(bytes - offset, current_->size - current_->position);
      memcpy(
          current_->buffer + current_->position,
          value.data() + offset,
          bytesFit);
      current_->position += bytesFit;
      offset += bytesFit;
      if (offset == bytes) {
        return;
      }
      extend(bits::roundUp(bytes - offset, memory::MappedMemory::kPageSize));
    }
  }

  template <typename T>
  void appendOne(const T& value) {
    append(folly::Range(&value, 1));
  }

  void flush(std::ostream* stream);

  // Returns the next byte that would be written to by a write. This
  // is used after an append to release the remainder of the reserved
  // space.
  char* writePosition() {
    if (ranges_.empty()) {
      return nullptr;
    }
    return reinterpret_cast<char*>(current_->buffer) + current_->position;
  }

 private:
  void extend(int32_t bytes = memory::MappedMemory::kPageSize) {
    ranges_.emplace_back();
    current_ = &ranges_.back();
    arena_->newRange(bytes, current_);
  }

  StreamArena* arena_;
  // Indicates that position in ranges_ is in bits, not bytes.
  const bool isBits_;
  const bool isReverseBitOrder_;
  // True if the bit order in ranges_ has been inverted. Presto requires reverse
  // bit order.
  bool isReversed_ = false;
  std::vector<ByteRange> ranges_;
  // Pointer to the current element of 'ranges_'.
  ByteRange* current_ = nullptr;
};

template <>
inline Timestamp ByteStream::read<Timestamp>() {
  Timestamp value;
  readBytes(reinterpret_cast<uint8_t*>(&value), sizeof(value));
  return value;
}

template <>
inline Date ByteStream::read<Date>() {
  Date value;
  readBytes(reinterpret_cast<uint8_t*>(&value), sizeof(value));
  return value;
}

} // namespace facebook::velox
