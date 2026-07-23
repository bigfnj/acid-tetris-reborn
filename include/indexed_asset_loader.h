#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace atet {

struct IndexedImage {
  int width = 0;
  int height = 0;
  std::vector<Uint8> rgba_pixels;
};

bool LoadIndexedImage(
    std::string_view indexed_path,
    std::string_view palette_path,
    int width,
    int height,
    int transparent_index,
    IndexedImage* image_out,
    std::string* error_out);

// Build an IndexedImage directly from in-memory palette indices + a palette
// (the in-engine-extraction path). transparent_index < 0 = fully opaque;
// otherwise that palette index becomes transparent (e.g. 0, or 0x20 for faces).
IndexedImage BuildIndexedImage(
    const std::vector<Uint8>& indices,
    int width,
    int height,
    const std::array<SDL_Color, 256>& palette,
    int transparent_index);

SDL_Texture* CreateTextureFromImage(SDL_Renderer* renderer, const IndexedImage& image, std::string* error_out);

}  // namespace atet
