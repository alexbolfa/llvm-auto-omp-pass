---
theme: default
title: AutoOMPPass
highlighter: shiki
transition: none
colorSchema: light
fonts:
  sans: IBM Plex Sans
  serif: IBM Plex Serif
  mono: JetBrains Mono
drag:
  grid: [10, 10]
---

<style>
:root { --accent: #eb9d00; }
.slidev-layout h1 { font-weight: 700; letter-spacing: -0.02em; }
.terminal-card { @apply rounded-lg p-6 bg-slate-50 border border-slate-200 font-mono text-sm shadow-sm; }
.card { @apply rounded-lg p-6 bg-slate-50 border border-slate-200 shadow-sm; }
.cmd { @apply text-amber-600 font-bold mr-2; }
.comment { @apply text-slate-400 italic; }
.accent { color: var(--accent); font-weight: 600; }
.accent-block {
  @apply pl-6 border-l-2 border-amber-400 ml-4 my-6 font-mono text-sm;
}

h2 {
  margin-top: 0.3rem !important;
}

</style>

# Auto<span class="accent">OMP</span>Pass
Automatic Parallelization of Loops in LLVM

<div class="absolute top-58 right-25 w-30 text-center group">
  <img src="https://upload.wikimedia.org/wikipedia/commons/6/61/ISO_7010_W014.svg" alt="WIP" />
  <p class="text-[10px] font-mono mt-3 text-slate-400 font-bold uppercase tracking-[0.2em]">BUILD_IN_PROGRESS</p>
</div>

---

# [00] Contents

<div class="terminal-card mt-10 space-y-4">
  
<style scoped>
  strong { color: var(--accent); }
</style>

**01** Motivation

**02** Design

**03** Performance

**04** Live demo

**05** Future work

</div>

---
dragPos:
  explosion: 608,289,152,133
---

# [01] Motivation

<div class="grid grid-cols-2 gap-10 mt-8">
<!-- Left -->
<div class="text-lg">

Writing parallel code is hard:
- You must guarantee **no data races**
- You must reason about **aliasing**
- You must handle **reductions correctly**

<br><br>

<p class="text-slate-600">

Mistakes don't crash, they silently corrupt results.

</p>

</div>

<!-- Right -->
<div class="text-lg">

Meanwhile, the compiler already knows:
- Memory dependencies
- Loop bounds
- Alias relationships

<img v-drag="'explosion'" src="https://upload.wikimedia.org/wikipedia/commons/0/00/ISO_7010_W002.svg">

</div>
</div>

---

# [01] Why Not Polly?

<div class="text-sm italic text-slate-500 border-l-2 border-amber-400 pl-4 mb-8">
  "Polly uses a mathematical representation, the polyhedral model... to apply advanced automatic parallelization." 
  <span class="block text-[10px] mt-1 font-mono uppercase tracking-wider">— polly.llvm.org</span>
</div>

Instead of relying on the Control Flow Graph (CFG), Polly converts code into **linear inequalities**. 

<div class="grid grid-cols-2 gap-8 mt-6">
<div class="bg-red-50 p-4 rounded-lg text-red-800">

**Polly Rejects:**

- **Opaque Function Calls**
- **Conditionals**
- **Non-affine indexing**

</div>

<div class="bg-green-50 p-4 rounded-lg text-green-800">

**AutoOMPPass Accepts:**

- **Pure Calls** via `readnone` attributes
- **Reductions**

</div>
</div>

<br>

<p class="text-sm text-slate-600">

By analyzing the `CFG` and LLVM Attributes directly, we are significantly **less restrictive**.

</p>  

---

# [02] Design

<div class="flex justify-center mt-20"> <div class="flex items-center gap-3 font-mono text-sm transform origin-top">
    <div class="flex items-center gap-2 shrink-0">
      <div class="p-2 border border-slate-300 rounded bg-white shadow-sm">Clang -O1</div>
      <span class="text-slate-400 font-bold">»</span>
      <div class="p-2 border border-slate-300 rounded font-bold bg-white shadow-sm">LLVM IR</div>
    </div>
    <div class="w-12 border-t-2 border-dashed border-amber-400 self-center"></div>
    <div class="flex flex-col gap-2 p-4 border-2 border-amber-400 bg-amber-50 rounded-xl shadow-md shrink-0">
      <div class="text-center mb-1">
        <div class="flex flex-col">
          <span class="font-bold text-amber-700 tracking-widest text-[8px] uppercase">AutoOMPPass</span>
          <span class="font-bold text-amber-700 tracking-widest text-[8px] leading-none text-center">- per loop -</span>
        </div>
      </div>
      <div class="px-3 py-1.5 bg-white/80 border border-amber-200 rounded shadow-sm text-[11px]">a. Structure check</div>
      <div class="px-3 py-1.5 bg-white/80 border border-amber-200 rounded shadow-sm text-[11px]">b. Dependence check</div>
      <div class="px-3 py-1.5 bg-white/80 border border-amber-200 rounded shadow-sm text-[11px]">c. Code generation</div>
    </div>
    <div class="w-12 border-t-2 border-dashed border-amber-400 self-center"></div>
    <div class="flex items-center gap-2 shrink-0">
      <div class="p-2 border border-slate-300 rounded bg-white shadow-sm">Parallel IR</div>
      <span class="text-slate-400 font-bold">»</span>
      <div class="p-2 border border-slate-800 rounded bg-slate-800 text-white shadow-lg font-bold">Machine Binary</div>
    </div>

  </div>
</div>

---

# [02.a] Structural Analysis

<style scoped>
  strong { color: var(--accent); }
  .label { @apply font-mono text-[10px] uppercase tracking-widest text-slate-400 font-bold mb-2 block; }
</style>

The pass must verify that the loop follows a canonical form that the OpenMP accepts.

<div class="doc-card mt-6">
  <span class="label">Structural checks</span>

  - **Invariance:** The loop bounds and step size must be loop-invariant.
  
  - **Single Entry/Exit:** Ensure a strictly structured CFG with no early exits.
  
  - **Heuristic Filtering:** The Trip Count must be determinable and > 64.
  
  

</div>

<div class="mt-6 font-mono text-[10px] p-4 bg-slate-900 text-red-400 rounded-lg shadow-inner">
  <span class="text-slate-500"># clang++ -Rpass-analysis=auto-omp</span><br>
  <span class="text-white">remark:</span> <span class="text-red-500">rejected;</span> loop contains multiple exit blocks <span class="text-slate-500">[early_exit]</span><br>
  <span class="text-white">remark:</span> <span class="text-red-500">rejected;</span> trip count is below parallelization threshold (64)
</div>

---

# [02.b] Dependence Analysis

Before transformation, the pass must prove that no data flow exists between iterations.

<style scoped>
  strong { color: var(--accent); }
  .label { @apply font-mono text-[10px] uppercase tracking-widest text-slate-400 font-bold mb-2 block; }
</style>

<div class="doc-card mt-6">
  <span class="label">Dependence Checks</span>

  - **Check reductions:** Verify that PHI-node cycles are strictly associative and commutative.
  
  - **Liveness analysis:** Identify loop-local `alloca` instructions to ensure they can be safely **privatized**.
  
  - **Memory independence:** Use `DependenceInfo` to prove pointer-based stores (arrays) don't overlap.
  
  - **Side-effect:** Reject loops containing opaque function calls or `volatile` memory accesses.
  
  - **Concurrency:** Reject loops writing to atomics.

</div>

<div class="mt-6 font-mono text-[10px] p-4 bg-slate-900 text-red-400 rounded-lg shadow-inner">
  <span class="text-slate-500"># clang++ -Rpass-analysis=auto-omp</span><br>
  <span class="text-white">remark:</span> <span class="text-red-500">❌ rejected;</span> potential loop-carried dependency detected <br>
  <span class="text-white">remark:</span> <span class="text-red-500">❌ rejected;</span> call to 'printf' may have global side effects
</div>

---

# [02.c] Code Generation

<style scoped>
  strong { color: var(--accent); }
  .label { @apply font-mono text-[10px] uppercase tracking-widest text-slate-400 font-bold mb-2 block; }
</style>

Unlike `#pragma omp` which handles the heavy lifting in source code, `OMPIRBuilder` requires manual orchestration of the LLVM IR transformation.

<div class="mt-6">
  <span class="label">Transformation Steps</span>

- **Privatization:** Isolate loop-locals and allocate reduction accumulators.

- **Body Cloning:** Create another loop with remapped variables.

- **CFG Cleanup:** Delete the original serial loop.

- **Result Mapping:** Connect external users to the final merged reduction values.

</div>

<div class="mt-6 font-mono text-[10px] p-4 bg-slate-900 text-emerald-400 rounded-lg shadow-inner">
  <span class="text-slate-500"># clang++ -Rpass-analysis=auto-omp</span><br>
  <span class="text-white">remark:</span> ✅ parallelized: #pragma omp parallel for reduction(+:x)<br>
  <span class="text-white">remark:</span> ✅ parallelized: #omp parallel for
</div>

---

# [03] Performance

<div class="flex justify-center mt-6">

  <img src="/bench_chart.png" class="rounded-xl shadow-lg" style="max-height: 380px;" />

</div>

<p class="text-center muted text-sm mt-4">N = 20M elements · median of 5 runs · -O3</p>

---

<style scoped>
  .accent { color: var(--accent); font-weight: 600; }
</style>

# [04] <span class="accent">Live</span> Demo

<div class="absolute top-50 right-25 w-40 text-center group">
  <img 
    src="https://upload.wikimedia.org/wikipedia/commons/b/b8/ISO_7010_M020.svg" 
    alt="Fasten Seatbelt"
    class="w-full transition-transform duration-500"
  />
  <p class="text-[10px] font-mono mt-4 text-slate-400 font-bold uppercase tracking-[0.2em]">
    Checking...
  </p>
</div>
---

# [05] <span class="accent">Future</span> Work

<div class="grid grid-cols-2 gap-8 mt-6">
<div>

**Deeper anaylises:**

- **Inter-procedural** analysis
- **Enable** global stores

</div>

<div>

**More transformations:**

- **Collapse** nested loops
- **Provide** SIMD suggestions

</div>
</div>

<!-- QUESTIONS -->
---
layout: center
---

<div class="terminal-card text-center scale-150">
  <div class="accent text-xl mb-4">EOF (End of File)</div>
  <div class="text-sm">Questions?</div>
</div>

<div class="absolute bottom-10 w-full text-center">
  <span class="font-mono text-[10px] muted">process exited with status 0</span>
</div>
