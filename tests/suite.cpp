#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "functions/parallel_loops.cpp"
#include "functions/not_parallel_loops.cpp"

namespace {
template <typename T> uint64_t compute_hash(const T *data, size_t n) {
    uint64_t hash = 14695981039346656037ULL; // FNV offset basis
    const uint8_t *byte_ptr = reinterpret_cast<const uint8_t *>(data);
    size_t size_bytes = n * sizeof(T);

    for (size_t i = 0; i < size_bytes; ++i) {
        hash ^= byte_ptr[i];
        hash *= 1099511628211ULL; // FNV prime
    }
    return hash;
}

constexpr int N = 1'000'000;

class LoopTest : public ::testing::Test {
  protected:
    std::mt19937 rng{42};

    std::vector<int> rand_ints(int n, int lo = -1000, int hi = 1000) {
        std::uniform_int_distribution<int> dist(lo, hi);
        std::vector<int> v(n);
        for (auto &x : v) {
            x = dist(rng);
        }
        return v;
    }

    std::vector<double> rand_doubles(int n, double lo = -100.0, double hi = 100.0) {
        std::uniform_real_distribution<double> dist(lo, hi);
        std::vector<double> v(n);
        for (auto &x : v) {
            x = dist(rng);
        }
        return v;
    }

    void verify_hash(uint64_t actual, uint64_t expected, const std::string &label) {
        if (actual != expected) {
            std::cout << "[ HASH MISMATCH ] " << label << " -> Actual: " << actual << "ULL"
                      << std::endl;
        }
        ASSERT_EQ(actual, expected);
    }
};

} // namespace

// ---- Parallel loops ----

TEST_F(LoopTest, EasySum) {
    auto data = rand_ints(N);
    EXPECT_EQ(easy_sum(data.data(), N), 692617);
}

TEST_F(LoopTest, ArgmemVarying) {
    std::vector<int> data(N);
    argmem_varying(data.data(), N);
    verify_hash(compute_hash(data.data(), N), 15887178784585699462ULL, "ArgmemVarying");
}

TEST_F(LoopTest, InterleavedRW) {
    constexpr int H = 500'000;
    std::vector<int> a(2 * H);
    for (int i = 0; i < 2 * H; ++i) {
        a[i] = i;
    }
    interleaved_rw(a.data(), H);
    verify_hash(compute_hash(a.data(), 2 * H), 8788400565043371036ULL, "InterleavedRW");
}

TEST_F(LoopTest, SplitArray) {
    constexpr int H = 500'000;
    std::vector<int> a(2 * H);
    for (int i = 0; i < 2 * H; ++i) {
        a[i] = i;
    }
    split_array(a.data(), H);
    verify_hash(compute_hash(a.data(), 2 * H), 12842703590451613480ULL, "SplitArray");
}

TEST_F(LoopTest, GlobalIndexedWrite) {
    auto data = rand_ints(N);
    global_indexed_write(data.data(), N);
    verify_hash(compute_hash(g_output, N), 8024147421901249633ULL, "GlobalIndexedWrite");
}

// ---- Not-parallel loops (must produce correct sequential results) ----

TEST_F(LoopTest, InterDependentReductions) {
    auto data = rand_ints(N);
    int a = 0, b = 0;
    for (int i = 0; i < N; i++) {
        a += b;
        b += data[i];
    }
    EXPECT_EQ(store_with_reduction_variables(data.data(), N), a * b);
}

TEST_F(LoopTest, PrefixSum) {
    auto data = rand_ints(N);
    auto reference = data;
    for (int i = 1; i < N; i++) {
        reference[i] += reference[i - 1];
    }
    prefix_sum(data.data(), N);
    EXPECT_EQ(data, reference);
}

TEST_F(LoopTest, ReadonlyCallRace) {
    auto arr = rand_ints(N);
    auto ref = arr;
    int expected = 0;
    for (int i = 0; i < N; i++) {
        ref[i] = i * 2;
        expected += ref[N - 1 - i];
    }
    int actual = readonly_call_race(arr.data(), N);
    EXPECT_EQ(actual, expected);
}

TEST_F(LoopTest, LastIterationValue) {
    auto data = rand_ints(N);
    int expected = data[N - 1] * 2;
    EXPECT_EQ(last_iteration_value(data.data(), N), expected);
}
