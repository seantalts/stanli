// Serial stand-ins for the three TBB facilities Stan's multi-path Pathfinder
// service uses, so that service compiles unmodified in a library that does
// not link TBB.
//
// stan/services/pathfinder/multi.hpp runs its paths in tbb::parallel_for,
// gathers them in a tbb::concurrent_vector, and writes through
// stan::callbacks::concurrent_writer, which owns a
// tbb::concurrent_bounded_queue. All three need the TBB runtime library;
// libstanli provides only the two symbols Stan Math's autodiff needs. The
// paths could not run concurrently here anyway: they share one model, and an
// Executor is one evaluation's mutable state.
//
// Include this FIRST in a translation unit. It defines the include guards of
// the three real headers, so they are skipped when Stan reaches for them,
// and supplies the names in their place. Everything else in TBB (ranges,
// partitioners, the arena and observer types Stan Math declares) stays the
// real thing. Reaching a real header first is a compile error below, and a
// TBB whose guards are named differently fails on redefinition, so this
// cannot go wrong quietly.
#ifndef STANLI_TBB_SERIAL_HPP
#define STANLI_TBB_SERIAL_HPP

#if defined(__TBB_parallel_for_H) || defined(__TBB_concurrent_vector_H) || \
    defined(__TBB_concurrent_queue_H)
#error "tbb_serial.hpp must come before every other header"
#endif
#define __TBB_parallel_for_H
#define __TBB_concurrent_vector_H
#define __TBB_concurrent_queue_H

#include <tbb/blocked_range.h>
#include <tbb/global_control.h>
#include <tbb/partitioner.h>

#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace tbb {

// Every grain of the range in order, on the calling thread: the range is
// split until it will not divide, as TBB splits it when it has workers to
// feed. One chunk for the whole range would also be a legal schedule, and is
// what TBB does with a single thread, but Stan's multi-path loop leaves its
// chunk on the first path that fails, so the chunking decides whether the
// paths after a failed one run at all. One path per chunk is the reading its
// own "Only m of the n pathfinders succeeded" message describes.
namespace stanli_serial {
template <typename Range, typename Body>
void each_grain(Range& range, const Body& body) {
  if (range.empty()) return;
  if (!range.is_divisible()) {
    body(range);
    return;
  }
  Range upper(range, split());
  each_grain(range, body);
  each_grain(upper, body);
}
}  // namespace stanli_serial

template <typename Range, typename Body>
void parallel_for(const Range& range, const Body& body) {
  Range all(range);
  stanli_serial::each_grain(all, body);
}

template <typename Range, typename Body, typename Partitioner>
void parallel_for(const Range& range, const Body& body,
                  const Partitioner& /*partitioner*/) {
  Range all(range);
  stanli_serial::each_grain(all, body);
}

// Grown from one thread only, so a std::vector is the same container.
template <typename T, typename Allocator = std::allocator<T>>
class concurrent_vector : public std::vector<T, Allocator> {
 public:
  using std::vector<T, Allocator>::vector;
};

// concurrent_writer pushes from the running thread and pops from a writer
// thread of its own, so this one is shared and guarded.
template <typename T>
class concurrent_bounded_queue {
 public:
  using size_type = std::ptrdiff_t;

  void set_capacity(size_type capacity) {
    std::lock_guard<std::mutex> lock(mutex_);
    capacity_ = capacity;
  }
  bool empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return items_.empty();
  }
  size_type size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return (size_type)items_.size();
  }
  template <typename U>
  bool try_push(U&& item) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (capacity_ >= 0 && (size_type)items_.size() >= capacity_) return false;
    items_.emplace_back(std::forward<U>(item));
    return true;
  }
  bool try_pop(T& item) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (items_.empty()) return false;
    item = std::move(items_.front());
    items_.pop_front();
    return true;
  }

 private:
  mutable std::mutex mutex_;
  std::deque<T> items_;
  size_type capacity_ = -1;
};

}  // namespace tbb

#endif
