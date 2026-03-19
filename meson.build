project(
  'AutoOMPPass',
  'cpp',
  default_options: [
    'cpp_std=c++23',
    'cpp_rtti=false',
    'cpp_eh=none',
  ],
)

# ====== Pass ======
llvm_dep = dependency(
  'llvm',
  version: '>=15',
  method: 'config-tool',
  modules: ['passes', 'core', 'analysis', 'support', 'frontendopenmp'],
)

gtest_dep = dependency('gtest', required: true, include_type: 'system')
gtest_main_dep = dependency('gtest_main', required: true, include_type: 'system')
gbench_dep = dependency('benchmark', required: true, include_type: 'system')

# ====== Plugin Library ======
auto_omp_lib = shared_library(
  'AutoOMPPass',
  'src/AutoOMPPass.cpp',
  'src/LoopCodegen.cpp',
  'src/PluginRegistration.cpp',
  dependencies: [llvm_dep],
  cpp_args: ['-g', '-O0'],
  name_prefix: '',
  install: true,
  include_directories: include_directories('include'),
  gnu_symbol_visibility: 'default',
)

# ====== Test Setup ======
barrier = custom_target('build-barrier',
  output: 'barrier.stamp',
  command: ['touch', '@OUTPUT@'],
  depends: auto_omp_lib  
)

test_exe = executable(
  'loop_tests',
  ['tests/suite.cpp', barrier],
  dependencies: [gtest_dep, gtest_main_dep],
  include_directories: include_directories('tests'),
  cpp_args: [
      '-O3',
      # '-Rpass-analysis=auto-omp',
      '-fpass-plugin=' + auto_omp_lib.full_path()
  ],
  link_args: ['-fopenmp'],
)

test('loop_tests', test_exe)

# ====== Benchmark Setup ======
bench_exe = executable(
  'loop_benchmarks',
  ['tests/bench.cpp', barrier],
  dependencies: [gbench_dep],
  include_directories: include_directories('tests'),
  cpp_args: [
      '-O3',
      '-fpass-plugin=' + auto_omp_lib.full_path()
  ],
  link_args: ['-fopenmp'],
)

benchmark('loop_benchmarks', bench_exe)

bench_seq_exe = executable(
  'loop_benchmarks_seq',
  'tests/bench.cpp',
  dependencies: [gbench_dep],
  include_directories: include_directories('tests'),
  cpp_args: ['-O1'],
)

benchmark('loop_benchmarks_seq', bench_seq_exe)
