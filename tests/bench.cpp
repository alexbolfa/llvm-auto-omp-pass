#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

#include <benchmark/benchmark.h>

// ---- Kernels under test ----

__attribute__((noinline))
void matrix_multiply_row(double *__restrict__ C, const double *__restrict__ A,
                         const double *__restrict__ B, int N) {
    for (int i = 0; i < N; ++i) {
        double sum = 0.0;
        for (int j = 0; j < N; ++j) {
            sum += A[i * N + j] * B[j];
        }
        C[i] = sum;
    }
}

__attribute__((noinline))
double heavy_math(double input) noexcept {
    double x = input, y = 1.0;
    if (std::fmod(x, 2.0) > 1.0) {
        x = std::sin(x) * std::sqrt(std::abs(x));
        y += std::exp(-std::abs(x));
    } else {
        x = std::cos(x) * std::pow(std::abs((x > 0 ? x : 1.0)), 0.33);
        y -= std::log1p(std::abs(std::sin(x)));
    }
    return x + y * 0.001;
}

__attribute__((noinline))
void physics_sim(double *__restrict__ out, const double *__restrict__ in, int N) {
    for (int i = 0; i < N; ++i) {
        out[i] = heavy_math(in[i]);
    }
}

__attribute__((noinline))
void image_brighten(int *__restrict__ out, const int *__restrict__ in, int N, int brightness) {
    for (int i = 0; i < N; ++i) {
        int val = in[i] + brightness;
        out[i] = val > 255 ? 255 : (val < 0 ? 0 : val);
    }
}

__attribute__((noinline)) double dot_product_bench(const double *__restrict__ a,
                                                   const double *__restrict__ b, int N) {
    double sum = 0.0;
    for (int i = 0; i < N; ++i) {
        sum += a[i] * b[i];
    }
    return sum;
}

__attribute__((noinline)) int find_max_bench(const int *data, int N) {
    int mx = data[0];
    for (int i = 1; i < N; ++i) {
        if (data[i] > mx) {
            mx = data[i];
        }
    }
    return mx;
}

// ---- Fixtures ----

class ImageBrightenBench : public benchmark::Fixture {
  public:
    static constexpr int N = 20'000'000;
    std::vector<int> in;
    std::vector<int> out;

    void SetUp(benchmark::State &) override {
        in.resize(N);
        out.resize(N);
        for (int i = 0; i < N; ++i) {
            in[i] = i % 256;
        }
    }

    void TearDown(benchmark::State &) override {
        in.clear();
        out.clear();
    }
};

class MatrixVectorBench : public benchmark::Fixture {
  public:
    static constexpr int MAT = 4000;
    std::vector<double> mat_a;
    std::vector<double> vec_b;
    std::vector<double> vec_c;

    void SetUp(benchmark::State &) override {
        mat_a.resize((size_t)MAT * MAT);
        vec_b.resize(MAT);
        vec_c.resize(MAT);
        for (int i = 0; i < MAT * MAT; ++i) {
            mat_a[i] = 0.001 * (i % 1000);
        }
        for (int i = 0; i < MAT; ++i) {
            vec_b[i] = 1.0;
        }
    }

    void TearDown(benchmark::State &) override {
        mat_a.clear();
        vec_b.clear();
        vec_c.clear();
    }
};

class PhysicsSimBench : public benchmark::Fixture {
  public:
    static constexpr int N = 20'000'000;
    std::vector<double> in;
    std::vector<double> out;

    void SetUp(benchmark::State &) override {
        in.resize(N);
        out.resize(N);
        for (int i = 0; i < N; ++i) {
            in[i] = 0.01 * i;
        }
    }

    void TearDown(benchmark::State &) override {
        in.clear();
        out.clear();
    }
};

class DotProductBench : public benchmark::Fixture {
  public:
    static constexpr int N = 20'000'000;
    std::vector<double> a;
    std::vector<double> b;

    void SetUp(benchmark::State &) override {
        a.resize(N);
        b.resize(N);
        for (int i = 0; i < N; ++i) {
            a[i] = 0.001 * (i % 1000);
            b[i] = 0.002 * ((i + 500) % 1000);
        }
    }

    void TearDown(benchmark::State &) override {
        a.clear();
        b.clear();
    }
};

class FindMaxBench : public benchmark::Fixture {
  public:
    static constexpr int N = 20'000'000;
    std::vector<int> data;

    void SetUp(benchmark::State &) override {
        data.resize(N);
        for (int i = 0; i < N; ++i) {
            data[i] = (i * 2654435761u) % N; // pseudo-random scatter
        }
    }

    void TearDown(benchmark::State &) override { data.clear(); }
};

// ---- Benchmarks ----

BENCHMARK_F(ImageBrightenBench, Run)(benchmark::State &state) {
    for (auto _ : state) {
        image_brighten(out.data(), in.data(), N, 50);
        benchmark::DoNotOptimize(out[N / 2]);
    }
}

BENCHMARK_F(MatrixVectorBench, Run)(benchmark::State &state) {
    for (auto _ : state) {
        matrix_multiply_row(vec_c.data(), mat_a.data(), vec_b.data(), MAT);
        benchmark::DoNotOptimize(vec_c[MAT / 2]);
    }
}

BENCHMARK_F(PhysicsSimBench, Run)(benchmark::State &state) {
    for (auto _ : state) {
        physics_sim(out.data(), in.data(), N);
        benchmark::DoNotOptimize(out[N / 2]);
    }
}

BENCHMARK_F(DotProductBench, Run)(benchmark::State &state) {
    for (auto _ : state) {
        benchmark::DoNotOptimize(dot_product_bench(a.data(), b.data(), N));
    }
}

BENCHMARK_F(FindMaxBench, Run)(benchmark::State &state) {
    for (auto _ : state) {
        benchmark::DoNotOptimize(find_max_bench(data.data(), N));
    }
}

BENCHMARK_MAIN();
