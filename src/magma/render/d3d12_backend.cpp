#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "backend.h"
#include "environment.h"
#include "glyph_atlas.h"
#include "gpu_scene.h"
#include "image_cache.h"
#include "pixels.h"
#include "scene_walk.h"

// The shader source from shaders/gpu.hlsl, as `hlsl_source`.
#include "gpu_hlsl.h"

namespace magma::render {
namespace {

template <class type> using com_ptr = Microsoft::WRL::ComPtr<type>;

// Frames the CPU may record before the GPU has finished the oldest of them.
constexpr auto frames_in_flight = std::size_t{3};
constexpr auto swapchain_buffers = UINT{3};
// Images are not evicted one at a time. A cache this large means the caller has moved on through
// many images, so starting over is cheaper than tracking use.
constexpr auto image_cache_limit = std::size_t{64};
constexpr auto smallest_instance_buffer = std::size_t{64} * 1024;
constexpr auto smallest_staging_buffer = std::size_t{1024} * 1024;
constexpr auto descriptors_per_frame = UINT{2048};
constexpr auto frame_format = DXGI_FORMAT_B8G8R8A8_UNORM;
constexpr auto row_pitch_alignment = std::size_t{D3D12_TEXTURE_DATA_PITCH_ALIGNMENT};
constexpr auto placement_alignment = std::size_t{D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT};

auto failure(std::string_view what, HRESULT result) -> render_error {
  return {"Direct3D 12 could not " + std::string{what} + " (HRESULT " +
          std::to_string(static_cast<long>(result)) + ")"};
}

auto align_up(std::size_t value, std::size_t alignment) -> std::size_t {
  return (value + alignment - 1) / alignment * alignment;
}

auto warp_requested() -> bool { return environment_equals("MAGMA_D3D12_ADAPTER", "warp"); }

auto validation_requested() -> bool { return environment_equals("MAGMA_D3D12_VALIDATION", "1"); }

struct handle_closer {
  auto operator()(void *handle) const -> void { CloseHandle(handle); }
};
using event_handle = std::unique_ptr<void, handle_closer>;

auto transition_barrier(ID3D12Resource *resource, D3D12_RESOURCE_STATES from,
                        D3D12_RESOURCE_STATES to) -> D3D12_RESOURCE_BARRIER {
  auto barrier = D3D12_RESOURCE_BARRIER{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = from;
  barrier.Transition.StateAfter = to;
  return barrier;
}

auto buffer_description(std::size_t size) -> D3D12_RESOURCE_DESC {
  auto description = D3D12_RESOURCE_DESC{};
  description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  description.Width = size;
  description.Height = 1;
  description.DepthOrArraySize = 1;
  description.MipLevels = 1;
  description.Format = DXGI_FORMAT_UNKNOWN;
  description.SampleDesc = {1, 0};
  description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  return description;
}

// The first adapter that the backend accepts: the GPU with the best performance, or WARP when
// MAGMA_D3D12_ADAPTER asks for it. Never the software adapter otherwise.
auto choose_adapter(IDXGIFactory4 *factory) -> com_ptr<IDXGIAdapter1> {
  auto adapter = com_ptr<IDXGIAdapter1>{};
  if (warp_requested()) {
    factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
    return adapter;
  }
  auto const accepts = [](IDXGIAdapter1 *candidate) {
    auto description = DXGI_ADAPTER_DESC1{};
    if (FAILED(candidate->GetDesc1(&description)) ||
        (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
      return false;
    }
    return SUCCEEDED(
        D3D12CreateDevice(candidate, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr));
  };
  auto ordered = com_ptr<IDXGIFactory6>{};
  if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&ordered)))) {
    for (auto index = UINT{0}; SUCCEEDED(ordered->EnumAdapterByGpuPreference(
             index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)));
         ++index) {
      if (accepts(adapter.Get())) {
        return adapter;
      }
    }
    return nullptr;
  }
  for (auto index = UINT{0}; SUCCEEDED(factory->EnumAdapters1(index, &adapter)); ++index) {
    if (accepts(adapter.Get())) {
      return adapter;
    }
  }
  return nullptr;
}

struct gpu_buffer {
  com_ptr<ID3D12Resource> resource;
  void *mapped = nullptr;
  std::size_t size = 0;
};

// A texture, and for render targets its view. `state` is the state the commands recorded so far
// leave it in.
struct gpu_image {
  com_ptr<ID3D12Resource> resource;
  com_ptr<ID3D12DescriptorHeap> render_view;
  int width = 0;
  int height = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
};

// One render target of a frame: the frame itself or a layer, and the frame device pixels it covers.
struct pass_target {
  gpu_image *image = nullptr;
  pixel_rect area;
  bool is_layer = false;
};

struct frame_slot {
  com_ptr<ID3D12CommandAllocator> allocator;
  com_ptr<ID3D12DescriptorHeap> descriptors;
  gpu_buffer instances;
  gpu_buffer staging;
  std::size_t staging_used = 0;
  // The descriptor of each texture the frame has used so far. Index 0 holds the instances.
  UINT descriptors_used = 1;
  std::unordered_map<std::uint64_t, UINT> texture_descriptors;
  std::uint64_t fence_value = 0;
};

struct target_constants {
  std::array<float, 2> origin;
  std::array<float, 2> size;
};

class d3d12_backend final : public render_backend {
public:
  d3d12_backend(platform *platform, window_id window) : platform_(platform), window_(window) {}

  d3d12_backend(d3d12_backend const &) = delete;
  auto operator=(d3d12_backend const &) -> d3d12_backend & = delete;

  ~d3d12_backend() override {
    if (queue_ != nullptr) {
      wait_for_gpu();
    }
  }

  [[nodiscard]] auto initialize() -> std::expected<void, render_error> {
    if (validation_requested()) {
      auto debug = com_ptr<ID3D12Debug>{};
      if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
        debug->EnableDebugLayer();
      }
    }
    if (platform_ != nullptr) {
      window_handle_ = static_cast<HWND>(platform_->win32_window_handle(window_));
      if (window_handle_ == nullptr) {
        return std::unexpected(render_error{"the window has no Windows handle"});
      }
    }
    if (auto const created = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory_)); FAILED(created)) {
      return std::unexpected(failure("create a DXGI factory", created));
    }
    auto const adapter = choose_adapter(factory_.Get());
    if (adapter == nullptr) {
      return std::unexpected(render_error{"this computer has no usable Direct3D 12 adapter"});
    }
    if (auto const created =
            D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_));
        FAILED(created)) {
      return std::unexpected(failure("create a device", created));
    }

    auto const queue_description = D3D12_COMMAND_QUEUE_DESC{D3D12_COMMAND_LIST_TYPE_DIRECT, 0,
                                                            D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
    if (auto const created = device_->CreateCommandQueue(&queue_description, IID_PPV_ARGS(&queue_));
        FAILED(created)) {
      return std::unexpected(failure("create a command queue", created));
    }
    if (auto const created = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
        FAILED(created)) {
      return std::unexpected(failure("create a fence", created));
    }
    event_.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (event_ == nullptr) {
      return std::unexpected(render_error{"could not create an event"});
    }
    cbv_srv_increment_ =
        device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    if (auto made = create_pipelines(); !made) {
      return made;
    }
    return create_frame_slots();
  }

  [[nodiscard]] auto kind() const -> backend override { return backend::d3d12; }

  [[nodiscard]] auto draw(frame_request const &request)
      -> std::expected<void, render_error> override {
    atlas_.begin_frame();
    auto const &scene =
        builder_.build(request.commands, request.areas, request.scale, request.clear, atlas_);

    auto &slot = slots_[next_slot_];
    wait_for_value(slot.fence_value);
    release_retired();
    slot.staging_used = 0;
    slot.descriptors_used = 1;
    slot.texture_descriptors.clear();

    if (auto const reset = slot.allocator->Reset(); FAILED(reset)) {
      return std::unexpected(failure("reset a command allocator", reset));
    }
    if (auto const reset = list_->Reset(slot.allocator.Get(), nullptr); FAILED(reset)) {
      return std::unexpected(failure("reset a command list", reset));
    }

    if (auto const uploaded = upload_atlas(slot); !uploaded) {
      return abandon(uploaded);
    }
    auto images = std::vector<std::shared_ptr<gpu_image>>{};
    if (auto uploaded = upload_images(slot, scene)) {
      images = std::move(*uploaded);
    } else {
      return abandon(std::unexpected(std::move(uploaded.error())));
    }
    if (auto const filled = fill_instance_buffer(slot, scene); !filled) {
      return abandon(filled);
    }
    if (auto const ready = ensure_frame(request.width, request.height); !ready) {
      return abandon(ready);
    }
    if (auto const encoded = encode(slot, scene, images, request); !encoded) {
      return abandon(encoded);
    }

    auto *back_buffer = static_cast<ID3D12Resource *>(nullptr);
    if (window_handle_ != nullptr) {
      if (auto const prepared = ensure_swapchain(request.width, request.height); !prepared) {
        return abandon(prepared);
      }
      back_buffer = back_buffers_[swapchain_->GetCurrentBackBufferIndex()].Get();
      copy_to_back_buffer(back_buffer);
    }

    if (auto const closed = list_->Close(); FAILED(closed)) {
      return std::unexpected(failure("finish a command list", closed));
    }
    auto *const lists = static_cast<ID3D12CommandList *>(list_.Get());
    queue_->ExecuteCommandLists(1, &lists);
    slot.fence_value = next_value_++;
    if (auto const signaled = queue_->Signal(fence_.Get(), slot.fence_value); FAILED(signaled)) {
      return std::unexpected(failure("signal a fence", signaled));
    }
    next_slot_ = (next_slot_ + 1) % frames_in_flight;

    if (back_buffer != nullptr) {
      if (auto const presented = swapchain_->Present(1, 0); FAILED(presented)) {
        return std::unexpected(failure("present a frame", presented));
      }
    }
    return {};
  }

  [[nodiscard]] auto read_pixel(int x, int y) const -> std::optional<basalt::rgba> override {
    if (frame_ == nullptr || x < 0 || y < 0 || x >= frame_->width || y >= frame_->height) {
      return std::nullopt;
    }
    auto const pixels = read_pixels(x, y, 1, 1);
    if (pixels.empty()) {
      return std::nullopt;
    }
    return unpremultiply(pixels.front());
  }

  [[nodiscard]] auto read_frame() const -> basalt::raster_image_ptr override {
    if (frame_ == nullptr) {
      return nullptr;
    }
    auto const width = frame_->width;
    auto const height = frame_->height;
    auto const pixels = read_pixels(0, 0, width, height);
    if (pixels.empty()) {
      return nullptr;
    }
    return to_raster_image(pixels, width, height);
  }

private:
  // ---- Setup ----

  auto compile(char const *entry, char const *target)
      -> std::expected<com_ptr<ID3DBlob>, render_error> {
    auto code = com_ptr<ID3DBlob>{};
    auto errors = com_ptr<ID3DBlob>{};
    auto const result = D3DCompile(
        hlsl_source, sizeof(hlsl_source) - 1, "gpu.hlsl", nullptr, nullptr, entry, target,
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(result)) {
      auto message = std::string{"could not compile the Direct3D 12 shaders"};
      if (errors != nullptr) {
        message += ": ";
        message.append(static_cast<char const *>(errors->GetBufferPointer()),
                       errors->GetBufferSize());
      }
      return std::unexpected(render_error{std::move(message)});
    }
    return code;
  }

  auto create_pipelines() -> std::expected<void, render_error> {
    // Parameter 0 holds the target and the first instance, 1 the instances and 2 the texture.
    auto ranges = std::array<D3D12_DESCRIPTOR_RANGE, 2>{};
    for (auto index = UINT{0}; index < ranges.size(); ++index) {
      ranges[index].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
      ranges[index].NumDescriptors = 1;
      ranges[index].BaseShaderRegister = index;
      ranges[index].OffsetInDescriptorsFromTableStart = 0;
    }
    auto parameters = std::array<D3D12_ROOT_PARAMETER, 3>{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = {0, 0, root_constant_count};
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {1, &ranges[0]};
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[2].DescriptorTable = {1, &ranges[1]};
    parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    auto sampler = D3D12_STATIC_SAMPLER_DESC{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    auto signature = D3D12_ROOT_SIGNATURE_DESC{};
    signature.NumParameters = static_cast<UINT>(parameters.size());
    signature.pParameters = parameters.data();
    signature.NumStaticSamplers = 1;
    signature.pStaticSamplers = &sampler;
    auto serialized = com_ptr<ID3DBlob>{};
    auto errors = com_ptr<ID3DBlob>{};
    if (auto const result = D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1_0,
                                                        &serialized, &errors);
        FAILED(result)) {
      return std::unexpected(failure("serialize a root signature", result));
    }
    if (auto const created = device_->CreateRootSignature(0, serialized->GetBufferPointer(),
                                                          serialized->GetBufferSize(),
                                                          IID_PPV_ARGS(&root_signature_));
        FAILED(created)) {
      return std::unexpected(failure("create a root signature", created));
    }

    auto vertex = compile("vertex_main", "vs_5_0");
    auto fragment = compile("fragment_main", "ps_5_0");
    if (!vertex) {
      return std::unexpected(std::move(vertex.error()));
    }
    if (!fragment) {
      return std::unexpected(std::move(fragment.error()));
    }
    for (auto const blending : {true, false}) {
      auto description = D3D12_GRAPHICS_PIPELINE_STATE_DESC{};
      description.pRootSignature = root_signature_.Get();
      description.VS = {(*vertex)->GetBufferPointer(), (*vertex)->GetBufferSize()};
      description.PS = {(*fragment)->GetBufferPointer(), (*fragment)->GetBufferSize()};
      auto &target = description.BlendState.RenderTarget[0];
      target.BlendEnable = blending ? TRUE : FALSE;
      target.SrcBlend = D3D12_BLEND_ONE;
      target.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
      target.BlendOp = D3D12_BLEND_OP_ADD;
      target.SrcBlendAlpha = D3D12_BLEND_ONE;
      target.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
      target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
      target.LogicOp = D3D12_LOGIC_OP_NOOP;
      target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
      description.SampleMask = UINT_MAX;
      description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
      description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
      description.RasterizerState.DepthClipEnable = TRUE;
      description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      description.NumRenderTargets = 1;
      description.RTVFormats[0] = frame_format;
      description.SampleDesc = {1, 0};
      auto &pipeline = blending ? blend_pipeline_ : copy_pipeline_;
      if (auto const created =
              device_->CreateGraphicsPipelineState(&description, IID_PPV_ARGS(&pipeline));
          FAILED(created)) {
        return std::unexpected(failure("create a pipeline", created));
      }
    }
    return {};
  }

  auto create_frame_slots() -> std::expected<void, render_error> {
    for (auto &slot : slots_) {
      if (auto const created = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                               IID_PPV_ARGS(&slot.allocator));
          FAILED(created)) {
        return std::unexpected(failure("create a command allocator", created));
      }
      auto description = D3D12_DESCRIPTOR_HEAP_DESC{};
      description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
      description.NumDescriptors = descriptors_per_frame;
      description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
      if (auto const created =
              device_->CreateDescriptorHeap(&description, IID_PPV_ARGS(&slot.descriptors));
          FAILED(created)) {
        return std::unexpected(failure("create a descriptor heap", created));
      }
    }
    if (auto const created =
            device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slots_[0].allocator.Get(),
                                       nullptr, IID_PPV_ARGS(&list_));
        FAILED(created)) {
      return std::unexpected(failure("create a command list", created));
    }
    list_->Close();

    // Batches without a texture still bind one, so that the shader always has a valid texture.
    // New committed resources start with zeros.
    auto empty = create_texture(1, 1, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_NONE,
                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (!empty) {
      return std::unexpected(std::move(empty.error()));
    }
    empty_ = std::move(*empty);
    return {};
  }

  // ---- Resources ----

  auto create_buffer(std::size_t size, D3D12_HEAP_TYPE heap_type) const
      -> std::expected<gpu_buffer, render_error> {
    auto heap = D3D12_HEAP_PROPERTIES{};
    heap.Type = heap_type;
    auto const description = buffer_description(size);
    auto const initial = heap_type == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST
                                                               : D3D12_RESOURCE_STATE_GENERIC_READ;
    auto result = gpu_buffer{};
    result.size = size;
    if (auto const created =
            device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, initial,
                                             nullptr, IID_PPV_ARGS(&result.resource));
        FAILED(created)) {
      return std::unexpected(failure("create a buffer", created));
    }
    auto const read_range = D3D12_RANGE{0, heap_type == D3D12_HEAP_TYPE_READBACK ? size : 0};
    if (auto const mapped = result.resource->Map(0, &read_range, &result.mapped); FAILED(mapped)) {
      return std::unexpected(failure("map a buffer", mapped));
    }
    return result;
  }

  auto create_texture(int width, int height, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
                      D3D12_RESOURCE_STATES state) const
      -> std::expected<std::shared_ptr<gpu_image>, render_error> {
    auto result = std::make_shared<gpu_image>();
    result->width = width;
    result->height = height;
    result->format = format;
    result->state = state;
    auto heap = D3D12_HEAP_PROPERTIES{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    auto description = D3D12_RESOURCE_DESC{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = static_cast<UINT64>(width);
    description.Height = static_cast<UINT>(height);
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = format;
    description.SampleDesc = {1, 0};
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    description.Flags = flags;
    if (auto const created =
            device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, state,
                                             nullptr, IID_PPV_ARGS(&result->resource));
        FAILED(created)) {
      return std::unexpected(failure("create a texture", created));
    }
    if ((flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0) {
      auto heap_description = D3D12_DESCRIPTOR_HEAP_DESC{};
      heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
      heap_description.NumDescriptors = 1;
      if (auto const created =
              device_->CreateDescriptorHeap(&heap_description, IID_PPV_ARGS(&result->render_view));
          FAILED(created)) {
        return std::unexpected(failure("create a render target view", created));
      }
      device_->CreateRenderTargetView(result->resource.Get(), nullptr,
                                      result->render_view->GetCPUDescriptorHandleForHeapStart());
    }
    return result;
  }

  // Keeps a resource alive until the GPU has finished the frame that is being recorded, which is
  // the latest frame that can still use it.
  auto retire(std::shared_ptr<void> resource) -> void {
    retired_.emplace_back(next_value_, std::move(resource));
  }

  auto release_retired() -> void {
    auto const completed = fence_->GetCompletedValue();
    std::erase_if(retired_, [completed](auto const &entry) { return entry.first <= completed; });
  }

  auto wait_for_value(std::uint64_t value) const -> void {
    if (fence_->GetCompletedValue() < value) {
      fence_->SetEventOnCompletion(value, event_.get());
      WaitForSingleObject(event_.get(), INFINITE);
    }
  }

  auto wait_for_gpu() const -> void {
    auto const value = next_value_++;
    queue_->Signal(fence_.Get(), value);
    wait_for_value(value);
  }

  // Ends a frame that failed before it was submitted. The command list is closed so that the next
  // frame can reset it.
  auto abandon(std::expected<void, render_error> const &failed)
      -> std::expected<void, render_error> {
    list_->Close();
    return std::unexpected(failed.error());
  }

  auto transition(gpu_image &image, D3D12_RESOURCE_STATES to) -> void {
    if (image.state == to) {
      return;
    }
    auto const change = transition_barrier(image.resource.Get(), image.state, to);
    list_->ResourceBarrier(1, &change);
    image.state = to;
  }

  // Space for pixels that the next copy commands read. The old buffer stays until the frame that
  // is being recorded has finished. A new buffer has room for twice what the frame has used so
  // far, so a frame that uploads many images replaces its buffer a few times, not once for each
  // image.
  auto reserve_staging(frame_slot &slot, std::size_t bytes)
      -> std::expected<std::pair<std::size_t, std::byte *>, render_error> {
    auto const offset = align_up(slot.staging_used, placement_alignment);
    if (slot.staging.resource == nullptr || offset + bytes > slot.staging.size) {
      auto bigger = create_buffer(std::max((offset + bytes) * 2, smallest_staging_buffer),
                                  D3D12_HEAP_TYPE_UPLOAD);
      if (!bigger) {
        return std::unexpected(std::move(bigger.error()));
      }
      retire(std::make_shared<gpu_buffer>(std::move(slot.staging)));
      slot.staging = std::move(*bigger);
      slot.staging_used = 0;
      return reserve_staging(slot, bytes);
    }
    slot.staging_used = offset + bytes;
    return std::pair{offset, static_cast<std::byte *>(slot.staging.mapped) + offset};
  }

  // Copies `rows` rows of `row_bytes` bytes each from `pixels` (`source_pitch` bytes apart) to the
  // rectangle of `image` at (x, y), and leaves the image readable by shaders.
  auto upload_rect(frame_slot &slot, gpu_image &image, int x, int y, int width, int height,
                   std::uint8_t const *pixels, std::size_t source_pitch, std::size_t pixel_bytes)
      -> std::expected<void, render_error> {
    auto const row_bytes = static_cast<std::size_t>(width) * pixel_bytes;
    auto const pitch = align_up(row_bytes, row_pitch_alignment);
    auto staged = reserve_staging(slot, pitch * static_cast<std::size_t>(height));
    if (!staged) {
      return std::unexpected(std::move(staged.error()));
    }
    for (auto row = std::size_t{0}; row < static_cast<std::size_t>(height); ++row) {
      std::memcpy(staged->second + row * pitch, pixels + row * source_pitch, row_bytes);
    }
    transition(image, D3D12_RESOURCE_STATE_COPY_DEST);
    auto destination = D3D12_TEXTURE_COPY_LOCATION{};
    destination.pResource = image.resource.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.SubresourceIndex = 0;
    auto source = D3D12_TEXTURE_COPY_LOCATION{};
    source.pResource = slot.staging.resource.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint.Offset = staged->first;
    source.PlacedFootprint.Footprint = {image.format, static_cast<UINT>(width),
                                        static_cast<UINT>(height), 1, static_cast<UINT>(pitch)};
    list_->CopyTextureRegion(&destination, static_cast<UINT>(x), static_cast<UINT>(y), 0, &source,
                             nullptr);
    transition(image, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return {};
  }

  // Copies new atlas pages and the changed parts of old ones to their textures.
  auto upload_atlas(frame_slot &slot) -> std::expected<void, render_error> {
    auto const pages = atlas_.pages();
    auto const size = atlas_.page_size();
    page_images_.resize(pages.size());
    for (auto index = std::size_t{0}; index < pages.size(); ++index) {
      auto const &page = pages[index];
      auto &[id, image] = page_images_[index];
      auto area = page.dirty;
      if (id != page.id || image == nullptr) {
        if (image != nullptr) {
          retire(std::move(image));
        }
        auto created = create_texture(size, size, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_NONE,
                                      D3D12_RESOURCE_STATE_COPY_DEST);
        if (!created) {
          return std::unexpected(std::move(created.error()));
        }
        id = page.id;
        image = std::move(*created);
        area = {0, 0, size, size};
      }
      if (area.width <= 0 || area.height <= 0) {
        continue;
      }
      auto const offset = static_cast<std::size_t>(area.y) * static_cast<std::size_t>(size) +
                          static_cast<std::size_t>(area.x);
      if (auto const uploaded =
              upload_rect(slot, *image, area.x, area.y, area.width, area.height,
                          page.pixels.data() + offset, static_cast<std::size_t>(size), 1);
          !uploaded) {
        return uploaded;
      }
    }
    atlas_.clear_dirty();
    return {};
  }

  // The textures of the scene's images, in scene order. Each image is copied to the GPU once.
  auto upload_images(frame_slot &slot, gpu_scene const &scene)
      -> std::expected<std::vector<std::shared_ptr<gpu_image>>, render_error> {
    return images_.textures_for(
        scene.images, [&](basalt::raster_image const &image) { return upload_image(slot, image); },
        [this](std::shared_ptr<gpu_image> texture) { retire(std::move(texture)); });
  }

  auto upload_image(frame_slot &slot, basalt::raster_image const &source)
      -> std::expected<std::shared_ptr<gpu_image>, render_error> {
    auto const width = static_cast<int>(source.width());
    auto const height = static_cast<int>(source.height());
    auto created = create_texture(width, height, DXGI_FORMAT_R8G8B8A8_UNORM,
                                  D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!created) {
      return std::unexpected(std::move(created.error()));
    }
    auto const texels = premultiplied_rgba(source.pixels());
    if (auto const uploaded = upload_rect(slot, **created, 0, 0, width, height, texels.data(),
                                          static_cast<std::size_t>(width) * 4, 4);
        !uploaded) {
      return std::unexpected(uploaded.error());
    }
    return std::move(*created);
  }

  // Copies the instances into the slot's buffer. The caller has waited for the frame that used
  // it before.
  auto fill_instance_buffer(frame_slot &slot, gpu_scene const &scene)
      -> std::expected<void, render_error> {
    auto const bytes = std::max<std::size_t>(scene.instances.size(), 1) * sizeof(gpu_instance);
    if (slot.instances.resource == nullptr || slot.instances.size < bytes) {
      auto bigger =
          create_buffer(std::max(bytes * 2, smallest_instance_buffer), D3D12_HEAP_TYPE_UPLOAD);
      if (!bigger) {
        return std::unexpected(std::move(bigger.error()));
      }
      slot.instances = std::move(*bigger);
    }
    std::memcpy(slot.instances.mapped, scene.instances.data(),
                scene.instances.size() * sizeof(gpu_instance));

    auto view = D3D12_SHADER_RESOURCE_VIEW_DESC{};
    view.Format = DXGI_FORMAT_UNKNOWN;
    view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Buffer.NumElements = static_cast<UINT>(slot.instances.size / sizeof(gpu_instance));
    view.Buffer.StructureByteStride = sizeof(gpu_instance);
    device_->CreateShaderResourceView(slot.instances.resource.Get(), &view,
                                      slot.descriptors->GetCPUDescriptorHandleForHeapStart());
    return {};
  }

  // The frame keeps its contents between frames. A new size starts it again, and the renderer
  // then sends a full redraw.
  auto ensure_frame(int width, int height) -> std::expected<void, render_error> {
    if (frame_ != nullptr && frame_->width == width && frame_->height == height) {
      return {};
    }
    auto created =
        create_texture(width, height, frame_format, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                       D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (!created) {
      return std::unexpected(
          render_error{"could not create a frame of that size: " + created.error().message});
    }
    if (frame_ != nullptr) {
      retire(std::move(frame_));
    }
    frame_ = std::move(*created);
    return {};
  }

  auto layer_image(std::uint32_t index, pixel_rect area)
      -> std::expected<gpu_image *, render_error> {
    if (layers_.size() <= index) {
      layers_.resize(index + 1);
    }
    auto &layer = layers_[index];
    if (layer == nullptr || layer->width != area.width || layer->height != area.height) {
      auto created = create_texture(area.width, area.height, frame_format,
                                    D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                    D3D12_RESOURCE_STATE_RENDER_TARGET);
      if (!created) {
        return std::unexpected(std::move(created.error()));
      }
      if (layer != nullptr) {
        retire(std::move(layer));
      }
      layer = std::move(*created);
    }
    return layer.get();
  }

  // ---- Drawing ----

  [[nodiscard]] auto descriptor_gpu_handle(frame_slot const &slot, UINT index) const
      -> D3D12_GPU_DESCRIPTOR_HANDLE {
    auto handle = slot.descriptors->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(index) * cbv_srv_increment_;
    return handle;
  }

  auto texture_descriptor(frame_slot &slot, texture_ref texture, gpu_image const &image)
      -> std::expected<D3D12_GPU_DESCRIPTOR_HANDLE, render_error> {
    auto const key = (static_cast<std::uint64_t>(texture.from) << 32) | texture.index;
    if (auto const found = slot.texture_descriptors.find(key);
        found != slot.texture_descriptors.end()) {
      return descriptor_gpu_handle(slot, found->second);
    }
    if (slot.descriptors_used >= descriptors_per_frame) {
      return std::unexpected(render_error{"the frame uses too many textures"});
    }
    auto const index = slot.descriptors_used++;
    auto view = D3D12_SHADER_RESOURCE_VIEW_DESC{};
    view.Format = image.format;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = 1;
    auto cpu = slot.descriptors->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(index) * cbv_srv_increment_;
    device_->CreateShaderResourceView(image.resource.Get(), &view, cpu);
    slot.texture_descriptors.emplace(key, index);
    return descriptor_gpu_handle(slot, index);
  }

  // The Direct3D 12 side of `walk_scene`: records the draws of one frame into the command list.
  class scene_encoder {
  public:
    scene_encoder(d3d12_backend &owner, frame_slot &slot,
                  std::vector<std::shared_ptr<gpu_image>> const &images, std::array<float, 4> clear,
                  bool full_redraw)
        : owner_(owner), list_(owner.list_.Get()), slot_(slot), images_(images), clear_(clear),
          full_redraw_(full_redraw) {}

    auto start(pass_target const &root) -> void {
      set_target(root);
      if (full_redraw_) {
        clear_target(root, clear_);
      }
    }

    auto set_scissor(pass_target const &target, pixel_rect scissor) -> void {
      auto const rect = D3D12_RECT{scissor.x - target.area.x, scissor.y - target.area.y,
                                   scissor.x - target.area.x + scissor.width,
                                   scissor.y - target.area.y + scissor.height};
      list_->RSSetScissorRects(1, &rect);
    }

    auto draw_clear(std::size_t area_index) -> std::expected<void, render_error> {
      list_->SetPipelineState(owner_.copy_pipeline_.Get());
      if (auto const bound = bind_texture({}); !bound) {
        return bound;
      }
      draw_instances(static_cast<std::uint32_t>(area_index), 1);
      list_->SetPipelineState(owner_.blend_pipeline_.Get());
      return {};
    }

    auto draw(draw_batch const &batch) -> std::expected<void, render_error> {
      if (auto const bound = bind_texture(batch.texture); !bound) {
        return bound;
      }
      draw_instances(batch.first, batch.count);
      return {};
    }

    auto begin_layer(pass_target const &, std::uint32_t index, pixel_rect area)
        -> std::expected<pass_target, render_error> {
      auto layer = owner_.layer_image(index, area);
      if (!layer) {
        return std::unexpected(std::move(layer.error()));
      }
      auto const target = pass_target{*layer, area, true};
      owner_.transition(*target.image, D3D12_RESOURCE_STATE_RENDER_TARGET);
      set_target(target);
      clear_target(target, {0.0F, 0.0F, 0.0F, 0.0F});
      return target;
    }

    auto end_layer(pass_target const &layer, pass_target const &parent) -> void {
      owner_.transition(*layer.image, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      set_target(parent);
    }

    auto finish(pass_target const &) -> void {}

  private:
    auto set_target(pass_target const &target) -> void {
      auto const view = target.image->render_view->GetCPUDescriptorHandleForHeapStart();
      list_->OMSetRenderTargets(1, &view, FALSE, nullptr);
      auto const viewport = D3D12_VIEWPORT{
          0.0F, 0.0F, static_cast<float>(target.area.width), static_cast<float>(target.area.height),
          0.0F, 1.0F};
      list_->RSSetViewports(1, &viewport);
      auto const constants = target_constants{
          {static_cast<float>(target.area.x), static_cast<float>(target.area.y)},
          {static_cast<float>(target.area.width), static_cast<float>(target.area.height)}};
      list_->SetGraphicsRoot32BitConstants(0, 4, &constants, 0);
    }

    auto clear_target(pass_target const &target, std::array<float, 4> color) -> void {
      list_->ClearRenderTargetView(target.image->render_view->GetCPUDescriptorHandleForHeapStart(),
                                   color.data(), 0, nullptr);
    }

    auto draw_instances(std::uint32_t first, std::uint32_t count) -> void {
      list_->SetGraphicsRoot32BitConstant(0, first, 4);
      list_->DrawInstanced(4, count, 0, 0);
    }

    auto bind_texture(texture_ref texture) -> std::expected<void, render_error> {
      auto const &image = [&]() -> gpu_image const & {
        switch (texture.from) {
        case texture_ref::source::atlas_page:
          return *owner_.page_images_[texture.index].second;
        case texture_ref::source::image:
          return *images_[texture.index];
        case texture_ref::source::layer:
          return *owner_.layers_[texture.index];
        case texture_ref::source::none:
          break;
        }
        return *owner_.empty_;
      }();
      auto handle = owner_.texture_descriptor(slot_, texture, image);
      if (!handle) {
        return std::unexpected(std::move(handle.error()));
      }
      list_->SetGraphicsRootDescriptorTable(2, *handle);
      return {};
    }

    d3d12_backend &owner_;
    ID3D12GraphicsCommandList *list_;
    frame_slot &slot_;
    std::vector<std::shared_ptr<gpu_image>> const &images_;
    std::array<float, 4> clear_;
    bool full_redraw_;
  };

  auto encode(frame_slot &slot, gpu_scene const &scene,
              std::vector<std::shared_ptr<gpu_image>> const &images, frame_request const &request)
      -> std::expected<void, render_error> {
    auto *const heaps = slot.descriptors.Get();
    list_->SetDescriptorHeaps(1, &heaps);
    list_->SetGraphicsRootSignature(root_signature_.Get());
    list_->SetPipelineState(blend_pipeline_.Get());
    list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    list_->SetGraphicsRootDescriptorTable(1, descriptor_gpu_handle(slot, 0));
    transition(*frame_, D3D12_RESOURCE_STATE_RENDER_TARGET);

    auto encoder =
        scene_encoder{*this, slot, images, premultiplied(request.clear), request.full_redraw};
    return walk_scene(scene, request,
                      pass_target{frame_.get(), {0, 0, request.width, request.height}, false},
                      encoder);
  }

  // ---- Presentation ----

  auto ensure_swapchain(int width, int height) -> std::expected<void, render_error> {
    if (swapchain_ == nullptr) {
      auto description = DXGI_SWAP_CHAIN_DESC1{};
      description.Width = static_cast<UINT>(width);
      description.Height = static_cast<UINT>(height);
      description.Format = frame_format;
      description.SampleDesc = {1, 0};
      description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
      description.BufferCount = swapchain_buffers;
      description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
      description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
      auto created = com_ptr<IDXGISwapChain1>{};
      if (auto const result = factory_->CreateSwapChainForHwnd(
              queue_.Get(), window_handle_, &description, nullptr, nullptr, &created);
          FAILED(result)) {
        return std::unexpected(failure("create a swap chain", result));
      }
      factory_->MakeWindowAssociation(window_handle_, DXGI_MWA_NO_ALT_ENTER);
      if (auto const result = created.As(&swapchain_); FAILED(result)) {
        return std::unexpected(failure("use the swap chain", result));
      }
      swapchain_size_ = {width, height};
    } else if (swapchain_size_ != std::pair{width, height}) {
      // The buffers cannot be resized while the GPU or this object still uses them.
      wait_for_gpu();
      for (auto &buffer : back_buffers_) {
        buffer.Reset();
      }
      if (auto const result = swapchain_->ResizeBuffers(
              0, static_cast<UINT>(width), static_cast<UINT>(height), DXGI_FORMAT_UNKNOWN, 0);
          FAILED(result)) {
        return std::unexpected(failure("resize the swap chain", result));
      }
      swapchain_size_ = {width, height};
    } else {
      return {};
    }
    for (auto index = UINT{0}; index < swapchain_buffers; ++index) {
      if (auto const result = swapchain_->GetBuffer(index, IID_PPV_ARGS(&back_buffers_[index]));
          FAILED(result)) {
        return std::unexpected(failure("get a swap chain buffer", result));
      }
    }
    return {};
  }

  // Copies the frame into the back buffer and leaves both ready for their next use.
  auto copy_to_back_buffer(ID3D12Resource *back_buffer) -> void {
    transition(*frame_, D3D12_RESOURCE_STATE_COPY_SOURCE);
    auto const to_copy = transition_barrier(back_buffer, D3D12_RESOURCE_STATE_PRESENT,
                                            D3D12_RESOURCE_STATE_COPY_DEST);
    list_->ResourceBarrier(1, &to_copy);
    auto destination = D3D12_TEXTURE_COPY_LOCATION{};
    destination.pResource = back_buffer;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    auto source = D3D12_TEXTURE_COPY_LOCATION{};
    source.pResource = frame_->resource.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    auto const box = D3D12_BOX{0,
                               0,
                               0,
                               static_cast<UINT>(std::min(frame_->width, swapchain_size_.first)),
                               static_cast<UINT>(std::min(frame_->height, swapchain_size_.second)),
                               1};
    list_->CopyTextureRegion(&destination, 0, 0, 0, &source, &box);
    auto const to_present = transition_barrier(back_buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                               D3D12_RESOURCE_STATE_PRESENT);
    list_->ResourceBarrier(1, &to_present);
    transition(*frame_, D3D12_RESOURCE_STATE_RENDER_TARGET);
  }

  // Premultiplied BGRA pixels of the frame, row by row. Waits for the GPU. Empty on failure.
  [[nodiscard]] auto read_pixels(int x, int y, int width, int height) const
      -> std::vector<std::uint32_t> {
    wait_for_gpu();
    auto const row_bytes = static_cast<std::size_t>(width) * 4;
    auto const pitch = align_up(row_bytes, row_pitch_alignment);
    auto buffer = create_buffer(pitch * static_cast<std::size_t>(height), D3D12_HEAP_TYPE_READBACK);
    if (!buffer || FAILED(slots_[0].allocator->Reset()) ||
        FAILED(list_->Reset(slots_[0].allocator.Get(), nullptr))) {
      return {};
    }
    auto const to_source =
        transition_barrier(frame_->resource.Get(), frame_->state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list_->ResourceBarrier(1, &to_source);
    auto source = D3D12_TEXTURE_COPY_LOCATION{};
    source.pResource = frame_->resource.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    auto destination = D3D12_TEXTURE_COPY_LOCATION{};
    destination.pResource = buffer->resource.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint.Footprint = {frame_format, static_cast<UINT>(width),
                                             static_cast<UINT>(height), 1,
                                             static_cast<UINT>(pitch)};
    auto const box = D3D12_BOX{static_cast<UINT>(x),         static_cast<UINT>(y),          0,
                               static_cast<UINT>(x + width), static_cast<UINT>(y + height), 1};
    list_->CopyTextureRegion(&destination, 0, 0, 0, &source, &box);
    auto const back =
        transition_barrier(frame_->resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, frame_->state);
    list_->ResourceBarrier(1, &back);
    if (FAILED(list_->Close())) {
      return {};
    }
    auto *const lists = static_cast<ID3D12CommandList *>(list_.Get());
    queue_->ExecuteCommandLists(1, &lists);
    wait_for_gpu();

    auto pixels = std::vector<std::uint32_t>(static_cast<std::size_t>(width) *
                                             static_cast<std::size_t>(height));
    for (auto row = std::size_t{0}; row < static_cast<std::size_t>(height); ++row) {
      std::memcpy(pixels.data() + row * static_cast<std::size_t>(width),
                  static_cast<std::byte const *>(buffer->mapped) + row * pitch, row_bytes);
    }
    return pixels;
  }

  static constexpr auto root_constant_count = UINT{5};

  platform *platform_;
  window_id window_;
  HWND window_handle_ = nullptr;
  com_ptr<IDXGIFactory4> factory_;
  com_ptr<ID3D12Device> device_;
  com_ptr<ID3D12CommandQueue> queue_;
  com_ptr<ID3D12Fence> fence_;
  event_handle event_;
  com_ptr<ID3D12GraphicsCommandList> list_;
  com_ptr<ID3D12RootSignature> root_signature_;
  com_ptr<ID3D12PipelineState> blend_pipeline_;
  com_ptr<ID3D12PipelineState> copy_pipeline_;
  UINT cbv_srv_increment_ = 0;
  std::array<frame_slot, frames_in_flight> slots_;
  std::size_t next_slot_ = 0;
  // The fence value the next submitted frame signals. Resources retire with the value of the frame
  // that is being recorded.
  mutable std::uint64_t next_value_ = 1;
  std::vector<std::pair<std::uint64_t, std::shared_ptr<void>>> retired_;

  com_ptr<IDXGISwapChain3> swapchain_;
  std::array<com_ptr<ID3D12Resource>, swapchain_buffers> back_buffers_;
  std::pair<int, int> swapchain_size_{0, 0};

  std::shared_ptr<gpu_image> frame_;
  std::shared_ptr<gpu_image> empty_;
  glyph_atlas atlas_;
  gpu_scene_builder builder_;
  // By atlas page index: the page's id and its texture.
  std::vector<std::pair<std::uint64_t, std::shared_ptr<gpu_image>>> page_images_;
  image_cache<std::shared_ptr<gpu_image>> images_{image_cache_limit};
  // By scene layer index.
  std::vector<std::shared_ptr<gpu_image>> layers_;
};

} // namespace

auto d3d12_device_available() -> bool {
  auto factory = com_ptr<IDXGIFactory4>{};
  if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
    return false;
  }
  return choose_adapter(factory.Get()) != nullptr;
}

auto make_d3d12_backend(platform *platform, window_id window)
    -> std::expected<std::unique_ptr<render_backend>, render_error> {
  auto backend = std::make_unique<d3d12_backend>(platform, window);
  if (auto ready = backend->initialize(); !ready) {
    return std::unexpected(std::move(ready.error()));
  }
  return backend;
}

} // namespace magma::render
