/**
 * evio -- A cwm4 git submodule for adding support for buffered, iostream oriented, epoll based I/O.
 *
 * @file
 * @brief Declaration of namespace evio; class MemoryBlock, MsgBlock, StreamBuf, InputBuffer, OutputBuffer and LinkBuffer.
 *
 * @Copyright (C) 2018  Carlo Wood.
 *
 * RSA-1024 0x624ACAD5 1997-01-26                    Sign & Encrypt
 * Fingerprint16 = 32 EC A7 B6 AC DB 65 A6  F6 F6 55 DD 1C DC FF 61
 *
 * This file is part of evio.
 *
 * Evio is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Evio is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with evio.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "sys.h"
#include "debug.h"
#include "StreamBuf-threads.h"
#include "utils/log2.h"
#include "utils/malloc_size.h"
#include "utils/is_power_of_two.h"
#include "utils/nearest_power_of_two.h"
#include "utils/FuzzyBool.h"
#include "utils/AIAlert.h"
#include "utils/c_escape.h"
#include "threadsafe/aithreadsafe.h"
#include <atomic>
#include <mutex>
#include <string_view>

#ifdef CWDEBUG
#include <libcwd/buf2str.h>
#endif
#ifdef DEBUGEVENTRECORDING
#include "utils/NodeMemoryPool.h"
#include <vector>
#endif
#ifdef DEBUGSTREAMBUFSTATS
#include <vector>
#endif

#if defined(CWDEBUG) && !defined(DOXYGEN)
NAMESPACE_DEBUG_CHANNELS_START
extern channel_ct io;           // IO specific debug output.
NAMESPACE_DEBUG_CHANNELS_END
#endif

namespace evio {

// Forward declarations.
class FileDescriptor;
class InputDevice;
class OutputDevice;
class MsgBlock;
class StreamBuf;

// The memory overhead of a call to malloc() in bytes.
//
// Determined during configuration. When N bytes are allocated with malloc(N) then in
// reality N + malloc_overhead_c bytes are used.
static constexpr size_t malloc_overhead_c = config::malloc_overhead_c;

//=============================================================================
//
// class MemoryBlock
//
// A reference counted memory block.
//
// The object is put at the beginning of a large(r) dynamically allocated
// memory block.
//
// A MemoryBlock* points to a single contiguous memory block allocated with
// malloc, where at the top a MemoryBlock object was created with placement new.
//
//                                             Allocated size with malloc().
//                   ___________________      /
// MemoryBlock* --> |                   |  ^  ^  ^  sizeof MemoryBlock.
//                  |   A MemoryBlock   |  |  |  |__/
//                  |                   |  |  |  v
//                  +-------------------+  |  |---
// block_start() -> |                   |  |  |  ^  m_block_size.
//                  |         ^         |  |  |  |__/
//                  |         |         |  |  |  |
//                  |     char data     |  |  |  |
//                  |         |         |  |  |  |
//                  |         v         |  |  |  |
//                  |                   |  |  |  |
//                  |___________________|  |  v  v
//                  | malloc_overhead_c |  |  \  \__ block_size
//                  |___________________|  v   \____ alloc_size
//                                          \_______ heap_size
//
// We want the data block to aligned as size_t, so sizeof(MemoryBlock) is
// a multiple of sizeof(size_t).

class MemoryBlock
{
  friend class StreamBuf;               // Needs access to create.
  friend class StreamBufProducer;       // Needs access to create.
  friend class StreamBufConsumer;       // Needs access to m_next.
  friend class MsgBlock;                // Needs access to create/add_reference/release.

 private:
  mutable std::atomic<int> m_count;     // Reference counter.
  size_t const m_block_size;            // Size of buffer area of this block in bytes.
  std::atomic<MemoryBlock*> m_next;     // The next block in the list, or nullptr if this was the last.

  MemoryBlock(size_t block_size) : m_count(1), m_block_size(block_size), m_next(nullptr) { }
  MemoryBlock(MemoryBlock const&) = delete;
  MemoryBlock& operator=(MemoryBlock const&) = delete;

  // Increment reference count by one. Called for every MsgBlock object that is created.
  void add_reference() const
  {
    m_count.fetch_add(1, std::memory_order_relaxed);
  }

 public: // Used by data_received() to make a message contiguous.
  // Create a new memory block with a reference count of 1 and a block size of block_size.
  // This initial pointer is stored in the StreamBuf::m_get_area_block_node list that this
  // MemoryBlock belongs to. The returned MemoryBlock can be viewed as a list containing a
  // single block.
  static MemoryBlock* create(size_t block_size)
  {
    // The caller is responsible to make this work by passing the value
    // utils::malloc_size(m_minimum_block_size + sizeof(evio::MemoryBlock)) - sizeof(evio::MemoryBlock)
    ASSERT(utils::is_power_of_two(sizeof(MemoryBlock) + block_size + malloc_overhead_c) ||
           (sizeof(MemoryBlock) + block_size + malloc_overhead_c) % 4096 == 0);
    // No mutex locking is required while creating a new memory block.
    MemoryBlock* memory_block = (MemoryBlock*)malloc(sizeof(MemoryBlock) + block_size);
    if (!memory_block)
      THROW_FMALERTE("Failed to allocate [BLOCK_SIZE] bytes", AIArgs("[BLOCK_SIZE]", sizeof(MemoryBlock) + block_size));
    AllocTag1(memory_block);
#ifdef DEBUGKEEPMEMORYBLOCKS
    std::memset(reinterpret_cast<char*>(memory_block + 1), 0xff, block_size);
#endif
    new (memory_block) MemoryBlock(block_size);
    return memory_block;
  }

  // Decrement reference count by one. Called when a MsgBlock is destructed and/or when
  // this MemoryBlock is removed from the StreamBuf::m_get_area_block_node list.
  void release() const
  {
    if (m_count.fetch_sub(1, std::memory_order_release) == 1)
    {
      std::atomic_thread_fence(std::memory_order_acquire);
#ifndef DEBUGKEEPMEMORYBLOCKS
      this->~MemoryBlock();
      free(const_cast<MemoryBlock*>(this));
#endif
    }
  }

 public:
  // Returns the start of the memory block for data.
  // Note that this function should not be called when it is possible that the BRT resets the get/put area of an empty buffer.
  char* block_start() const { return const_cast<char*>(reinterpret_cast<char const*>(this) + sizeof(MemoryBlock)); }

  // Returns the current size of the allocated memory block.
  size_t get_size() const { return m_block_size; }

  // See AIRefCount::unique
  utils::FuzzyBool unique() const { return std::atomic_load_explicit(&m_count, std::memory_order_relaxed) == 1 ? fuzzy::True : fuzzy::WasFalse; }

#ifdef CWDEBUG
  void print_on(std::ostream& os) const
  {
    os << "{m_count:" << m_count << ", m_block_size:" << m_block_size << ", m_next: " << m_next << "} [" << this << "]";
  }

  friend std::ostream& operator<<(std::ostream& os, MemoryBlock const& memory_block)
  {
    memory_block.print_on(os);
    return os;
  }
#endif
};

// m_block_size should be the last member in the object, so that
static_assert(alignof(MemoryBlock) == alignof(size_t) && sizeof(MemoryBlock) % sizeof(size_t) == 0, "Unexpected alignment of the data block part.");

// Subtract this from a power of two when passing a minimum block size to StreamBuf.
static constexpr size_t block_overhead_c = sizeof(MemoryBlock) + malloc_overhead_c;

//=============================================================================
//
// class MsgBlock
//
// MsgBlock is only passed as a temporary object to InputDevice::decode and as such
// a particular instance is only used by the consumer thread, which means it is
// effectively single threaded with respect to the whole MemoryBlocksBuffer.
//
class MsgBlock
{
 private:
  std::string_view m_string;
  MemoryBlock const* m_memory_block;

 public:
  // Allow to pass a non-MemoryBlock string view to a function that takes a MsgBlock.
  MsgBlock(char const* start, size_t len) : m_string(start, len), m_memory_block(nullptr) { }
  MsgBlock(std::string_view view) : m_string(view), m_memory_block(nullptr) { }

  MsgBlock(char const* start, size_t len, MemoryBlock const* memory_block) : m_string(start, len), m_memory_block(memory_block)
  {
    // The string view must point entirely inside the MemoryBlock.
    ASSERT(m_string.data() >= m_memory_block->block_start() && m_string.data() + m_string.size() <= m_memory_block->block_start() + m_memory_block->get_size());
    m_memory_block->add_reference();
  }

  ~MsgBlock() { if (m_memory_block) m_memory_block->release(); }

  MsgBlock(MsgBlock const& msg_block) : m_string(msg_block.m_string), m_memory_block(msg_block.m_memory_block)
  {
    // Do not copy a MsgBlock that is not associated with a MemoryBlock.
    ASSERT(m_memory_block);
    m_memory_block->add_reference();
  }

  MsgBlock(MsgBlock&& msg_block) : m_string(msg_block.m_string), m_memory_block(msg_block.m_memory_block)
  {
    msg_block.m_memory_block = nullptr;
  }

  MsgBlock& operator=(MsgBlock const& msg_block)
  {
    if (this == &msg_block)
      return *this;
    if (m_memory_block)
      m_memory_block->release();
    m_string = msg_block.m_string;
    m_memory_block = msg_block.m_memory_block;
    if (m_memory_block)
      m_memory_block->add_reference();
    return *this;
  }

  MsgBlock& operator=(MsgBlock&& msg_block)
  {
    if (this == &msg_block)
      return *this;
    if (m_memory_block)
      m_memory_block->release();
    m_string = msg_block.m_string;
    m_memory_block = msg_block.m_memory_block;
    msg_block.m_memory_block = nullptr;
    return *this;
  }

  char const* get_start() const { return m_string.data(); }
  char const* get_end() const { return m_string.data() + m_string.size(); }
  size_t get_size() const { return m_string.size(); }

  std::string_view const& view() const { return m_string; }

  void remove_prefix(size_t n) { m_string.remove_prefix(n); }
  void remove_suffix(size_t n) { m_string.remove_suffix(n); }
};

#ifdef DEBUGEVENTRECORDING
// Extreme Debugging Section.

enum recording_data_type
{
  memcpy_writing,
  memcpy_reading,
  put_area_reset,
  get_area_reset,
  stored_last_gptr,
  get_area_update
};

struct RecordingData
{
  recording_data_type m_type;
  size_t m_stream_offset;
  char const* m_start;
  size_t m_length;

  RecordingData(size_t stream_offset, char* start, std::streamsize length) : m_stream_offset(stream_offset), m_start(start), m_length(length) { }
  void operator delete(void* ptr) { utils::NodeMemoryPool::static_free(ptr); }
  friend std::ostream& operator<<(std::ostream& os, RecordingData const& data);
};

#endif // DEBUGEVENTRECORDING

//=============================================================================
//
// class StreamBuf
//
// A dynamic char buffer existing of a linked list of allocated memory blocks
// intended for full-duplex I/O using two cross-linked std::streambuf interfaces.
//
// This class is derived from std::streambuf and as such provides the interface
// to two buffers: one for output (ostream) and one for input (istream).
// However a single instance of this class also represents the actual output
// buffer; the ostream interface of the std::streambuf (the put area) writes
// to the buffer of this object, while the istream interface of the std::streambuf
// (the get area) reads from a different buffer (StreamBuf object).
//
// It is currently assumed that the other StreamBuf (pointed to by m_input_streambuf)
// symmetrically reads from our buffer (see the ASCII-art image below).
//
// StreamBuf is derived from (evio::)streambuf (which in turn is derived from
// std::streambuf) which hides this crosslinked interface and provides protected
// member functions for both the put area and the get area of the buffer of
// this StreamBuf.
//
// Starting with a single empty block, with a user definable minimum size,
// new blocks are allocated when needed and old blocks are freed when empty.
// The size of the newly allocated blocks depends on the current total number
// of valid bytes in the buffer.
//
// The primary goal of this buffer is to be fast: Data is never moved.
//
// This StreamBuf (derived from streambuf, derived from std::streambuf):
//
// this-> .------------------++. <---.    .---->.------------------++.
//        | !std::streambuf! |||      \  /      | !std::streambuf! |||
//        +------------------'||       \/       +------------------'||
//        | !streambuf!       ||       /\       | !streambuf!       ||
//        |                   ||      /  \      |                   ||
//        | m_input_streambuf-++-----'    `-----+-m_input_streambuf ||
//        +-------------------'|                +-------------------'|
//        | !StreamBuf!        |                | !StreamBuf!        |
//        |                    |                |                    |
//        | m_get_area---------+-----.    .-----+-m_get_area         |          *) The real names are m_get_area_block_node
//    .---+-m_put_area         |      \  /      | m_put_area---------+--.          and m_put_area_block_node.
//    |   `--------------------'       \/       `--------------------'  |
//    |                                /\                               |
//    |                               /  \                              |
//    |    .--------------------.<---'    `---->.--------------------.<-'
//    |    | !MemoryBlock!      |               | !MemoryBlock!      |
//    | .--+-m_next             |               | m_next-------------+--> nullptr
//    | |  |--------------------|               |--------------------|
//    | |  | !data!             |               | !data!             |
//    | |  |                    |               |                    |
//    | |  `--------------------'               `--------------------'
//    | |
//    | `->.--------------------.
//    |    | !MemoryBlock!      |
//    | .--+-m_next             |
//    | |  +--------------------+
//    | |  | !data!             |
//    | |  |                    |
//    | |  `--------------------'
//    | |
//    `-`->.--------------------.
//         | !MemoryBlock!      |
//         | m_next-------------+--> nullptr
//         +--------------------+
//         | !data!             |
//         |                    |
//         `--------------------'
//
// The StreamBuf class can be accessed by two threads at the same time
//  - the producer thread that writes to the buffer, and
//  - the consumer thread that reads from the buffer.
// Each type of access has its own interface. Some methods belong to the
// producer interface and others belong to the consumer interface.
// Only one thread at a time may use a specific interface, therefore methods
// that belong to one interface type may not call methods of the other type.
// In order to enforce that in a more or less robust way, the interfaces
// have been separated in two classes:
//  - class StreamBufProducer, and
//  - class StreamBufConsumer.
//
// As both classes need access to a common part including the std::streambuf
// base class, it would work to use a diamond inheritance diagram, like:
//
//                  std::streambuf
//                         |
//                 StreamBufCommon
//                   /          \.
//       StreamBufProducer  StreamBufConsumer
//                   \          /
//                    StreamBuf
//
// where StreamBufCommon is a virtual base class.
// However, I am not willing to accept the extra dereference of a virtual
// base class just to make the code a little bit more robust (separation
// of the two interfaces). Therefore we use the following design:
//
//     std::streambuf
//           |
//    StreamBufCommon
//           |
//    StreamBufProducer  StreamBufConsumer
//                \          /
//                  StreamBuf
//
// And then use the fact that only StreamBuf objects are instantiated,
// so that I can always cast a StreamBufConsumer to a StreamBuf and
// from there access the std::streambuf.

class StreamBufCommon : public std::streambuf
{
 protected:
  friend class StreamBufConsumer;
  std::atomic<char*> m_last_pptr;       // This is used to transfer the pptr to the consumer thread.
  std::atomic<char*> m_last_gptr;       // This is used to transfer the gptr of an EMPTY buffer to the producer thread.

  // The total accumulated amount of freed memory.
  // This value only ever increases.
  std::atomic<std::streamsize> m_total_freed;
  // The total accumulated amount of data that was read from this buffer.
  // This value only ever increases, it is not decreased when memory is freed.
  std::atomic<std::streamsize> m_total_read;
  // This is used to signal that the consumer thread has to reset the get area.
  std::atomic<bool> m_resetting;
  // This is set by the producer when the buffer runs full, and reset by the
  // consumer when it isn't too full anymore. Used to speed up things a little
  // (avoiding more expensive tests), but also because otherwise we can't know
  // when we are allowed to call buffer_not_full_anymore() (from need_producer_restart).
  std::atomic<bool> m_buffer_was_full;

  // Constructor.
  StreamBufCommon() :
    m_last_pptr(nullptr),
    m_last_gptr(nullptr),               // See update_put_area.
    m_total_freed(0),
    m_total_read(0),
    m_resetting(false),
    m_buffer_was_full(false)
#ifdef DEBUGEVENTRECORDING
    , recording_pool(1024, sizeof(RecordingData))
#endif
  {
  }

#ifdef DEBUGSTREAMBUFSTATS
  virtual void print_stats() const = 0;
#endif

#ifdef DEBUGEVENTRECORDING
 public:
  utils::NodeMemoryPool recording_pool;
  std::vector<RecordingData*> recording_buffer;
  std::mutex recording_mutex;
#endif

 public: // Please do not use this (for internal use only).
  [[gnu::always_inline]] char* last_pptr_consumer_read_access() const { return m_last_pptr.load(std::memory_order_acquire); }
  [[gnu::always_inline]] bool resetting_consumer_read_access() const { return m_resetting.load(std::memory_order_acquire); }

#ifdef CWDEBUG
  std::streamsize total_read() const { return m_total_read.load(std::memory_order_relaxed); }
#endif
};

//=============================================================================
//
// class StreamBufProducer
//
// This class provides the interface for the producer thread.
//

class Sink;

class StreamBufProducer : public StreamBufCommon
{
 public:
  size_t m_minimum_block_size;                  // Size of the smallest block.
  size_t m_buffer_full_watermark;               // 'buffer_full' returns true when this amount is buffered.
  size_t m_max_allocated_block_size;            // The maximum amount of allocated data size (total block size).

 protected:
  // The total accumulated amount of memory allocated for this buffer.
  // This value only ever increases, it is not decreased when memory is freed.
  std::streamsize m_total_allocated;

  // The total accumulated amount of reused memory by resetting the buffer.
  std::streamsize m_total_reset;

  // Pointer to the put area - block object.
  MemoryBlock* m_put_area_block_node;

  // The output device whose constructor this StreamBuf was passed to.
  // Writing to this variable is single threaded. The producer may read it.
  OutputDevice* m_odevice;

#ifdef DEBUGSTREAMBUFSTATS
  size_t m_number_of_created_blocks;
  std::vector<size_t> m_created_block_size;

 public:
  void reset_stats()
  {
    m_number_of_created_blocks = 0;
    m_created_block_size.clear();
  }

  void dump_stats() const;
#endif

 private:
  // Calculate the size of the new block as a function of the currently amount of buffered data.
  size_t new_block_size() const;

 protected:
  StreamBufProducer(size_t minimum_block_size, size_t buffer_full_watermark, size_t max_allocated_block_size) :
    m_minimum_block_size(minimum_block_size),
    m_buffer_full_watermark(buffer_full_watermark),
    m_max_allocated_block_size(max_allocated_block_size)
    /*m_buffer_size_minus_unused_in_last_block(0)*/
  {
  }

  ~StreamBufProducer()
  {
    // ~StreamBufConsumer should have freed all blocks (~StreamBufConsumer should be called before ~StreamBufProducer due to inheritance order).
    ASSERT(m_total_allocated == m_total_freed);
  }

  friend class Sink;
  void change_specs(size_t minimum_block_size, size_t buffer_full_watermark, size_t max_allocated_block_size)
  {
    DoutEntering(dc::io, "StreamBufProducer::change_specs(" << minimum_block_size << ", " << buffer_full_watermark << ", " << max_allocated_block_size << ") [" << this << "]");

    // These values are read by the producer thread.
    m_minimum_block_size = minimum_block_size;                  // StreamBufProducer::new_block_size, StreamBufProducer::overflow_a,
                                                                //   StreamBufProducer::xsputn_a and StreamBuf::reduce_buf.
    m_buffer_full_watermark = buffer_full_watermark;            // StreamBufProducer::buffer_full and StreamBufProducer::buffer_not_full_anymore.
    m_max_allocated_block_size = max_allocated_block_size;      // StreamBufProducer::overflow_a and StreamBufProducer::xsputn_a.
    // In the case of StreamBuf::reduce_buf the calling thread is both, the consumer thread and the producer thread.

    // Hence, this function may only be called by the producer thread,
    // in particular by the overriden decode() of protocol::Decoder.
  }

  // Note that the way m_last_pptr is updated demands that the data was already written to the buffer before pbump() or setp_pbump() is called.
  [[gnu::always_inline]] void pbump(int n)
  {
    std::streambuf::pbump(n);
    sync_egptr();
  }

  [[gnu::always_inline]] void setp(char* p, char* ep)
  {
    sync_egptr(p);
    std::streambuf::setp(p, ep);
  }
  void setp_pbump(char* p, char* ep, int n)
  {
    sync_egptr(p + n);
    std::streambuf::setp(p, ep);
    std::streambuf::pbump(n);
  }

  MemoryBlock* create_memory_block(size_t block_size);

 protected:
  int_type overflow_a(int_type c);
  std::streamsize xsputn_a(char const* s, std::streamsize const n);

 protected:
  // Called when a putback failed.
  int_type pbackfail(int_type c) override final;

#ifndef DEBUGDBSTREAMBUF
  // Allow printing of `this' pointers.
  friend std::ostream& operator<<(std::ostream& os, StreamBufProducer* sb);
#endif

 protected:
  // Returns the number of bytes that can be written directly into memory
  // at position pptr() at this moment.
  size_t available_contiguous_number_of_bytes() const { return epptr() - pptr(); }

  // Same as above, but doesn't return 0 unless out of memory or buffer full.
  size_t force_available_contiguous_number_of_bytes()
  {
    size_t contiguous_size = epptr() - pptr();
    if (contiguous_size == 0)
    {
      if (overflow_a(0) == EOF)                         // Write a dummy byte '\0'
      {
        m_buffer_was_full = true;
        Dout(dc::io, "Set m_buffer_was_full = true [this = " << this << "]");
      }
      else
      {
        pbump(-1);                                      // Erase dummy byte
        contiguous_size = epptr() - pptr();
      }
    }
    return contiguous_size;
  }

 public:
  // Called by the producer thread to indicate that there is
  // more in the buffer that can be read by the device.
  int sync() override;

  // Alternatively, this can be called, i.e. for non-streams,
  // whenever anything was written to the buffer to make
  // sure that it is written out.
  void flush();

  // Store the current value of pptr in m_last_pptr.
  [[gnu::always_inline]] void sync_egptr(char* cur_pptr)
  {
    m_last_pptr.store(cur_pptr, std::memory_order_release);     // This must be release, because this could make a reset pptr go beyond the non-reset gptr value
                                                                // making it indistinguishable from a non-reset value for the consumer thread if we don't release
                                                                // the write to m_resetting here.
#ifdef DEBUGNEXTEGPTRSANITYCHECK
    sanity_check();
#endif
  }
  [[gnu::always_inline]] void sync_egptr() { sync_egptr(pptr()); }

  char* update_put_area(std::streamsize& available);

 public:
  [[gnu::always_inline]] inline utils::FuzzyBool nothing_to_get() const;

  // Return the number of unused bytes in the put area of the output buffer.
  size_t unused_in_last_block() const { return epptr() - pptr(); }

  // Return the amount of allocated memory currently in the buffer.
  size_t get_allocated_upper_bound() const
  {
    return m_total_allocated - m_total_freed.load(std::memory_order_acquire);
  }

  // Return the number of bytes currently in the buffer.
  size_t get_data_size_upper_bound() const
  {
    // This is an upper bound because m_total_read might be growing.
    return m_total_allocated - unused_in_last_block() + m_total_reset - m_total_read.load(std::memory_order_acquire);
    //     ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^\.
    //                                                               this part was what is written in total to the buffer.
  }

 public:
  //---------------------------------------------------------------------------
  // Public accessors
  //

  // Returns true if the output buffer is full.
  bool buffer_full() const
  {
    bool full = get_data_size_upper_bound() >= m_buffer_full_watermark;
    Dout(dc::warning, "StreamBufProducer::buffer_full: get_data_size_upper_bound() = " << get_data_size_upper_bound() <<
        " >= m_buffer_full_watermark = " << m_buffer_full_watermark << " [" << this << ']');
    return full;
  }

#ifdef DEBUGEVENTRECORDING
 public:
  size_t write_stream_offset;

  void record_memcpy(RecordingData* data, char const* from);
  void resetting_put_area(RecordingData* data);
#endif
};

class StreamBufConsumer
{
 protected:
  StreamBuf* m_input_streambuf;         // Buffer that we read from.

  // Pointer to the get area - block object.
  MemoryBlock* m_get_area_block_node;

  // The input device whose constructor this StreamBuf was passed to.
  // Used by a LinkBuffer to restart the input device writing to it
  // once the output device read enough data from a previously full
  // buffer.
  InputDevice* m_idevice;

#ifdef DEBUGSTREAMBUFSTATS
  size_t m_number_of_calls_to_update_get_area;
  size_t m_number_of_get_area_resets;
  size_t m_number_of_calls_to_store_last_gptr;
  size_t m_number_of_calls_to_xsgetn_a;
  size_t m_number_of_calls_to_underflow_a;
  size_t m_xsgetn_a_read_zero_bytes;
  size_t m_xsgetn_a_read_all_requested_bytes;

 public:
  void reset_stats()
  {
    m_number_of_calls_to_update_get_area = 0;
    m_number_of_get_area_resets = 0;
    m_number_of_calls_to_store_last_gptr = 0;
    m_number_of_calls_to_xsgetn_a = 0;
    m_number_of_calls_to_underflow_a = 0;
    m_xsgetn_a_read_zero_bytes = 0;
    m_xsgetn_a_read_all_requested_bytes = 0;
  }

  void dump_stats() const;
#endif

 private:
  [[gnu::always_inline]] inline StreamBufCommon& common();
  [[gnu::always_inline]] inline StreamBufCommon const& common() const;

 protected:
  inline StreamBufConsumer();

  ~StreamBufConsumer()
  {
    while (release_memory_block(m_get_area_block_node))
      ;
  }

  // Get area / consumer thread / reading.
  [[gnu::always_inline]] inline char* eback() const;
  [[gnu::always_inline]] inline char* gptr() const;
  [[gnu::always_inline]] inline char* egptr() const;
  [[gnu::always_inline]] inline void gbump(int n);
  [[gnu::always_inline]] inline void bump_total_read(int n);
  [[gnu::always_inline]] inline void setg(char* eb, char* g, char* eg);

  [[gnu::always_inline]] void store_last_gptr(char* p)
  {
#ifdef DEBUGSTREAMBUFSTATS
    ++m_number_of_calls_to_store_last_gptr;
#endif
    common().m_last_gptr.store(p, std::memory_order_release);
#ifdef DEBUGEVENTRECORDING
    RecordingData* data = new (common().recording_pool) RecordingData(read_stream_offset, p, 0);
    data->m_type = stored_last_gptr;
    std::lock_guard<std::mutex> lock(common().recording_mutex);
    common().recording_buffer.push_back(data);
#endif
  }

  // Added _a to avoid compiler warning about hidden virtual function :/.
  std::streamsize showmanyc_a();
  std::streambuf::int_type underflow_a();
  std::streamsize xsgetn_a(char* s, std::streamsize const n);

  friend class LinkBuffer;      // Needs access to next_contiguous_number_of_bytes and force_next_contiguous_number_of_bytes.

  // Return the amount of contiguous bytes in the get area.
  // This might return 0 even if the buffer isn't empty, therefore call
  // force_next_contiguous_number_of_bytes() when it returns 0.
  size_t next_contiguous_number_of_bytes() const { return egptr() - gptr(); }

  // Returns the number of bytes that can be read directly from memory
  // from position igptr(). Do not return 0 unless everything that
  // was written before the last call to sync_egptr() has been read.
  size_t force_next_contiguous_number_of_bytes()
  {
    size_t contiguous_size;
    contiguous_size = egptr() - gptr();
    if (!contiguous_size && underflow_a() != EOF)
    {
      contiguous_size = egptr() - gptr();
#ifdef CWDEBUG
      if (!contiguous_size)
        DoutFatal(dc::core, "StreamBuf needs fixing");
#endif
    }
    return contiguous_size;
  }

  bool update_get_area(MemoryBlock*& get_area_block_node, char*& cur_gptr, std::streamsize& available);

  char* release_memory_block(MemoryBlock*& get_area_block_node);

 public: // Really ugly hack. Please do not use this (for internal use only).
  [[gnu::always_inline]] inline char* gptr_producer_read_access() const;

 public:
  [[gnu::always_inline]] inline utils::FuzzyBool nothing_to_get() const;

  // Return the number of unused bytes in the get area of the input buffer
  size_t unused_in_first_block() const { return gptr() - eback(); }

  // Return the number of bytes currently in the buffer.
  // m_buffer_size_minus_unused_in_last_block is not updated at the moment.
//  std::streamsize get_data_size_lower_bound() const { return m_buffer_size_minus_unused_in_last_block - unused_in_first_block(); }

 public:
  // Used for passing to MsgBlock constructor to increment the reference count.
  MemoryBlock* get_get_area_block_node() const { return m_get_area_block_node; }
  // Mostly for the testsuite.
  MemoryBlock*& get_get_area_block_node() { return m_get_area_block_node; }

  // Return a pointer to the first byte of the current get area memory block.
  char* get_area_block_node_start() const { return m_get_area_block_node->block_start(); }

  // Return a pointer that points one past the end of the current get area memory block.
  char* get_area_block_node_end() const { return m_get_area_block_node->block_start() + m_get_area_block_node->get_size(); }

  // Returns true if a string with length `len' is contiguous
  // in the current get area of the output buffer.
  bool is_contiguous(size_t len) const;

#ifndef DEBUGDBSTREAMBUF
  // Allow printing of `this' pointers.
  friend std::ostream& operator<<(std::ostream& os, StreamBufConsumer* sb);
#endif

#ifdef DEBUGEVENTRECORDING
 public:
  size_t read_stream_offset;

  void updating_get_area(RecordingData* data);
  void resetting_get_area(RecordingData* data);
  void record_memcpy(RecordingData* data, char* to);
#endif
};

class StreamBuf : public StreamBufProducer, public StreamBufConsumer
{
 private:
  // Override virtual functions.

  //---------------------------------------------------------------------------
  // Get area / consumer thread / reading.

  // Called to probe how much can at least be extracted from the input buffer.
  std::streamsize showmanyc() override final;

  // Called when a get area is empty while reading.
  int_type underflow() override final;

  // Called to speed up a read of `n' number of characters.
  std::streamsize xsgetn(char* s, std::streamsize n) override final;

  //---------------------------------------------------------------------------
  // Put area / producer thread / writing.

  // Called when a block is full.
  int_type overflow(int_type c) override final;

  // Called to speed up a write of `n' number of characters.
  std::streamsize xsputn(char const* s, std::streamsize n) override final;

 protected:
  //---------------------------------------------------------------------------
  // Single Threaded Protected attributes:
  //

  // Count of number of devices.
  int m_device_counter;

  //===========================================================================
  //
  // SINGLE THREADED API
  //
  // Neither put thread nor get thread may be running while this is called.
  //
 protected:
  // Called when the buffer is empty to reduce its size.
  void reduce_buffer();

  //---------------------------------------------------------------------------
  // Manipulators and accessors that are called from InputBuffer/OutputBuffer.

  // Should be called to make sure that the buffer also decreases.
  inline void reduce_buffer_if_empty();

 public:
  //---------------------------------------------------------------------------
  // Constructor
  //

  // Construct a StreamBuf object. The minimum number of allocated bytes for
  // one block of the output buffer is minimum_block_size.
  // The maximum possible number of total allocated bytes of all blocks
  // together is max_alloc. When this value is reached, overflow() will
  // return EOF.
  //
  // The method buffer_full() returns true when the number of buffered
  // bytes in the output buffer exceed buffer_full_watermark.
  //
  // After using this constructor, the input buffer is the same as the
  // output buffer. Use set_input_buffer() from the same thread that is
  // used for construction to change this.
  StreamBuf(size_t requested_minimum_block_size, size_t buffer_full_watermark, size_t max_alloc);

  // Turn a human provided minimum_block_size into the real one (more malloc friendly).
  static size_t round_up_minimum_block_size(size_t requested_minimum_block_size)
  {
    return utils::malloc_size(requested_minimum_block_size + sizeof(MemoryBlock)) - sizeof(MemoryBlock);
  }

  // Which InputDevice is pointing to me, if any?
  void set_input_device(InputDevice* device);

  // Which OutputDevice is pointing to me, if any?
  void set_output_device(OutputDevice* device);

  // Finish initialization by setting the input buffer of this StreamBuf.
#if 0
  // Initialize the input buffer pointer.
  void set_input_buffer(StreamBuf* input_buffer, SingleThread)
  {
    // This assumes that also input_buffer was just constructed and still empty.
    char* start = input_buffer->std::streambuf::pbase();
    m_input_streambuf = input_buffer;
    m_input_streambuf->std::streambuf::setg(start, start, start);
  }
#endif

  // Returns true when we can start writing again to a buffer that was full before.
  // This should be called by the consumer thread (after having read some data from
  // the buffer) while the producer is inhibited from writing to the buffer because
  // the buffer was full; therefore it is "single threaded".
  bool buffer_not_full_anymore() const
  {
    // This MUST be the consumer thread, because calling unused_in_first_block() is UB otherwise!
    // The producer may not be allocating new memory blocks while this is being called.
    return get_allocated_upper_bound() - unused_in_first_block() < m_buffer_full_watermark;
  }

  // Edge triggered event for when the buffer went from full to not-full: calls m_idevice->start_input_device() iff m_idevice != nullptr.
  [[gnu::always_inline]] void restart_input_device_if_needed()
  {
    // Relaxed because missing a beat doesn't really matter.
    if (AI_UNLIKELY(m_buffer_was_full.load(std::memory_order_relaxed)))
      do_restart_input_device_if_needed();
  }

  // Same as force_available_contiguous_number_of_bytes, but never returns 0.
  // Throws when out of memory.
  size_t force_additional_block()
  {
    // Only call this after force_available_contiguous_number_of_bytes() failed and has_multiple_blocks() returned false.
    ASSERT(m_buffer_was_full && !has_multiple_blocks());

    size_t block_size = utils::malloc_size(m_minimum_block_size + sizeof(MemoryBlock)) - sizeof(MemoryBlock);
    try
    {
      //===========================================================
      // Create a new MemoryBlock.
      MemoryBlock* new_block = create_memory_block(block_size);         // This can throw.
      char* start = new_block->block_start();
      m_put_area_block_node->m_next = new_block;
      setp(start, start + block_size);
      m_put_area_block_node = new_block;
      //===========================================================
#ifdef DEBUGDBSTREAMBUF
      printOn(std::cerr);
#endif
    }
    catch (AIAlert::ErrorCode const& error)
    {
      // Prepend our function name.
      THROW_FALERT(error);
    }

    return block_size;
  }

 protected:     // destructor
  // Should only be called by release()
  virtual ~StreamBuf() { Dout(dc::io, "~StreamBuf() [" << this << ']'); }

 public:
  // When both (or the only) associated devices call this function, then we delete ourselfs.
  bool release(FileDescriptor const* device);

 public:
  // Returns true if output buffer is empty.
  bool buffer_empty() const { return StreamBufConsumer::gptr() == pptr(); }

  // Same as get_data_size_upper_bound, but this time returning a lasting, exact value
  // because it is not possible that the consumer thread removes data from the buffer immediately
  // after returning (since we are the consumer thread too).
  size_t get_data_size() const
  {
    return m_total_allocated - unused_in_last_block() + m_total_reset - m_total_read.load(std::memory_order_relaxed);
  }

  // Correct the value of m_total_read, knowing the amount of data that should be in the buffer right now.
  void update_total_read(size_t data_size)
  {
    DoutEntering(dc::io, "update_total_read(" << data_size << ")");
#if CW_DEBUG
    std::streamsize prev_total_read = m_total_read.load(std::memory_order_relaxed);
#endif
    std::streamsize new_total_read = m_total_allocated - unused_in_last_block() + m_total_reset - data_size;
    m_total_read.store(new_total_read, std::memory_order_relaxed);
    Dout(dc::io, "m_total_read incremented by " << (new_total_read - prev_total_read));
    ASSERT(new_total_read >= prev_total_read);
  }

  // Returns `true' when this buffer currently has more then one block allocated.
  // This can be used to speed up read/write access methods.
  // The returned value only makes sense when this is both the consumer thread and the producer thread at the same time.
  bool has_multiple_blocks() const { return m_get_area_block_node != m_put_area_block_node; }

 private:
  void do_restart_input_device_if_needed();

  //===========================================================================
  // Debugging stuff.

#ifdef DEBUGDBSTREAMBUF
 protected:
  bool is_resetting() const { return m_resetting.load(std::memory_order_relaxed); }
#endif

#ifdef DEBUGKEEPMEMORYBLOCKS
 public:
  std::vector<MemoryBlock*> m_keep_v;
  void keep(MemoryBlock* mb);
  void dump();
#endif

#ifdef DEBUGNEXTEGPTRSANITYCHECK
 public:
  std::mutex get_area_release_mutex;

  void sanity_check() override
  {
    char* last_pptr = m_last_pptr.load(std::memory_order_relaxed);
    bool reachable = false;
    std::lock_guard<std::mutex> lock(get_area_release_mutex);
    MemoryBlock* volatile before_get_area_block_node = m_get_area_block_node;
    [[maybe_unused]] MemoryBlock* volatile before_next = before_get_area_block_node->m_next;
    for (MemoryBlock* block = before_get_area_block_node; block; block = block->m_next)
    {
      reachable |= (block->block_start() <= last_pptr && last_pptr <= block->block_start() + block->get_size());
    }
    [[maybe_unused]] MemoryBlock* volatile after_get_area_block_node = m_get_area_block_node;
    [[maybe_unused]] MemoryBlock* volatile after_next = before_get_area_block_node->m_next;
    if (!reachable)
    {
      for (MemoryBlock* block = m_get_area_block_node; block; block = block->m_next)
      {
        Dout(dc::notice, "Block: [" << (void*)block->block_start() << ", " << (void*)(block->block_start() + block->get_size()) << "> (size " << block->get_size() << ")");
      }
      Dout(dc::notice, "last_pptr = " << (void*)last_pptr);
    }
    ASSERT(reachable);
  }
#endif

#ifdef DEBUGDBSTREAMBUF
 public:
  // Print debug information in stream `o'.
  // *undocumented*
  void printOn(std::ostream& o) const;
#endif

#if defined(CWDEBUG) || defined(DEBUG) || defined(DEBUG_GTEST_TESTSUITE)
 public:
  bool debug_update_get_area(MemoryBlock*& get_area_block_node, char*& cur_gptr, std::streamsize& available)
  {
    return update_get_area(get_area_block_node, cur_gptr, available);
  }
#endif

#ifndef DEBUGDBSTREAMBUF
  // Allow printing of `this' pointers.
  friend std::ostream& operator<<(std::ostream& os, StreamBuf* sb);
#endif

#ifdef DEBUGSTREAMBUFSTATS
  void print_stats() const override
  {
    StreamBufProducer::dump_stats();
    StreamBufConsumer::dump_stats();
  }
#endif
};

utils::FuzzyBool StreamBufProducer::nothing_to_get() const
{
  // This is the producer thread. Therefore, if the buffer is empty it will stay empty,
  // but if it is not empty then the consumer thread might make it empty immediately
  // after leaving this function. So, if the expression is false we return WasFalse,
  // because it could become true after returning and before using the result.
  //
  // Another problem here is that reading gptr is ... well, undefined behavior.
  // The gptr is written to by the consumer thread, isn't atomic, and can't be
  // guarded with a mutex (std::streambuf is just not thread-safe, ie calling
  // std::streambuf::sbumpc would add 1 to gptr without even calling libevio).
  //
  // So, here we are relying on the hardware being "thread-safe" and allowing
  // us to read gptr as-if reading an atomic with relaxed memory order (which
  // is at least true on intel. For this it is needed that gptr lays in a single
  // cache line, but that is pretty much guaranteed too considering its normal
  // alignment). This unsafe access is reflected in the fact that we need to
  // cast 'this' to get access.
  //
  // There is nothing to get when either m_resetting is false and m_last_pptr == gptr,
  // or when m_resetting is true and m_last_pptr == pbase. Since we are the producer
  // thread, m_last_pptr and pbase are non-changing; but gptr can be changed by
  // the consumer thread (going from something to get to nothing to get, at most)
  // as well as m_resetting - when set - can go from true to false.
  //
  // Hence, if m_resetting is false it will stay false - and if in that case
  // m_last_pptr == gptr so that the buffer is empty, then it will stay empty.
  // While, when m_resetting is true then we can at our leasure determine if
  // m_last_pptr == pbase.
  return ((m_resetting.load(std::memory_order_acquire) ? pbase()
                                                       : static_cast<StreamBuf const*>(this)->gptr_producer_read_access())
      == m_last_pptr.load(std::memory_order_relaxed)) ? fuzzy::True : fuzzy::WasFalse;
}

utils::FuzzyBool StreamBufConsumer::nothing_to_get() const
{
  // This is the get thread. Therefore, if the buffer is not empty it will stay not empty,
  // but if it is empty then the put thread might (write data to it and) call sync_egptr()
  // immediately after leaving this function. So, if the expression is true we return WasTrue,
  // because it could become false after returning and before using the result.
  //
  // As above, there is nothing to get when either m_resetting is false and m_last_pptr == gptr,
  // or when m_resetting is true and m_last_pptr == eback(), where now we picked eback() because
  // m_resetting will only be true when eback() == pbase(), but is at least constant for use,
  // since we are now the consumer thread.
  //
  // This case is more complex however. If m_resetting is true it will stay true and we only
  // read one unstable atomic (last_pptr), but if m_resetting is false, which is unstable (fuzzy)
  // we still have to read last_pptr and compare that with gptr(). In the latter case we
  // have to do TWO unstable reads. We have to make sure that if that results in returning
  // fuzzy::False that it really is and will stay false.
  //
  // So, this happens when m_resetting is false (when we load it) and last_ptr != gptr at
  // the moment we read it.
  return ((static_cast<StreamBuf const*>(this)->resetting_consumer_read_access() ? eback() : gptr())
      == static_cast<StreamBuf const*>(this)->last_pptr_consumer_read_access()) ? fuzzy::WasTrue : fuzzy::False;
}

StreamBufConsumer::StreamBufConsumer() : m_input_streambuf(static_cast<StreamBuf*>(this))
{
}

char* StreamBufConsumer::eback() const
{
  return m_input_streambuf->std::streambuf::eback();
}

char* StreamBufConsumer::gptr() const
{
  return m_input_streambuf->std::streambuf::gptr();
}

char* StreamBufConsumer::gptr_producer_read_access() const
{
  return m_input_streambuf->std::streambuf::gptr();
}

char* StreamBufConsumer::egptr() const
{
  return m_input_streambuf->std::streambuf::egptr();
}

void StreamBufConsumer::gbump(int n)
{
  m_input_streambuf->std::streambuf::gbump(n);
}

// This must be called every time the streambuf was bumped.
void StreamBufConsumer::bump_total_read(int n)
{
  // Update m_total_read, avoiding an expensive RMW operation. This is safe because only the consumer thread ever updates m_total_read.
  auto new_total_read = common().m_total_read.load(std::memory_order_relaxed);
  new_total_read += n;
  common().m_total_read.store(new_total_read, std::memory_order_release);
}

void StreamBufConsumer::setg(char* eb, char* g, char* eg)
{
  m_input_streambuf->std::streambuf::setg(eb, g, eg);
}

StreamBufCommon& StreamBufConsumer::common()
{
  return static_cast<StreamBuf&>(*this);
}

StreamBufCommon const& StreamBufConsumer::common() const
{
  return static_cast<StreamBuf const&>(*this);
}

//
// Interface classes
//

// Reading from a device:

class Dev2Buf : public StreamBuf
{
 public:
  using StreamBuf::StreamBuf;

  // Writing by the device:
  size_t dev2buf_contiguous() const                             // Return the number of bytes that can be written directly
  {                                                             //  into memory at position dev2buf_ptr() at this moment.
    return available_contiguous_number_of_bytes();
  }
  size_t dev2buf_contiguous_forced()                            // Same as above, but doesn't return 0 unless
  {                                                             //  out of memory or buffer full.
    return force_available_contiguous_number_of_bytes();
  }
  char* dev2buf_ptr() const                                     // Get pointer to put area.
  {
    return pptr();
  }
  // Data must be written to the buffer *before* calling dev2buf_bump.
  void dev2buf_bump(int n)                                      // Bump pointer `n' bytes.
  {
    pbump(n);
  }

  // Copy n bytes from s to the buffer. Maybe ONLY be called by the producer thread!
  std::streamsize sputn(char const* s, std::streamsize const n)
  {
    return xsputn_a(s, n);
  }
};

// Writing to a device:

class Buf2Dev : public StreamBuf
{
 public:
  using StreamBuf::StreamBuf;

  // Reading by the device:
  size_t buf2dev_contiguous() const                             // Returns the number of bytes that are available for reading
  {                                                             // from memory from position buf2dev_ptr() right now.
    return next_contiguous_number_of_bytes();                   // Call buf2dev_contiguous_forced() if this returns 0.
  }
  size_t buf2dev_contiguous_forced()                            // Returns the number of bytes that can be read directly
  {                                                             // from memory from position buf2dev_ptr().
    return force_next_contiguous_number_of_bytes();             // Does not return 0 unless the buffer is empty.
  }
  char* buf2dev_ptr() const                                     // Get pointer to get area.
  {
    return StreamBufConsumer::gptr();
  }
  void buf2dev_bump(int n)                                      // Bump pointer `n' bytes.
  {
    StreamBufConsumer::gbump(n);
    StreamBufConsumer::bump_total_read(n);
  }
};

// Linking two devices together:

class LinkBuffer : public Dev2Buf
{
 public:
  LinkBuffer(InputDevice* input_device, OutputDevice* output_device,
      size_t minimum_block_size, size_t buffer_full_watermark, size_t max_alloc) :
    Dev2Buf(minimum_block_size, buffer_full_watermark, max_alloc)
  {
    set_input_device(input_device);
    as_Buf2Dev()->set_output_device(output_device);
  }

  //-----------------------------------------------------------
  // DUPLICATE METHODS OF Buf2Dev (maybe only be called by the consumer thread).
  size_t buf2dev_contiguous() const { return as_Buf2Dev()->next_contiguous_number_of_bytes(); }
  size_t buf2dev_contiguous_forced() { return as_Buf2Dev()->force_next_contiguous_number_of_bytes(); }
  char* buf2dev_ptr() const { return as_Buf2Dev()->StreamBufConsumer::gptr(); }
  void buf2dev_bump(int n) { as_Buf2Dev()->StreamBufConsumer::gbump(n); as_Buf2Dev()->StreamBufConsumer::bump_total_read(n); }
  //-----------------------------------------------------------

  // Because this class has the same interface as Buf2Dev, it is safe to provide these casting operators:
  operator Buf2Dev&() { return *static_cast<Buf2Dev*>(static_cast<StreamBuf*>(this)); }
  operator Buf2Dev const&() const { return *static_cast<Buf2Dev const*>(static_cast<StreamBuf const*>(this)); }

  // Also allow converting a pointer.
  Buf2Dev* as_Buf2Dev() { return static_cast<Buf2Dev*>(static_cast<StreamBuf*>(this)); }
  Buf2Dev const* as_Buf2Dev() const { return static_cast<Buf2Dev const*>(static_cast<StreamBuf const*>(this)); }
};

inline void StreamBuf::reduce_buffer_if_empty()
{
  if (buffer_empty())
    reduce_buffer();
}

// device --> buffer --> [istream]

// This class may NOT define any variables; it is merely an interface.
// A Dev2Buf can and is cast to an InputBuffer to obtain this interface, even though
// originally it wasn't created as a LinkBuffer, see InputDevice::set_link_input.
class InputBuffer : public Dev2Buf
{
 public:
  InputBuffer(InputDevice* input_device, size_t requested_minimum_block_size, size_t buffer_full_watermark, size_t max_alloc) :
    Dev2Buf(requested_minimum_block_size, buffer_full_watermark, max_alloc) { set_input_device(input_device); }

  // Stuff below reads from the input buffer and therefore may only be accessed by the consumer thread.

  // Raw binary access (instead of using istream):
  char* raw_gptr() const { return StreamBufConsumer::gptr(); }          // Get pointer to get area.
  void raw_gbump(int n) { StreamBufConsumer::gbump(n); StreamBufConsumer::bump_total_read(n); } // Bump pointer `n' bytes.
  size_t raw_sgetn(char* s, size_t n) { return xsgetn_a(s, n); }        // Read `n' bytes and copy them to `s'.

  // Administration:
  void raw_reduce_buffer_if_empty() { reduce_buffer_if_empty(); }       // Should be called to make sure that the buffer also decreases.
};

// [ostream] --> buffer --> device.

class OutputBuffer : public Buf2Dev
{
 public:
  OutputBuffer(OutputDevice* output_device, size_t minimum_block_size, size_t buffer_full_watermark, size_t max_alloc) :
    Buf2Dev(minimum_block_size, buffer_full_watermark, max_alloc) { set_output_device(output_device); }

  // Stuff below writes to the output buffer and therefore should be accessed by the producer thread.

  // Raw binary access (instead of using ostream):
  char* raw_pptr() const { return pptr(); }                             // Get pointer to put area.
  // Data must be written to the buffer *before* calling raw_pbump().
  void raw_pbump(int n) { pbump(n); }                                   // Bump pointer `n' bytes.
  size_t raw_sputn(char const* s, size_t n) { return xsputn_a(s, n); }  // Copy `n' bytes from `s' to the buffer.
};

// Returns true if a string with length `len' is contiguous
// in the current get area of the output buffer.
inline bool StreamBufConsumer::is_contiguous(size_t len) const
{
#ifdef DEBUGDBSTREAMBUF
  if (gptr() < get_area_block_node_start() || gptr() > get_area_block_node_end())
    DoutFatal(dc::core, "Ack! getpointer out of sync with get_area_block_node !");
#endif
  return gptr() + len <= get_area_block_node_end();
}

inline std::ostream& operator<<(std::ostream& os, MsgBlock const& msg_block)
{
  os << '"';
  utils::c_escape(os, msg_block.view());
  os << '"';
  return os;
}

#ifdef DEBUGDBSTREAMBUF
inline std::ostream& operator<<(std::ostream& os, StreamBuf const& db)
{
  db.printOn(os);
  return os;
}
#endif

} // namespace evio
