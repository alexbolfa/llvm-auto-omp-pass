project(
  'LoopSuggestionPass',
  'cpp',
  version: '0.1.0',
  default_options: [
    'cpp_std=c++17',
    'cpp_rtti=false',
    'cpp_eh=none',
  ],
)

llvm_dep = dependency(
  'llvm',
  version: '>=15',
  modules: ['passes', 'core', 'analysis', 'support'],
  static: false
)

shared_library(
  'LoopSuggestionPass',
  'src/LoopSuggestionAnalyzer.cpp',
  dependencies: [llvm_dep],
  name_prefix: '',
  install: true,
  include_directories: include_directories('include'),
  gnu_symbol_visibility: 'default',
)


