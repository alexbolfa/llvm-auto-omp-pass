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
void black_scholes(double *__restrict__ call, double *__restrict__ put,
                   const double *__restrict__ S, const double *__restrict__ K,
                   const double *__restrict__ T, double r, double sigma, int N) {
    for (int i = 0; i < N; ++i) {
        double vol = sigma * std::sqrt(T[i]);
        double d1 = (std::log(S[i] / K[i]) + (r + 0.5 * sigma * sigma) * T[i]) / vol;
        double d2 = d1 - vol;
        double nd1 = 0.5 * std::erfc(-d1 * M_SQRT1_2);
        double nd2 = 0.5 * std::erfc(-d2 * M_SQRT1_2);
        double disc = std::exp(-r * T[i]);
        call[i] = S[i] * nd1 - K[i] * disc * nd2;
        put[i] = K[i] * disc * (1.0 - nd2) - S[i] * (1.0 - nd1);
    }
}

__attribute__((noinline))
void mandelbrot(int *__restrict__ out, int W, int H, int max_iter) {
    for (int row = 0; row < H; ++row) {
        for (int col = 0; col < W; ++col) {
            double cr = (col - W / 2.0) * 4.0 / W;
            double ci = (row - H / 2.0) * 4.0 / H;
            double zr = 0.0, zi = 0.0;
            int iter = 0;
            while (zr * zr + zi * zi <= 4.0 && iter < max_iter) {
                double tmp = zr * zr - zi * zi + cr;
                zi = 2.0 * zr * zi + ci;
                zr = tmp;
                ++iter;
            }
            out[row * W + col] = iter;
        }
    }
}

__attribute__((noinline))
void nbody_step(double *__restrict__ x, double *__restrict__ y, double *__restrict__ z,
                double *__restrict__ vx, double *__restrict__ vy, double *__restrict__ vz,
                const double *__restrict__ mass, int N, double dt) {
    constexpr double SOFTENING = 1e-9;
    for (int i = 0; i < N; ++i) {
        double fx = 0.0, fy = 0.0, fz = 0.0;
        for (int j = 0; j < N; ++j) {
            double dx = x[j] - x[i];
            double dy = y[j] - y[i];
            double dz = z[j] - z[i];
            double dist = std::sqrt(dx * dx + dy * dy + dz * dz + SOFTENING);
            double inv_dist3 = 1.0 / (dist * dist * dist);
            fx += mass[j] * dx * inv_dist3;
            fy += mass[j] * dy * inv_dist3;
            fz += mass[j] * dz * inv_dist3;
        }
        vx[i] += dt * fx;
        vy[i] += dt * fy;
        vz[i] += dt * fz;
    }
    for (int i = 0; i < N; ++i) {
        x[i] += dt * vx[i];
        y[i] += dt * vy[i];
        z[i] += dt * vz[i];
    }
}

__attribute__((noinline))
void gaussian_blur(double *__restrict__ out, const double *__restrict__ in,
                   int W, int H) {
    constexpr int R = 5;
    constexpr int D = 2 * R + 1;
    double kernel[D * D];
    double sigma = 1.5;
    double sum = 0.0;
    for (int dy = -R; dy <= R; ++dy) {
        for (int dx = -R; dx <= R; ++dx) {
            double v = std::exp(-(dx * dx + dy * dy) / (2.0 * sigma * sigma));
            kernel[(dy + R) * D + (dx + R)] = v;
            sum += v;
        }
    }
    for (int i = 0; i < D * D; ++i) kernel[i] /= sum;

    for (int row = R; row < H - R; ++row) {
        for (int col = R; col < W - R; ++col) {
            double acc = 0.0;
            for (int dy = -R; dy <= R; ++dy) {
                for (int dx = -R; dx <= R; ++dx) {
                    acc += in[(row + dy) * W + (col + dx)] * kernel[(dy + R) * D + (dx + R)];
                }
            }
            out[row * W + col] = acc;
        }
    }
}

// ---- Fixtures ----

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

class BlackScholesBench : public benchmark::Fixture {
  public:
    static constexpr int N = 10'000'000;
    std::vector<double> S, K, T, call, put;

    void SetUp(benchmark::State &) override {
        S.resize(N);
        K.resize(N);
        T.resize(N);
        call.resize(N);
        put.resize(N);
        for (int i = 0; i < N; ++i) {
            S[i] = 80.0 + 0.000004 * i;
            K[i] = 100.0;
            T[i] = 0.5 + 0.0000001 * i;
        }
    }

    void TearDown(benchmark::State &) override {
        S.clear(); K.clear(); T.clear();
        call.clear(); put.clear();
    }
};

class MandelbrotBench : public benchmark::Fixture {
  public:
    static constexpr int W = 4000, H = 4000, MAX_ITER = 256;
    std::vector<int> out;

    void SetUp(benchmark::State &) override { out.resize((size_t)W * H); }
    void TearDown(benchmark::State &) override { out.clear(); }
};

class NBodyBench : public benchmark::Fixture {
  public:
    static constexpr int N = 8192;
    std::vector<double> x, y, z, vx, vy, vz, mass;

    void SetUp(benchmark::State &) override {
        x.resize(N); y.resize(N); z.resize(N);
        vx.resize(N); vy.resize(N); vz.resize(N);
        mass.resize(N);
        for (int i = 0; i < N; ++i) {
            x[i] = 0.001 * (i % 1000);
            y[i] = 0.002 * ((i * 7) % 1000);
            z[i] = 0.001 * ((i * 13) % 1000);
            vx[i] = vy[i] = vz[i] = 0.0;
            mass[i] = 1.0 + 0.0001 * i;
        }
    }

    void TearDown(benchmark::State &) override {
        x.clear(); y.clear(); z.clear();
        vx.clear(); vy.clear(); vz.clear();
        mass.clear();
    }
};

class GaussianBlurBench : public benchmark::Fixture {
  public:
    static constexpr int W = 4000, H = 4000;
    std::vector<double> in, out;

    void SetUp(benchmark::State &) override {
        in.resize((size_t)W * H);
        out.resize((size_t)W * H, 0.0);
        for (int i = 0; i < W * H; ++i) {
            in[i] = 0.5 + 0.5 * std::sin(0.001 * i);
        }
    }

    void TearDown(benchmark::State &) override {
        in.clear();
        out.clear();
    }
};

// ---- Benchmarks ----

BENCHMARK_F(MatrixVectorBench, Run)(benchmark::State &state) {
    for (auto _ : state) {
        matrix_multiply_row(vec_c.data(), mat_a.data(), vec_b.data(), MAT);
        benchmark::DoNotOptimize(vec_c[MAT / 2]);
    }
}

BENCHMARK_F(BlackScholesBench, Run)(benchmark::State &state) {
    for (auto _ : state) {
        black_scholes(call.data(), put.data(), S.data(), K.data(), T.data(), 0.05, 0.2, N);
        benchmark::DoNotOptimize(call[N / 2]);
    }
}

BENCHMARK_F(MandelbrotBench, Run)(benchmark::State &state) {
    for (auto _ : state) {
        mandelbrot(out.data(), W, H, MAX_ITER);
        benchmark::DoNotOptimize(out[(size_t)W * H / 2]);
    }
}

BENCHMARK_F(NBodyBench, Run)(benchmark::State &state) {
    for (auto _ : state) {
        nbody_step(x.data(), y.data(), z.data(),
                   vx.data(), vy.data(), vz.data(),
                   mass.data(), N, 0.001);
        benchmark::DoNotOptimize(x[N / 2]);
    }
}

BENCHMARK_F(GaussianBlurBench, Run)(benchmark::State &state) {
    for (auto _ : state) {
        gaussian_blur(out.data(), in.data(), W, H);
        benchmark::DoNotOptimize(out[(size_t)W * H / 2]);
    }
}

BENCHMARK_MAIN();
