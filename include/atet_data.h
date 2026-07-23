#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace atet {

// In-engine reimplementation of the Python asset pipeline
// (scripts/extract_atet_dat.py + extract_palettes.py + decode_verified_graphics.py):
// parses ATET.DAT into chunks and decodes the exact buffers the port's loaders
// consume, in memory, so no extracted/copyrighted assets are ever shipped. The
// user's own ATET.DAT (from the bundled or user-supplied ACiD Tetris archive) is
// decoded locally at load time -- the ScummVM/DOSBox model.
class AtetData {
 public:
  struct Indexed {
    int width = 0;
    int height = 0;
    std::vector<Uint8> indices;  // one palette index per pixel, row-major
  };

  // Parse + decode an ATET.DAT image. Returns false (with *error) if the archive
  // header or a known chunk layout does not validate.
  bool LoadFromDat(const std::vector<Uint8>& dat_bytes, std::string* error);

  // Extract ATET.DAT from an ACiD Tetris archive (plain .zip; deflate/stored),
  // then LoadFromDat. Returns false if the zip has no ATET.DAT member.
  bool LoadFromZip(const std::vector<Uint8>& zip_bytes, std::string* error);

  bool loaded() const { return loaded_; }

  // Palettes (VGA 6-bit scaled to 8-bit) from each screen chunk's 768-byte prefix:
  // chunk 0 = DDD splash, chunk 1 = gameplay, chunk 2 = warning splash, chunk 4 = title.
  const std::array<SDL_Color, 256>& ddd_palette() const { return ddd_palette_; }
  const std::array<SDL_Color, 256>& gameplay_palette() const { return gameplay_palette_; }
  const std::array<SDL_Color, 256>& warning_palette() const { return warning_palette_; }
  const std::array<SDL_Color, 256>& title_palette() const { return title_palette_; }

  // 320x240 local-palette screens.
  const Indexed& ddd_splash() const { return ddd_splash_; }
  const Indexed& gameplay_screen() const { return gameplay_screen_; }
  const Indexed& warning_splash() const { return warning_splash_; }
  const Indexed& title_screen() const { return title_screen_; }

  // Gameplay auxiliary atlases + overlay (chunk 3 / chunk 8).
  const Indexed& piece_atlas() const { return piece_atlas_; }        // 56x8
  const Indexed& digit_atlas() const { return digit_atlas_; }        // 25x10
  const Indexed& game_over_overlay() const { return game_over_overlay_; }  // 128x36

  // chunk 6 alert/mood-face bank: 14 tiles of 50x50.
  int alert_tile_count() const { return static_cast<int>(alert_tiles_.size()); }
  const Indexed& alert_tile(int index) const { return alert_tiles_.at(static_cast<size_t>(index)); }

  // chunk 5 frontend font atlas (176x72) + its per-glyph width table (66 bytes).
  const Indexed& font_atlas() const { return font_atlas_; }
  const std::vector<Uint8>& font_widths() const { return font_widths_; }

  // chunk 3 four-stage particle color ramps (0x400 bytes) + chunk 7 frontend
  // animation bank set (0x38000 bytes), both consumed as raw blobs by the port.
  const std::vector<Uint8>& particle_ramps() const { return particle_ramps_; }
  const std::vector<Uint8>& frontend_bankset() const { return frontend_bankset_; }

  // Raw chunk bytes -- audio (WAV) and music (MikMod UNI) chunks are stored
  // ready-to-use in ATET.DAT, so the port consumes them verbatim.
  const std::vector<Uint8>& chunk(int chunk_id) const;
  int chunk_count() const { return static_cast<int>(chunks_.size()); }

 private:
  bool loaded_ = false;
  std::vector<std::vector<Uint8>> chunks_;
  std::array<SDL_Color, 256> ddd_palette_{};
  std::array<SDL_Color, 256> gameplay_palette_{};
  std::array<SDL_Color, 256> warning_palette_{};
  std::array<SDL_Color, 256> title_palette_{};
  Indexed ddd_splash_;
  Indexed gameplay_screen_;
  Indexed warning_splash_;
  Indexed title_screen_;
  Indexed piece_atlas_;
  Indexed digit_atlas_;
  Indexed game_over_overlay_;
  std::vector<Indexed> alert_tiles_;
  Indexed font_atlas_;
  std::vector<Uint8> font_widths_;
  std::vector<Uint8> particle_ramps_;
  std::vector<Uint8> frontend_bankset_;
  std::vector<Uint8> empty_;
};

}  // namespace atet
