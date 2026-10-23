#pragma once
#include <stdint.h>
#include <vector>

// Rewrites the H.264 SPS in an Annex-B access unit so it declares
// max_num_reorder_frames = 0. Without it the decoder keeps a frame back for
// possible reordering (one full frame of extra delay). The phone encodes
// without B-frames, so this is safe. Returns false if nothing was changed.
bool PatchH264NoReorder(const uint8_t* data, size_t size, std::vector<uint8_t>& out);
