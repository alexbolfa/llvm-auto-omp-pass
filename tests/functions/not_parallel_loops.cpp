// ======= Simple loops =======

void cpy_unsafe(int *dst, int *src, int N) {
    for (int i = 0; i < N; ++i) {
        dst[i] = src[i];
    }
}

// ====== Cross-iteration memory recurrence ======

void fibonacci(int *data, int n) {
    data[0] = 1;
    data[1] = 1;
    for (int i = 2; i < n; i++) {
        data[i] = data[i - 1] + data[i - 2];
    }
}

// ====== Prefix sum (loop-carried dependency) ======

void prefix_sum(int *data, int N) {
    for (int i = 1; i < N; i++) {
        data[i] += data[i - 1];
    }
}

// ====== Function with global side effects ======

int g_counter = 0;
static void write_and_count(int *out, int val) {
    *out = val;
    g_counter++;
}

void global_write(int *data, int N) {
    for (int i = 0; i < N; ++i) {
        write_and_count(&data[i], i);
    }
}

// --

int g_scalar = 0;
static void update_global_scalar(int val) { g_scalar = val; }

void global_scalar_write(int *data, int N) {
    for (int i = 0; i < N; ++i) {
        update_global_scalar(data[i]);
    }
}

// ====== Argmemonly but writes to loop-invariant address ======

static void set_val_not_parallel(int *out, int val) { *out = val; }

void invariant_write(int *data, int N, int *shared) {
    for (int i = 0; i < N; ++i) {
        data[i] = i;
        set_val_not_parallel(shared, i);
    }
}

// ====== Small trip counts (overhead > benefit) ======

void small_fill(int *out) {
    for (int i = 0; i < 64; ++i) {
        out[i] = i * 3;
    }
}
