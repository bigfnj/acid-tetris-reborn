#include "indexed_asset_loader.h"

#include <array>
#include <cctype>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace atet {

namespace {

constexpr size_t kPaletteColorCount = 256;
constexpr size_t kPaletteComponentCount = kPaletteColorCount * 3;

void SetErrorMessage(std::string* error_out, std::string message) {
  if (error_out == nullptr) {
    return;
  }

  *error_out = std::move(message);
}

bool ReadBinaryFile(std::string_view path, std::vector<Uint8>* bytes_out, std::string* error_out) {
  if (bytes_out == nullptr) {
    SetErrorMessage(error_out, "Binary output buffer was null.");
    return false;
  }

  std::ifstream input(std::string(path), std::ios::binary);
  if (!input) {
    SetErrorMessage(error_out, "Failed to open binary asset: " + std::string(path));
    return false;
  }

  bytes_out->assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  if (!input.good() && !input.eof()) {
    SetErrorMessage(error_out, "Failed while reading binary asset: " + std::string(path));
    return false;
  }

  return true;
}

bool ReadTextFile(std::string_view path, std::string* text_out, std::string* error_out) {
  if (text_out == nullptr) {
    SetErrorMessage(error_out, "Text output buffer was null.");
    return false;
  }

  std::ifstream input{std::string(path)};
  if (!input) {
    SetErrorMessage(error_out, "Failed to open text asset: " + std::string(path));
    return false;
  }

  std::ostringstream buffer;
  buffer << input.rdbuf();
  if (!input.good() && !input.eof()) {
    SetErrorMessage(error_out, "Failed while reading text asset: " + std::string(path));
    return false;
  }

  *text_out = buffer.str();
  return true;
}

bool ParseColors8Bit(std::string_view json, std::array<SDL_Color, kPaletteColorCount>* palette_out, std::string* error_out) {
  if (palette_out == nullptr) {
    SetErrorMessage(error_out, "Palette output buffer was null.");
    return false;
  }

  const size_t key_position = json.find("\"colors_8bit\"");
  if (key_position == std::string_view::npos) {
    SetErrorMessage(error_out, "Palette JSON is missing the colors_8bit array.");
    return false;
  }

  const size_t array_start = json.find('[', key_position);
  if (array_start == std::string_view::npos) {
    SetErrorMessage(error_out, "Palette JSON has no colors_8bit array start.");
    return false;
  }

  std::array<int, kPaletteComponentCount> components{};
  size_t component_index = 0;
  int value = 0;
  bool in_number = false;

  for (size_t i = array_start; i < json.size() && component_index < components.size(); ++i) {
    const unsigned char ch = static_cast<unsigned char>(json[i]);
    if (std::isdigit(ch)) {
      value = in_number ? (value * 10) + static_cast<int>(ch - '0') : static_cast<int>(ch - '0');
      in_number = true;
      continue;
    }

    if (in_number) {
      components[component_index++] = value;
      value = 0;
      in_number = false;
    }
  }

  if (in_number && component_index < components.size()) {
    components[component_index++] = value;
  }

  if (component_index != components.size()) {
    SetErrorMessage(error_out, "Palette JSON did not contain 256 RGB colors in colors_8bit.");
    return false;
  }

  for (size_t color_index = 0; color_index < kPaletteColorCount; ++color_index) {
    const size_t offset = color_index * 3;
    (*palette_out)[color_index] = SDL_Color{
        static_cast<Uint8>(components[offset + 0]),
        static_cast<Uint8>(components[offset + 1]),
        static_cast<Uint8>(components[offset + 2]),
        SDL_ALPHA_OPAQUE};
  }

  return true;
}

}  // namespace

bool LoadIndexedImage(
    std::string_view indexed_path,
    std::string_view palette_path,
    int width,
    int height,
    int transparent_index,
    IndexedImage* image_out,
    std::string* error_out) {
  if (image_out == nullptr) {
    SetErrorMessage(error_out, "Image output buffer was null.");
    return false;
  }

  if (width <= 0 || height <= 0) {
    SetErrorMessage(error_out, "Indexed image dimensions must be positive.");
    return false;
  }

  std::vector<Uint8> indexed_bytes;
  if (!ReadBinaryFile(indexed_path, &indexed_bytes, error_out)) {
    return false;
  }

  const size_t expected_size = static_cast<size_t>(width) * static_cast<size_t>(height);
  if (indexed_bytes.size() != expected_size) {
    SetErrorMessage(
        error_out,
        "Indexed asset size mismatch for " + std::string(indexed_path) + ": expected " +
            std::to_string(expected_size) + " bytes, got " + std::to_string(indexed_bytes.size()) + ".");
    return false;
  }

  std::string palette_json;
  if (!ReadTextFile(palette_path, &palette_json, error_out)) {
    return false;
  }

  std::array<SDL_Color, kPaletteColorCount> palette{};
  if (!ParseColors8Bit(palette_json, &palette, error_out)) {
    return false;
  }

  IndexedImage image;
  image.width = width;
  image.height = height;
  image.rgba_pixels.resize(expected_size * 4);

  for (size_t pixel_index = 0; pixel_index < expected_size; ++pixel_index) {
    const Uint8 palette_index = indexed_bytes[pixel_index];
    const SDL_Color color = palette[palette_index];
    const size_t rgba_offset = pixel_index * 4;
    image.rgba_pixels[rgba_offset + 0] = color.r;
    image.rgba_pixels[rgba_offset + 1] = color.g;
    image.rgba_pixels[rgba_offset + 2] = color.b;
    // transparent_index < 0 -> fully opaque; otherwise the given palette index
    // is the color-key. Most assets key index 0; the chunk-6 alert/face tiles
    // key index 0x20 (RE board-alert-pass: 0x1793d treats palette index 0x20 as
    // transparent), which is why the face is an icon over the water, not a box.
    image.rgba_pixels[rgba_offset + 3] =
        (transparent_index >= 0 && palette_index == static_cast<Uint8>(transparent_index))
            ? static_cast<Uint8>(SDL_ALPHA_TRANSPARENT)
            : static_cast<Uint8>(SDL_ALPHA_OPAQUE);
  }

  *image_out = std::move(image);
  if (error_out != nullptr) {
    error_out->clear();
  }

  return true;
}

IndexedImage BuildIndexedImage(
    const std::vector<Uint8>& indices,
    int width,
    int height,
    const std::array<SDL_Color, 256>& palette,
    int transparent_index) {
  IndexedImage image;
  image.width = width;
  image.height = height;
  const size_t pixel_count = static_cast<size_t>(width) * static_cast<size_t>(height);
  image.rgba_pixels.resize(pixel_count * 4);
  for (size_t pixel_index = 0; pixel_index < pixel_count; ++pixel_index) {
    const Uint8 palette_index = pixel_index < indices.size() ? indices[pixel_index] : 0;
    const SDL_Color color = palette[palette_index];
    const size_t rgba_offset = pixel_index * 4;
    image.rgba_pixels[rgba_offset + 0] = color.r;
    image.rgba_pixels[rgba_offset + 1] = color.g;
    image.rgba_pixels[rgba_offset + 2] = color.b;
    image.rgba_pixels[rgba_offset + 3] =
        (transparent_index >= 0 && palette_index == static_cast<Uint8>(transparent_index))
            ? static_cast<Uint8>(SDL_ALPHA_TRANSPARENT)
            : static_cast<Uint8>(SDL_ALPHA_OPAQUE);
  }
  return image;
}

SDL_Texture* CreateTextureFromImage(SDL_Renderer* renderer, const IndexedImage& image, std::string* error_out) {
  if (renderer == nullptr) {
    SetErrorMessage(error_out, "Renderer was null while creating a texture.");
    return nullptr;
  }

  // rgba_pixels is a byte array in R,G,B,A order. SDL_PIXELFORMAT_RGBA8888 is a
  // *packed* 32-bit format (0xRRGGBBAA), which on little-endian reads those bytes
  // as A,B,G,R -> channels mangled (art rendered red-shifted). RGBA32 is the
  // endianness-correct array alias for R,G,B,A byte order.
  SDL_Texture* texture =
      SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, image.width, image.height);
  if (texture == nullptr) {
    SetErrorMessage(error_out, SDL_GetError());
    return nullptr;
  }

  if (!SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST)) {
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "SDL_SetTextureScaleMode failed: %s", SDL_GetError());
  }

  if (!SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND)) {
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "SDL_SetTextureBlendMode failed: %s", SDL_GetError());
  }

  if (!SDL_UpdateTexture(texture, nullptr, image.rgba_pixels.data(), image.width * 4)) {
    SetErrorMessage(error_out, SDL_GetError());
    SDL_DestroyTexture(texture);
    return nullptr;
  }

  if (error_out != nullptr) {
    error_out->clear();
  }

  return texture;
}

}  // namespace atet
