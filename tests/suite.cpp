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

// ---- SIMD reductions ----

TEST_F(LoopTest, DotProduct) {
    auto a = rand_doubles(N);
    auto b = rand_doubles(N);
    double expected = 0.0;
    for (int i = 0; i < N; ++i)
        expected += a[i] * b[i];
    double actual = dot_product(a.data(), b.data(), N);
    EXPECT_NEAR(actual, expected, std::abs(expected) * 1e-10);
}

TEST_F(LoopTest, FpProduct) {
    // Use small values near 1.0 to avoid overflow/underflow.
    auto data = rand_doubles(N, 0.999, 1.001);
    double expected = 1.0;
    for (int i = 0; i < N; ++i)
        expected *= data[i];
    double actual = fp_product(data.data(), N);
    EXPECT_NEAR(actual, expected, std::abs(expected) * 1e-6);
}

TEST_F(LoopTest, FindMax) {
    auto data = rand_ints(N);
    int expected = *std::max_element(data.begin(), data.end());
    EXPECT_EQ(find_max(data.data(), N), expected);
}

TEST_F(LoopTest, FindMin) {
    auto data = rand_ints(N);
    int expected = *std::min_element(data.begin(), data.end());
    EXPECT_EQ(find_min(data.data(), N), expected);
}

TEST_F(LoopTest, BitwiseOrReduce) {
    auto data = rand_ints(N, 0, 255);
    int expected = 0;
    for (int i = 0; i < N; ++i)
        expected |= data[i];
    EXPECT_EQ(bitwise_or_reduce(data.data(), N), expected);
}

// ---- Collapse ----

TEST_F(LoopTest, ScaleMatrix) {
    constexpr int M = 1000, K = 1000;
    auto data = rand_ints(M * K);
    auto ref = data;
    for (int i = 0; i < M * K; ++i) {
        ref[i] *= 7;
    }
    scale_matrix(data.data(), M, K, 7);
    EXPECT_EQ(data, ref);
}

TEST_F(LoopTest, FillMatrix) {
    constexpr int M = 1000, K = 1000;
    std::vector<int> data(M * K, -1);
    fill_matrix(data.data(), M, K);
    for (int i = 0; i < M; ++i) {
        for (int j = 0; j < K; ++j) {
            ASSERT_EQ(data[i * K + j], i * 1000 + j) << "at (" << i << "," << j << ")";
        }
    }
}

TEST_F(LoopTest, AddMatrices) {
    constexpr int M = 1000, K = 1000;
    auto A = rand_ints(M * K);
    auto B = rand_ints(M * K);
    std::vector<int> C(M * K);
    std::vector<int> ref(M * K);
    for (int i = 0; i < M * K; ++i) {
        ref[i] = A[i] + B[i];
    }
    add_matrices(C.data(), A.data(), B.data(), M, K);
    EXPECT_EQ(C, ref);
}

// ---- Promotion (outer loop parallelized) ----

TEST_F(LoopTest, MatrixVector) {
    constexpr int K = 500;
    auto A = rand_doubles(K * K);
    auto B = rand_doubles(K);
    std::vector<double> C(K), ref(K);
    for (int i = 0; i < K; ++i) {
        double sum = 0.0;
        for (int j = 0; j < K; ++j) {
            sum += A[i * K + j] * B[j];
        }
        ref[i] = sum;
    }
    matrix_vector(C.data(), A.data(), B.data(), K);
    for (int i = 0; i < K; ++i) {
        EXPECT_NEAR(C[i], ref[i], std::abs(ref[i]) * 1e-10) << "row " << i;
    }
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
