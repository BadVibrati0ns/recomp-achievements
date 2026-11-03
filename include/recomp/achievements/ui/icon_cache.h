#pragma once

// Turns icon refs into renderer textures. The renderer supplies texture
// creation; platform readers supply image bytes for their schemes
// ("xdbf:<id>", "trophy:<file>", "ra:<badge>"); plain refs are files relative
// to the set's base_dir. Get() must be called on the thread that may create
// textures (the render thread).

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace recomp::achievements::ui {

struct Image {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> rgba;  // width * height * 4, straight alpha.
};

struct IconRequest {
  std::string_view ref;     // Full ref, e.g. "xdbf:12".
  std::string_view scheme;  // "xdbf"; empty for file refs.
  std::string_view body;    // "12"; the path for file refs.
  std::string_view set_id;
  const std::filesystem::path* base_dir = nullptr;
};

// What a provider hands back: encoded bytes (PNG, BMP, ...) for the decoder,
// or an already decoded image.
struct IconData {
  std::vector<uint8_t> bytes;
  Image image;
};

enum class IconResult {
  kOk,
  kPending,  // Not ready yet (e.g. downloading); asked again later.
  kMissing,  // Never available; cached as missing.
};

// Built-in image decoding (PNG, JPEG, BMP, TGA via stb_image) to RGBA. No SDL
// needed; IconCache uses it unless SetDecoder replaces it.
bool DecodeImage(std::span<const uint8_t> bytes, Image& out);

class IconCache {
 public:
  // Returns the renderer's texture handle (an ImTextureID cast to void* for
  // ImGui), or null on failure.
  using CreateTexture = std::function<void*(int width, int height, const uint8_t* rgba)>;
  using DestroyTexture = std::function<void(void* texture)>;
  using Decoder = std::function<bool(std::span<const uint8_t> bytes, Image& out)>;
  using Provider = std::function<IconResult(const IconRequest& request, IconData& out)>;

  IconCache();
  ~IconCache();  // Destroys every texture it created.
  IconCache(const IconCache&) = delete;
  IconCache& operator=(const IconCache&) = delete;

  void SetTextureCallbacks(CreateTexture create, DestroyTexture destroy = nullptr);
  // Without a decoder only decoded IconData::image results work. The UI
  // library installs SDL3's PNG/BMP decoder when built with SDL3.
  void SetDecoder(Decoder decoder);
  bool has_decoder() const;
  // Registers a source for refs "<scheme>:<body>". Replaces an earlier one.
  void AddProvider(std::string scheme, Provider provider);
  void RemoveProvider(std::string_view scheme);

  // Texture for ref, or null if it has none (yet). Failures are cached until
  // Clear(); pending results are retried after retry_seconds.
  void* Get(std::string_view ref, std::string_view set_id = {},
            const std::filesystem::path& base_dir = {});
  // Loads the image without creating a texture (for tests / other renderers).
  IconResult Load(std::string_view ref, std::string_view set_id,
                  const std::filesystem::path& base_dir, Image& out);

  void SetRetrySeconds(double seconds);
  void Clear();  // Destroys textures; forget failures.
  size_t size() const;

 private:
  struct Entry {
    void* texture = nullptr;
    bool failed = false;
    double retry_at = 0.0;  // Pending: monotonic seconds when to ask again.
    int width = 0, height = 0;
  };

  mutable std::mutex mutex_;
  CreateTexture create_;
  DestroyTexture destroy_;
  Decoder decoder_ = DecodeImage;
  std::map<std::string, Provider, std::less<>> providers_;
  std::map<std::string, Entry, std::less<>> entries_;
  double retry_seconds_ = 2.0;
};

// Splits "scheme:body". Windows drive letters ("C:/x.png") and refs without a
// scheme come back with an empty scheme.
void SplitIconRef(std::string_view ref, std::string_view& scheme, std::string_view& body);

// Reads a whole file; false if it can't be opened.
bool ReadFileBytes(const std::filesystem::path& path, std::vector<uint8_t>& out);

}  // namespace recomp::achievements::ui
