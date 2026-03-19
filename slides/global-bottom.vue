<template>
  <div v-if="$nav.currentLayout !== 'cover' && $nav.currentPage !== 1">
    <footer
      class="absolute bottom-0 left-0 right-0 flex items-center p-0 text-[7px] pointer-events-none font-mono"
      style="background-color: #f8fafc; border-top: 1px solid #e2e8f0;"
    >
      <div class="flex items-center gap-2 px-3 py-1 bg-slate-200 text-slate-700 font-bold">
        <img src="https://llvm.org/img/DragonMedium.png" alt="LLVM" class="h-2.5 opacity-80" />
        <span>AutoOMPPass</span>
      </div>

      <div class="flex items-center gap-3 px-3 py-1 text-slate-400 italic">
        <span>main*</span>
        <span class="opacity-50">|</span>
        <span>LLVM::Analysis</span>
      </div>

      <div class="flex-grow"></div>

      <div class="flex items-center gap-2 px-4 py-1 bg-[#eb9d00] text-white font-bold">
        <span>{{ $nav.currentPage }}</span>
        <!-- <span class="opacity-50">/</span>
        <span>{{ $nav.total }}</span> -->
      </div>
    </footer>
  </div>
</template>