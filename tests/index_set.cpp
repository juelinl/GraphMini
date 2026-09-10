#include "backend/index_set.h"
#include "backend/bitmap_tasks.h"
#include <numeric>
#include <oneapi/tbb/global_control.h>
#include <type_traits>
#include <vector>

using namespace minigraph;
using Pool = internal::VertexSetPool;
void require(bool ok) {
  if (!ok)
    throw std::runtime_error("IndexSet regression");
}
static_assert(!std::is_copy_constructible_v<IndexSet> &&
              !std::is_copy_assignable_v<IndexSet>);
static_assert(std::is_nothrow_move_constructible_v<IndexSet> &&
              std::is_nothrow_move_assignable_v<IndexSet>);
static_assert(std::is_same_v<decltype(std::declval<const IndexSet &>().data()),
                             const uint32_t *>);
static_assert(sizeof(void *) != 8 || sizeof(IndexSet) == 24);

int main() {
  Pool::configure_for_graph(127);
  auto &pool = Pool::for_request(8);
  const auto initial = pool.checked_out();
  IndexSet empty;
  IndexSet zero(0);
  require(empty.data() == nullptr && empty.size() == 0 &&
          empty.capacity() == 0);
  require(zero.data() == nullptr && pool.checked_out() == initial);
  bool rejected = false;
  try {
    empty.set_size(1);
  } catch (const std::length_error &) {
    rejected = true;
  }
  require(rejected);
  uint32_t *reusable = nullptr;
  {
    IndexSet a(8), b(8);
    require(a.capacity() == pool.capacity() && a.size() == 0);
    require(pool.checked_out() == initial + 2 && a.data() != b.data());
    for (size_t i = 0; i < a.capacity(); ++i)
      a.data()[i] = static_cast<uint32_t>(i + 123);
    a.set_size(8);
    rejected = false;
    try {
      a.set_size(a.capacity() + 1);
    } catch (const std::length_error &) {
      rejected = true;
    }
    require(rejected && a.size() == 8);
    reusable = a.data();
    IndexSet moved(std::move(a));
    require(a.data() == nullptr && a.size() == 0 && a.capacity() == 0);
    require(moved.data() == reusable && moved.size() == 8);
    b = std::move(moved);
    require(pool.checked_out() == initial + 1 && moved.data() == nullptr);
    auto &same = b;
    b = std::move(same);
    require(b.data() == reusable && b.size() == 8);
  }
  require(pool.checked_out() == initial);
  const auto allocated = Pool::TOTAL_ALLOCATED.load();
  for (int repeat = 0; repeat < 100; ++repeat) {
    IndexSet reused(8);
    require(reused.data() == reusable && reused.size() == 0);
    // Only read values initialized above; returning/reacquiring never clears.
    require(reused.data()[0] == 123 && reused.data()[127] == 250);
  }
  require(Pool::TOTAL_ALLOCATED.load() == allocated);
  try {
    IndexSet lease(8);
    throw std::runtime_error("unwind");
  } catch (const std::runtime_error &) {
  }
  require(pool.checked_out() == initial);

  // Larger graphs do not resize or invalidate outstanding smaller leases.
  {
    IndexSet small(8);
    small.data()[0] = 42;
    Pool::configure_for_graph(2047);
    IndexSet large(2000);
    require(large.capacity() >= 2048 && small.capacity() == 128 &&
            small.data()[0] == 42);
  }
  require(pool.checked_out() == initial);

  // Nested decode buffers stay checked out until child reductions finish.
  // Repeat at 1/4 workers to exercise both same-worker reentrancy and
  // borrowing.
  for (int threads : {1, 4}) {
    tbb::global_control workers(tbb::global_control::max_allowed_parallelism,
                                threads);
    std::vector<uint32_t> ids(129);
    std::iota(ids.begin(), ids.end(), 0);
    NeighborhoodUniverse universe(999, ids);
    Bitmap full(universe, true), lazy(universe);
    lazy.assign_bounded<0, false>(full, 100);
    for (auto mode :
         {BitmapIteration::DecodedScalar, BitmapIteration::DecodedAVX2}) {
      BitmapTaskPolicy policy{8, 99, true, false, mode};
      auto &origin = Pool::for_request(129);
      const auto outstanding = origin.checked_out();
      auto actual = bitmap_for_each(
          lazy, true,
          [&](auto cursor, bool) -> uint64_t {
            uint64_t count = 0;
            for (; cursor.valid(); cursor.advance()) {
              const auto parent = cursor.position();
              Bitmap child(universe);
              child.assign_bounded<0, false>(lazy, parent);
              count += bitmap_for_each(
                  child, true,
                  [&](auto nested, bool) -> uint64_t {
                    uint64_t result = 0;
                    for (; nested.valid(); nested.advance()) {
                      require(nested.position() < parent);
                      ++result;
                    }
                    return result;
                  },
                  policy, 1);
              require(cursor.position() ==
                      parent); // Child leases did not overwrite the parent.
            }
            return count;
          },
          policy);
      require(actual == 100 * 99 / 2 && origin.checked_out() == outstanding);
      rejected = false;
      try {
        bitmap_for_each(
            lazy, true,
            [&](auto, bool) -> uint64_t {
              return bitmap_for_each(
                  lazy, true,
                  [](auto, bool) -> uint64_t {
                    throw std::runtime_error("cancel nested tasks");
                  },
                  policy, 1);
            },
            policy);
      } catch (const std::runtime_error &) {
        rejected = true;
      }
      require(rejected && origin.checked_out() == outstanding);
    }
  }
}
