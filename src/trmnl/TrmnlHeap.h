#pragma once

namespace TrmnlHeap {

// Frees Wi-Fi-adjacent font caches so PNGdec can allocate during TRMNL fetch finalize.
void releaseTransientMemory();

}  // namespace TrmnlHeap
