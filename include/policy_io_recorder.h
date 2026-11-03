// policy_io_recorder.h: records the network input and output on a separate thread without delaying the control loop.
//
// C++17 standard library only. Single header, include as is.
//
// Rules:
//  * push() is called by ONE thread (the policy loop). It never waits: no locks, no memory allocation,
//    no disk writes, no system calls other than reading the clock.
//  * The buffer is a lock-free ring (single producer, single consumer). When it is full the record is lost,
//    the drop counter increases, push() returns false and the loop carries on.
//  * A dedicated thread writes to disk. A write failure does not affect the loop: the thread counts the
//    errors and discards the data.
//  * Disabled until a file is open (open() not called): push() returns false immediately.
//
// File format policy_io.bin (little-endian), reader: read_policy_io.py.
//   header: "PIO1", u32 version (=1), u32 obs_dim, u32 act_dim, f64 wall_start (unix seconds),
//           u32 layout length, observation layout text ("key:size,...")
//   record: u64 seq (push call number; gaps in seq = lost records), u64 dropped (records lost so far),
//           i32 frame, i64 t_ns (monotonic time since open), char clip[64], f32 obs[obs_dim], f32 act[act_dim]
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace pio {

class PolicyIoRecorder {
 public:
  static constexpr size_t kClipBytes = 64;
  static constexpr size_t kFixedBytes = 8 + 8 + 4 + 8 + kClipBytes;

  PolicyIoRecorder() = default;
  PolicyIoRecorder(const PolicyIoRecorder&) = delete;
  PolicyIoRecorder& operator=(const PolicyIoRecorder&) = delete;
  ~PolicyIoRecorder() { close(); }

  // Opens <dir>/policy_io.bin and starts the writer thread. false: the file could not be opened (recording stays off).
  bool open(const std::string& dir, const std::string& obs_layout, uint32_t obs_dim, uint32_t act_dim,
            size_t capacity = 4096) {
    if (file_ != nullptr || capacity == 0) return false;
    const std::string path = dir + "/policy_io.bin";
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) return false;
    std::setvbuf(f, nullptr, _IOFBF, 1 << 20);
    const double wall = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    const uint32_t version = 1, layout_len = static_cast<uint32_t>(obs_layout.size());
    bool ok = std::fwrite("PIO1", 1, 4, f) == 4;
    ok = ok && std::fwrite(&version, 4, 1, f) == 1 && std::fwrite(&obs_dim, 4, 1, f) == 1 &&
         std::fwrite(&act_dim, 4, 1, f) == 1 && std::fwrite(&wall, 8, 1, f) == 1 &&
         std::fwrite(&layout_len, 4, 1, f) == 1 &&
         (layout_len == 0 || std::fwrite(obs_layout.data(), 1, layout_len, f) == layout_len);
    if (!ok) {
      std::fclose(f);
      return false;
    }
    obs_dim_ = obs_dim;
    act_dim_ = act_dim;
    slot_bytes_ = kFixedBytes + 4 * (static_cast<size_t>(obs_dim) + act_dim);
    capacity_ = capacity;
    buf_.assign(slot_bytes_ * capacity_, 0);
    head_.store(0);
    tail_.store(0);
    seq_ = 0;
    dropped_.store(0);
    written_.store(0);
    write_errors_.store(0);
    origin_ = std::chrono::steady_clock::now();
    file_ = f;
    stop_.store(false);
    open_.store(true, std::memory_order_release);
    worker_ = std::thread([this] { run_writer(); });
    return true;
  }

  // Called from the control loop. Never waits. true: the record was accepted by the buffer.
  bool push(int32_t frame, const char* clip, const float* obs, const float* act) {
    if (!open_.load(std::memory_order_acquire)) return false;
    const uint64_t h = head_.load(std::memory_order_relaxed);
    const uint64_t t = tail_.load(std::memory_order_acquire);
    const uint64_t seq = seq_++;
    if (h - t >= capacity_) {
      dropped_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    uint8_t* p = buf_.data() + (h % capacity_) * slot_bytes_;
    const uint64_t dropped = dropped_.load(std::memory_order_relaxed);
    const int64_t t_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now() - origin_).count();
    std::memcpy(p, &seq, 8);
    std::memcpy(p + 8, &dropped, 8);
    std::memcpy(p + 16, &frame, 4);
    std::memcpy(p + 20, &t_ns, 8);
    char* c = reinterpret_cast<char*>(p + 28);
    size_t i = 0;
    for (; clip != nullptr && clip[i] != '\0' && i < kClipBytes - 1; ++i) c[i] = clip[i];
    for (; i < kClipBytes; ++i) c[i] = '\0';
    std::memcpy(p + kFixedBytes, obs, 4 * static_cast<size_t>(obs_dim_));
    std::memcpy(p + kFixedBytes + 4 * static_cast<size_t>(obs_dim_), act, 4 * static_cast<size_t>(act_dim_));
    head_.store(h + 1, std::memory_order_release);
    return true;
  }

  // Flushes the remainder, stops the thread and closes the file. Do not call from the control loop.
  void close() {
    if (!open_.exchange(false)) return;
    stop_.store(true, std::memory_order_release);
    if (worker_.joinable()) worker_.join();
    if (file_ != nullptr) {
      std::fflush(file_);
      std::fclose(file_);
      file_ = nullptr;
    }
  }

  uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
  uint64_t written() const { return written_.load(std::memory_order_relaxed); }
  uint64_t write_errors() const { return write_errors_.load(std::memory_order_relaxed); }

 private:
  void run_writer() {
    auto last_flush = std::chrono::steady_clock::now();
    bool failed = false;
    for (;;) {
      const bool stopping = stop_.load(std::memory_order_acquire);
      const uint64_t h = head_.load(std::memory_order_acquire);
      uint64_t t = tail_.load(std::memory_order_relaxed);
      if (t == h) {
        if (stopping) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
      while (t != h) {
        if (!failed) {
          const uint8_t* p = buf_.data() + (t % capacity_) * slot_bytes_;
          if (std::fwrite(p, 1, slot_bytes_, file_) == slot_bytes_) {
            written_.fetch_add(1, std::memory_order_relaxed);
          } else {
            failed = true;
            write_errors_.fetch_add(1, std::memory_order_relaxed);
          }
        }
        ++t;
        tail_.store(t, std::memory_order_release);
      }
      const auto now = std::chrono::steady_clock::now();
      if (!failed && now - last_flush > std::chrono::milliseconds(500)) {
        std::fflush(file_);
        last_flush = now;
      }
    }
  }

  std::FILE* file_ = nullptr;
  std::vector<uint8_t> buf_;
  size_t slot_bytes_ = 0, capacity_ = 0;
  uint32_t obs_dim_ = 0, act_dim_ = 0;
  std::chrono::steady_clock::time_point origin_{};
  alignas(64) std::atomic<uint64_t> head_{0};
  alignas(64) std::atomic<uint64_t> tail_{0};
  alignas(64) uint64_t seq_ = 0;              // touched by the producer only
  std::atomic<uint64_t> dropped_{0}, written_{0}, write_errors_{0};
  std::atomic<bool> open_{false}, stop_{false};
  std::thread worker_;
};

// Controller wrapper: the network input arrives as a "name -> vector" map, the output as a vector.
// Input keys are taken in alphabetical order (the order is stored in the file layout). Enabled only if
// start() received a non-empty directory: that is the only switch. The first tap() call opens the file and
// allocates memory; there are no allocations afterwards. Call tap() from a single thread.
class PolicyIoTap {
 public:
  bool start(const char* dir) {
    if (dir == nullptr || dir[0] == '\0' || enabled_) return false;
    dir_ = dir;
    enabled_ = true;
    return true;
  }

  template <class Map>
  void tap(int32_t frame, const char* clip, const Map& obs, const std::vector<float>& act) {
    if (!enabled_) return;
    if (!opened_ && !first_call(obs, act)) return;
    size_t at = 0;
    for (size_t i = 0; i < keys_.size(); ++i) {
      auto it = obs.find(keys_[i]);
      if (it == obs.end() || it->second.size() != sizes_[i] || act.size() != act_dim_) {
        bad_shape_.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      std::memcpy(flat_.data() + at, it->second.data(), 4 * sizes_[i]);
      at += sizes_[i];
    }
    rec_.push(frame, clip, flat_.data(), act.data());
  }

  void stop() {
    rec_.close();
    enabled_ = false;
  }

  uint64_t bad_shape() const { return bad_shape_.load(std::memory_order_relaxed); }
  const PolicyIoRecorder& recorder() const { return rec_; }

 private:
  template <class Map>
  bool first_call(const Map& obs, const std::vector<float>& act) {
    for (const auto& kv : obs) keys_.push_back(kv.first);
    std::sort(keys_.begin(), keys_.end());
    std::string layout;
    size_t total = 0;
    for (const auto& k : keys_) {
      const size_t n = obs.find(k)->second.size();
      sizes_.push_back(n);
      total += n;
      layout += (layout.empty() ? "" : ",") + k + ":" + std::to_string(n);
    }
    act_dim_ = act.size();
    flat_.assign(total, 0.f);
    opened_ = true;
    if (!rec_.open(dir_, layout, static_cast<uint32_t>(total), static_cast<uint32_t>(act_dim_))) {
      enabled_ = false;                      // directory unavailable: recording is turned off, the loop is unaffected
      return false;
    }
    return true;
  }

  PolicyIoRecorder rec_;
  std::string dir_;
  std::vector<std::string> keys_;
  std::vector<size_t> sizes_;
  std::vector<float> flat_;
  size_t act_dim_ = 0;
  bool enabled_ = false, opened_ = false;
  std::atomic<uint64_t> bad_shape_{0};
};

}  // namespace pio
