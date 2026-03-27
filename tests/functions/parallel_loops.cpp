#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

// ======= Simple loops =======

void cpy_safe(int *__restrict__ dst, int *__restrict__ src, int N) {
    for (int i = 0; i < N; ++i) {
        dst[i] = src[i];
    }
}

// ======= Simple reductions =======

int easy_sum(int input[], int N) {
    int sum = 0;
    for (int i = 0; i < N; ++i) {
        sum += input[i];
    }
    return sum;
}

long long sum_array(const long long *data, int N) {
    long long sum = 0;
    for (int i = 0; i < N; ++i) {
        sum += data[i];
    }
    return sum;
}

double fp_product(const double *data, int N) {
    double product = 1.0;
    for (int i = 0; i < N; ++i) {
        product *= data[i];
    }
    return product;
}

// ======= Nested loops with collapse =======

void scale_matrix(int *data, int M, int N, int factor) {
    for (int i = 0; i < M; ++i) {
        for (int j = 0; j < N; ++j) {
            data[i * N + j] *= factor;
        }
    }
}

// ======= Argmemonly function calls =======

static void set_val_parallel(int *out, int val) { *out = val; }
static void double_val(int *p) { *p *= 2; }

void argmem_varying(int *data, int N) {
    for (int i = 0; i < N; ++i) {
        set_val_parallel(&data[i], i * 3);
    }
}

void local_temp(int *out, int N) {
    for (int i = 0; i < N; ++i) {
        int a = i;
        int b = i + 1;
        double_val(&a);
        double_val(&b);
        out[i] = a + b;
    }
}

// ======= Non-overlapping subscripts =======

void interleaved_rw(int *a, int N) {
    for (int i = 0; i < N; ++i) {
        a[2 * i] = a[2 * i + 1] + 1;
    }
}

void split_array(int *a, int N) {
    for (int i = 0; i < N; ++i) {
        a[i] = a[i + N] + 1;
    }
}

// ======= Global array =======

int g_output[1000000];

static void update_global_at(int idx, int val) { g_output[idx] = val; }

void global_indexed_write(int *data, int N) {
    for (int i = 0; i < N; ++i) {
        update_global_at(i, data[i] * 2);
    }
}

// ======= Matrices =======

void fill_matrix(int *data, int M, int N) {
    for (int i = 0; i < M; ++i) {
        for (int j = 0; j < N; ++j) {
            data[i * N + j] = i * 1000 + j;
        }
    }
}

void add_matrices(int *__restrict__ C, const int *__restrict__ A, const int *__restrict__ B, int M,
                  int N) {
    for (int i = 0; i < M; ++i) {
        for (int j = 0; j < N; ++j) {
            C[i * N + j] = A[i * N + j] + B[i * N + j];
        }
    }
}

void matrix_vector(double *__restrict__ C, const double *__restrict__ A,
                   const double *__restrict__ B, int N) {
    for (int i = 0; i < N; ++i) {
        double sum = 0.0;
        for (int j = 0; j < N; ++j) {
            sum += A[i * N + j] * B[j];
        }
        C[i] = sum;
    }
}

// ======= SIMD reductions =======

double dot_product(const double *__restrict__ a, const double *__restrict__ b, int N) {
    double sum = 0.0;
    for (int i = 0; i < N; ++i) {
        sum += a[i] * b[i];
    }
    return sum;
}

int find_max(const int *data, int N) {
    int mx = data[0];
    for (int i = 1; i < N; ++i) {
        if (data[i] > mx)
            mx = data[i];
    }
    return mx;
}

int find_min(const int *data, int N) {
    int mn = data[0];
    for (int i = 1; i < N; ++i) {
        if (data[i] < mn)
            mn = data[i];
    }
    return mn;
}

int bitwise_or_reduce(const int *data, int N) {
    int result = 0;
    for (int i = 0; i < N; ++i) {
        result |= data[i];
    }
    return result;
}

// ======= Heavy computation =======

double do_heavy_math(double input, int iterations = 20) noexcept {
    double x = input;
    double y = 1.0;

    if (std::fmod(x, 2.0) > 1.0) {
        x = std::sin(x) * std::sqrt(std::abs(x));
        y += std::exp(-std::abs(x));
    } else {
        x = std::cos(x) * std::pow(std::abs((x > 0 ? x : 1.0)), 0.33);
        y -= std::log1p(std::abs(std::sin(x)));
    }

    x = x + (y * 0.001);
    x = std::clamp(x, -10000.0, 10000.0);

    return x + y;
}

void simple_1d(std::vector<int> &input) {
    for (size_t i = 0; i < input.size(); ++i) {
        input[i] = do_heavy_math(i);
    }
}
