#include "atet_data.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "miniz.h"

namespace atet {
namespace {

Uint32 ReadLE32(const std::vector<Uint8>& data, size_t offset) {
  return static_cast<Uint32>(data[offset]) | (static_cast<Uint32>(data[offset + 1]) << 8) |
         (static_cast<Uint32>(data[offset + 2]) << 16) | (static_cast<Uint32>(data[offset + 3]) << 24);
}

// scripts/common.py rle4_decompress: a 2-byte (color1,color2) header then control
// bytes; count = (ctrl & 0x3F) + 1, mode = ctrl >> 6 (0=literal copy, 1=color2
// run, 2=explicit-value run, 3=color1 run). Output is clamped to out_size.
std::vector<Uint8> Rle4Decompress(const Uint8* data, size_t len, size_t out_size) {
  std::vector<Uint8> out;
  if (len < 2) {
    return out;
  }
  out.reserve(out_size);
  const Uint8 color1 = data[0];
  const Uint8 color2 = data[1];
  size_t pos = 2;
  while (out.size() < out_size && pos < len) {
    const Uint8 ctrl = data[pos++];
    const int count = (ctrl & 0x3F) + 1;
    const int mode = ctrl >> 6;
    if (mode == 0) {
      for (int i = 0; i < count && pos < len; ++i) {
        out.push_back(data[pos++]);
      }
    } else if (mode == 1) {
      out.insert(out.end(), static_cast<size_t>(count), color2);
    } else if (mode == 2) {
      if (pos >= len) {
        break;
      }
      const Uint8 value = data[pos++];
      out.insert(out.end(), static_cast<size_t>(count), value);
    } else {
      out.insert(out.end(), static_cast<size_t>(count), color1);
    }
  }
  out.resize(out_size, 0);  // valid data fills exactly; guarantee the downstream size
  return out;
}

Uint8 ScaleVga6(Uint8 component) {
  return static_cast<Uint8>(std::lround((static_cast<double>(component) / 63.0) * 255.0));
}

std::array<SDL_Color, 256> ParsePalette768(const std::vector<Uint8>& chunk) {
  std::array<SDL_Color, 256> palette{};
  for (int index = 0; index < 256; ++index) {
    const size_t offset = static_cast<size_t>(index) * 3;
    palette[static_cast<size_t>(index)] = SDL_Color{
        ScaleVga6(chunk[offset]), ScaleVga6(chunk[offset + 1]), ScaleVga6(chunk[offset + 2]), SDL_ALPHA_OPAQUE};
  }
  return palette;
}

// scripts/decode_verified_graphics.py build_glyph_atlas: row-major placement of
// glyph_count fixed glyphs into a columns-wide grid.
AtetData::Indexed BuildGlyphAtlas(const std::vector<Uint8>& glyphs, int glyph_count, int columns, int glyph_width,
                                  int glyph_height) {
  const int rows = (glyph_count + columns - 1) / columns;
  AtetData::Indexed atlas;
  atlas.width = columns * glyph_width;
  atlas.height = rows * glyph_height;
  atlas.indices.assign(static_cast<size_t>(atlas.width) * static_cast<size_t>(atlas.height), 0);
  for (int glyph_index = 0; glyph_index < glyph_count; ++glyph_index) {
    const int atlas_x = (glyph_index % columns) * glyph_width;
    const int atlas_y = (glyph_index / columns) * glyph_height;
    for (int row = 0; row < glyph_height; ++row) {
      const size_t src = static_cast<size_t>(glyph_index) * glyph_width * glyph_height + static_cast<size_t>(row) * glyph_width;
      const size_t dst = static_cast<size_t>(atlas_y + row) * atlas.width + atlas_x;
      for (int col = 0; col < glyph_width; ++col) {
        atlas.indices[dst + col] = (src + col < glyphs.size()) ? glyphs[src + col] : 0;
      }
    }
  }
  return atlas;
}

std::vector<Uint8> Slice(const std::vector<Uint8>& source, size_t start, size_t length) {
  const size_t end = std::min(source.size(), start + length);
  if (start >= source.size()) {
    return {};
  }
  return std::vector<Uint8>(source.begin() + static_cast<std::ptrdiff_t>(start),
                            source.begin() + static_cast<std::ptrdiff_t>(end));
}

}  // namespace

const std::vector<Uint8>& AtetData::chunk(int chunk_id) const {
  if (chunk_id < 0 || chunk_id >= static_cast<int>(chunks_.size())) {
    return empty_;
  }
  return chunks_[static_cast<size_t>(chunk_id)];
}

bool AtetData::LoadFromDat(const std::vector<Uint8>& dat, std::string* error) {
  auto fail = [&](const std::string& message) {
    if (error != nullptr) {
      *error = message;
    }
    return false;
  };

  loaded_ = false;
  chunks_.clear();
  alert_tiles_.clear();

  if (dat.size() < 8) {
    return fail("ATET.DAT is too small to contain a header");
  }
  const Uint32 declared_region_count = ReadLE32(dat, 0);
  const Uint32 table_end = ReadLE32(dat, 4);
  if (table_end < 8 || table_end > dat.size() || (table_end - 8) % 4 != 0) {
    return fail("ATET.DAT offset table is malformed");
  }
  const size_t offset_count = (table_end - 8) / 4;
  std::vector<Uint32> boundaries;
  boundaries.reserve(offset_count + 1);
  for (size_t i = 0; i < offset_count; ++i) {
    boundaries.push_back(ReadLE32(dat, 8 + i * 4));
  }
  boundaries.push_back(static_cast<Uint32>(dat.size()));

  size_t start = table_end;
  for (const Uint32 end : boundaries) {
    if (end < start || end > dat.size()) {
      return fail("ATET.DAT region boundary is out of range");
    }
    chunks_.push_back(Slice(dat, start, end - start));
    start = end;
  }
  (void)declared_region_count;

  // Need chunks 0..8 for graphics; audio/music live in later chunks.
  if (chunks_.size() < 9) {
    return fail("ATET.DAT has fewer chunks than expected");
  }

  ddd_palette_ = ParsePalette768(chunks_[0]);
  gameplay_palette_ = ParsePalette768(chunks_[1]);
  warning_palette_ = ParsePalette768(chunks_[2]);
  title_palette_ = ParsePalette768(chunks_[4]);

  // 320x240 local-palette screens: [768 palette][u32 comp size][rle4 data].
  auto decode_screen = [&](int chunk_id) {
    const std::vector<Uint8>& c = chunks_[static_cast<size_t>(chunk_id)];
    const Uint32 comp_size = ReadLE32(c, 768);
    Indexed image;
    image.width = 320;
    image.height = 240;
    image.indices = Rle4Decompress(c.data() + 772, std::min<size_t>(comp_size, c.size() - 772), 320 * 240);
    return image;
  };
  ddd_splash_ = decode_screen(0);
  gameplay_screen_ = decode_screen(1);
  warning_splash_ = decode_screen(2);
  title_screen_ = decode_screen(4);

  // chunk 3: [0x400 particle-ramp lookup][u32 comp][u32 decomp][rle4 pixels]
  // -> 7x (8x8) piece tiles + 10x (5x5) digits, each built into an atlas.
  {
    const std::vector<Uint8>& c = chunks_[3];
    particle_ramps_ = Slice(c, 0, 0x400);
    const Uint32 comp_size = ReadLE32(c, 0x400);
    const Uint32 decomp_size = ReadLE32(c, 0x404);
    const std::vector<Uint8> pixels =
        Rle4Decompress(c.data() + 0x408, std::min<size_t>(comp_size, c.size() - 0x408), decomp_size);
    const std::vector<Uint8> piece_region = Slice(pixels, 0, 7 * 64);
    piece_atlas_ = BuildGlyphAtlas(piece_region, 7, 7, 8, 8);
    const std::vector<Uint8> digit_region = Slice(pixels, 7 * 64, 10 * 25);
    digit_atlas_ = BuildGlyphAtlas(digit_region, 10, 5, 5, 5);
  }

  // chunk 5: [u32 raw_size][metadata=widths][u32 decomp][u32 comp][rle4 font pixels]
  // -> 176x72 glyph atlas + a 66-byte width table.
  {
    const std::vector<Uint8>& c = chunks_[5];
    const Uint32 raw_size = ReadLE32(c, 0);
    font_widths_ = Slice(c, 4, raw_size);
    const size_t font_off = 4 + raw_size;
    const Uint32 font_decomp = ReadLE32(c, font_off);
    const Uint32 font_comp = ReadLE32(c, font_off + 4);
    const std::vector<Uint8> font_pixels =
        Rle4Decompress(c.data() + font_off + 8, std::min<size_t>(font_comp, c.size() - (font_off + 8)), font_decomp);
    font_atlas_ = BuildGlyphAtlas(font_pixels, static_cast<int>(font_widths_.size()), 11, 16, 12);
  }

  // chunk 6: [u32 decomp][u32 comp][rle4 pixels] -> N x 50x50 alert/mood tiles.
  {
    const std::vector<Uint8>& c = chunks_[6];
    const Uint32 decomp_size = ReadLE32(c, 0);
    const Uint32 comp_size = ReadLE32(c, 4);
    const std::vector<Uint8> pixels =
        Rle4Decompress(c.data() + 8, std::min<size_t>(comp_size, c.size() - 8), decomp_size);
    const size_t tile_count = pixels.size() / (50 * 50);
    for (size_t tile = 0; tile < tile_count; ++tile) {
      Indexed image;
      image.width = 50;
      image.height = 50;
      image.indices = Slice(pixels, tile * 2500, 2500);
      alert_tiles_.push_back(std::move(image));
    }
  }

  // chunk 7: [u32 comp][rle4 -> 8 banks x 0x7000] frontend animation bank set.
  {
    const std::vector<Uint8>& c = chunks_[7];
    const Uint32 comp_size = ReadLE32(c, 0);
    frontend_bankset_ = Rle4Decompress(c.data() + 4, std::min<size_t>(comp_size, c.size() - 4), 8 * 0x7000);
  }

  // chunk 8: [u32 comp][rle4 -> 128x36] GAME OVER overlay.
  {
    const std::vector<Uint8>& c = chunks_[8];
    const Uint32 comp_size = ReadLE32(c, 0);
    game_over_overlay_.width = 128;
    game_over_overlay_.height = 36;
    game_over_overlay_.indices = Rle4Decompress(c.data() + 4, std::min<size_t>(comp_size, c.size() - 4), 128 * 36);
  }

  loaded_ = true;
  return true;
}

bool AtetData::LoadFromZip(const std::vector<Uint8>& zip_bytes, std::string* error) {
  auto fail = [&](const std::string& message) {
    if (error != nullptr) {
      *error = message;
    }
    return false;
  };
  if (zip_bytes.empty()) {
    return fail("ACiD Tetris archive is empty.");
  }

  mz_zip_archive zip;
  std::memset(&zip, 0, sizeof(zip));
  if (!mz_zip_reader_init_mem(&zip, zip_bytes.data(), zip_bytes.size(), 0)) {
    return fail("Not a valid ZIP archive.");
  }
  // IGNORE_PATH matches ATET.DAT by basename regardless of any folder prefix, and
  // (CASE_SENSITIVE unset) case-insensitively, so a lowercase 'atet.dat' member
  // in the intact ACiD Tetris archive is found.
  const int index = mz_zip_reader_locate_file(&zip, "ATET.DAT", nullptr, MZ_ZIP_FLAG_IGNORE_PATH);
  if (index < 0) {
    mz_zip_reader_end(&zip);
    return fail("Archive does not contain ATET.DAT.");
  }
  size_t out_size = 0;
  void* extracted = mz_zip_reader_extract_to_heap(&zip, static_cast<mz_uint>(index), &out_size, 0);
  mz_zip_reader_end(&zip);
  if (extracted == nullptr) {
    return fail("Failed to extract ATET.DAT from the archive.");
  }
  const Uint8* begin = static_cast<const Uint8*>(extracted);
  std::vector<Uint8> dat(begin, begin + out_size);
  mz_free(extracted);
  return LoadFromDat(dat, error);
}

}  // namespace atet
