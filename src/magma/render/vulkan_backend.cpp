#include <volk.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
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

// Compiled from shaders/gpu.vert and shaders/gpu.frag at build time.
#include "gpu_frag_spirv.h"
#include "gpu_vert_spirv.h"

namespace magma::render {
namespace {

// Frames the CPU may record before the GPU has finished the oldest of them.
constexpr auto frames_in_flight = std::size_t{3};
// Images are not evicted one at a time. A cache this large means the caller has moved on through
// many images, so starting over is cheaper than tracking use.
constexpr auto image_cache_limit = std::size_t{64};
constexpr auto smallest_instance_buffer = std::size_t{64} * 1024;
constexpr auto smallest_staging_buffer = std::size_t{1024} * 1024;
constexpr auto texture_sets_per_pool = std::uint32_t{64};
constexpr auto frame_format = VK_FORMAT_B8G8R8A8_UNORM;

struct target_uniforms {
  std::array<float, 2> origin;
  std::array<float, 2> size;
};

auto destroy(VkDevice device, VkImage handle) -> void { vkDestroyImage(device, handle, nullptr); }
auto destroy(VkDevice device, VkImageView handle) -> void {
  vkDestroyImageView(device, handle, nullptr);
}
auto destroy(VkDevice device, VkDeviceMemory handle) -> void {
  vkFreeMemory(device, handle, nullptr);
}
auto destroy(VkDevice device, VkBuffer handle) -> void { vkDestroyBuffer(device, handle, nullptr); }
auto destroy(VkDevice device, VkFramebuffer handle) -> void {
  vkDestroyFramebuffer(device, handle, nullptr);
}
auto destroy(VkDevice device, VkSemaphore handle) -> void {
  vkDestroySemaphore(device, handle, nullptr);
}
auto destroy(VkDevice device, VkFence handle) -> void { vkDestroyFence(device, handle, nullptr); }
auto destroy(VkDevice device, VkRenderPass handle) -> void {
  vkDestroyRenderPass(device, handle, nullptr);
}
auto destroy(VkDevice device, VkPipeline handle) -> void {
  vkDestroyPipeline(device, handle, nullptr);
}
auto destroy(VkDevice device, VkPipelineLayout handle) -> void {
  vkDestroyPipelineLayout(device, handle, nullptr);
}
auto destroy(VkDevice device, VkDescriptorSetLayout handle) -> void {
  vkDestroyDescriptorSetLayout(device, handle, nullptr);
}
auto destroy(VkDevice device, VkDescriptorPool handle) -> void {
  vkDestroyDescriptorPool(device, handle, nullptr);
}
auto destroy(VkDevice device, VkShaderModule handle) -> void {
  vkDestroyShaderModule(device, handle, nullptr);
}
auto destroy(VkDevice device, VkSampler handle) -> void {
  vkDestroySampler(device, handle, nullptr);
}
auto destroy(VkDevice device, VkSwapchainKHR handle) -> void {
  vkDestroySwapchainKHR(device, handle, nullptr);
}
auto destroy(VkDevice device, VkCommandPool handle) -> void {
  vkDestroyCommandPool(device, handle, nullptr);
}

// Owns one Vulkan handle and destroys it when it goes out of scope.
template <class handle> class owned {
public:
  owned() = default;
  owned(handle value, std::function<void(handle)> destroy_handle)
      : value_(value), destroy_(std::move(destroy_handle)) {}
  owned(owned const &) = delete;
  auto operator=(owned const &) -> owned & = delete;
  owned(owned &&other) noexcept
      : value_(std::exchange(other.value_, handle{})), destroy_(std::move(other.destroy_)) {}
  auto operator=(owned &&other) noexcept -> owned & {
    if (this != &other) {
      reset();
      value_ = std::exchange(other.value_, handle{});
      destroy_ = std::move(other.destroy_);
    }
    return *this;
  }
  ~owned() { reset(); }

  auto reset() -> void {
    if (value_ != handle{} && destroy_) {
      destroy_(value_);
    }
    value_ = handle{};
  }
  [[nodiscard]] auto get() const -> handle { return value_; }
  [[nodiscard]] explicit operator bool() const { return value_ != handle{}; }

private:
  handle value_{};
  std::function<void(handle)> destroy_;
};

template <class handle> auto own(VkDevice device, handle value) -> owned<handle> {
  return {value, [device](handle h) { destroy(device, h); }};
}

auto failure(std::string_view what, VkResult result) -> render_error {
  return {"Vulkan could not " + std::string{what} + " (VkResult " +
          std::to_string(static_cast<int>(result)) + ")"};
}

auto software_devices_allowed() -> bool {
  return environment_equals("MAGMA_VULKAN_ALLOW_SOFTWARE", "1");
}

auto validation_requested() -> bool { return environment_equals("MAGMA_VULKAN_VALIDATION", "1"); }

auto has_instance_layer(char const *name) -> bool {
  auto count = std::uint32_t{0};
  if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS) {
    return false;
  }
  auto layers = std::vector<VkLayerProperties>(count);
  if (vkEnumerateInstanceLayerProperties(&count, layers.data()) != VK_SUCCESS) {
    return false;
  }
  return std::ranges::any_of(layers, [name](VkLayerProperties const &layer) {
    return std::string_view{layer.layerName} == name;
  });
}

// Higher is better. Zero means the backend does not accept the device.
auto device_rank(VkPhysicalDeviceProperties const &properties) -> int {
  switch (properties.deviceType) {
  case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
    return 4;
  case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
    return 3;
  case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
    return 2;
  case VK_PHYSICAL_DEVICE_TYPE_CPU:
    return software_devices_allowed() ? 1 : 0;
  default:
    return 0;
  }
}

auto create_instance(std::vector<char const *> const &extensions)
    -> std::expected<VkInstance, render_error> {
  if (auto const loaded = volkInitialize(); loaded != VK_SUCCESS) {
    return std::unexpected(render_error{"this computer has no Vulkan library"});
  }
  auto layers = std::vector<char const *>{};
  if (validation_requested() && has_instance_layer("VK_LAYER_KHRONOS_validation")) {
    layers.push_back("VK_LAYER_KHRONOS_validation");
  }
  auto application = VkApplicationInfo{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                       .pApplicationName = "magma",
                                       .apiVersion = VK_API_VERSION_1_0};
  auto info =
      VkInstanceCreateInfo{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                           .pApplicationInfo = &application,
                           .enabledLayerCount = static_cast<std::uint32_t>(layers.size()),
                           .ppEnabledLayerNames = layers.data(),
                           .enabledExtensionCount = static_cast<std::uint32_t>(extensions.size()),
                           .ppEnabledExtensionNames = extensions.data()};
  auto instance = VkInstance{};
  if (auto const created = vkCreateInstance(&info, nullptr, &instance); created != VK_SUCCESS) {
    return std::unexpected(failure("create an instance", created));
  }
  volkLoadInstance(instance);
  return instance;
}

// The requirements of an image that stays in its layout between draws.
struct layout_scope {
  VkPipelineStageFlags stage;
  VkAccessFlags access;
};

auto scope_of(VkImageLayout layout, bool as_destination) -> layout_scope {
  switch (layout) {
  case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
    return {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
  case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
    return {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT};
  case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
    return {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT};
  case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
    return {VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT};
  default:
    return {static_cast<VkPipelineStageFlags>(as_destination ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT
                                                             : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT),
            0};
  }
}

auto barrier(VkCommandBuffer commands, VkImage image, VkImageLayout from, VkImageLayout to)
    -> void {
  auto const source = scope_of(from, false);
  auto const destination = scope_of(to, true);
  auto const change =
      VkImageMemoryBarrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                           .srcAccessMask = source.access,
                           .dstAccessMask = destination.access,
                           .oldLayout = from,
                           .newLayout = to,
                           .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                           .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                           .image = image,
                           .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
  vkCmdPipelineBarrier(commands, source.stage, destination.stage, 0, 0, nullptr, 0, nullptr, 1,
                       &change);
}

struct gpu_buffer {
  // Declared first, so that it is freed after the buffer.
  owned<VkDeviceMemory> memory;
  owned<VkBuffer> buffer;
  void *mapped = nullptr;
  std::size_t size = 0;
};

// An image with its view and, for render targets, its framebuffer. `layout` is the layout the
// commands recorded so far leave it in.
struct gpu_image {
  owned<VkDeviceMemory> memory;
  owned<VkImage> image;
  owned<VkImageView> view;
  owned<VkFramebuffer> framebuffer;
  int width = 0;
  int height = 0;
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

// One render target of a frame: the frame itself or a layer, and the frame device pixels it covers.
struct pass_target {
  gpu_image *image = nullptr;
  pixel_rect area;
  bool is_layer = false;
};

struct frame_slot {
  VkCommandBuffer commands = VK_NULL_HANDLE;
  owned<VkFence> fence;
  owned<VkSemaphore> acquired;
  gpu_buffer instances;
  gpu_buffer staging;
  std::size_t staging_used = 0;
  VkDescriptorSet instance_set = VK_NULL_HANDLE;
  std::vector<owned<VkDescriptorPool>> pools;
  std::size_t pool_index = 0;
  std::unordered_map<std::uint64_t, VkDescriptorSet> texture_sets;
  std::uint64_t serial = 0;
};

struct swapchain_state {
  owned<VkSwapchainKHR> handle;
  std::vector<VkImage> images;
  std::vector<owned<VkSemaphore>> finished;
  VkExtent2D extent{};
};

class vulkan_backend final : public render_backend {
public:
  vulkan_backend(platform *platform, window_id window) : platform_(platform), window_(window) {}

  vulkan_backend(vulkan_backend const &) = delete;
  auto operator=(vulkan_backend const &) -> vulkan_backend & = delete;

  ~vulkan_backend() override {
    if (device_ != VK_NULL_HANDLE) {
      vkDeviceWaitIdle(device_);
    }
    // The members that hold device objects go before the device, the surface before the instance.
    retired_.clear();
    swapchain_ = {};
    slots_ = {};
    instance_pool_.reset();
    frame_.reset();
    layers_.clear();
    page_images_.clear();
    images_.clear();
    empty_.reset();
    pipelines_ = {};
    command_pool_.reset();
    if (device_ != VK_NULL_HANDLE) {
      vkDestroyDevice(device_, nullptr);
    }
    if (surface_ != 0 && platform_ != nullptr) {
      platform_->destroy_vulkan_surface(instance_, surface_);
    }
    if (instance_ != VK_NULL_HANDLE) {
      vkDestroyInstance(instance_, nullptr);
    }
  }

  [[nodiscard]] auto initialize() -> std::expected<void, render_error> {
    auto extensions = std::vector<char const *>{};
    auto extension_names = std::vector<std::string>{};
    if (platform_ != nullptr) {
      extension_names = platform_->vulkan_instance_extensions(window_);
      if (extension_names.empty()) {
        return std::unexpected(render_error{"the window was not created for Vulkan"});
      }
      for (auto const &name : extension_names) {
        extensions.push_back(name.c_str());
      }
    }
    auto instance = create_instance(extensions);
    if (!instance) {
      return std::unexpected(std::move(instance.error()));
    }
    instance_ = *instance;

    if (platform_ != nullptr) {
      auto surface = platform_->create_vulkan_surface(window_, instance_);
      if (!surface) {
        return std::unexpected(render_error{std::move(surface.error().message)});
      }
      surface_ = *surface;
    }
    if (auto chosen = choose_device(); !chosen) {
      return chosen;
    }
    if (auto made = create_device(); !made) {
      return made;
    }
    if (auto made = create_pipelines(); !made) {
      return made;
    }
    return create_frame_slots();
  }

  [[nodiscard]] auto kind() const -> backend override { return backend::vulkan; }

  [[nodiscard]] auto draw(frame_request const &request)
      -> std::expected<void, render_error> override {
    atlas_.begin_frame();
    auto const &scene =
        builder_.build(request.commands, request.areas, request.scale, request.clear, atlas_);

    auto &slot = slots_[next_slot_];
    auto const previous_frame = slot.fence.get();
    if (auto const waited = vkWaitForFences(device_, 1, &previous_frame, VK_TRUE, UINT64_MAX);
        waited != VK_SUCCESS) {
      return std::unexpected(failure("wait for the previous frame", waited));
    }
    completed_ = std::max(completed_, slot.serial);
    release_retired();
    slot.staging_used = 0;
    slot.texture_sets.clear();
    slot.pool_index = 0;
    for (auto const &pool : slot.pools) {
      vkResetDescriptorPool(device_, pool.get(), 0);
    }

    if (auto const begun = begin_commands(slot.commands); !begun) {
      return begun;
    }
    auto const commands = slot.commands;
    if (auto const uploaded = upload_atlas(slot, commands); !uploaded) {
      return uploaded;
    }
    auto images = std::vector<std::shared_ptr<gpu_image>>{};
    if (auto uploaded = upload_images(slot, commands, scene)) {
      images = std::move(*uploaded);
    } else {
      return std::unexpected(std::move(uploaded.error()));
    }
    if (auto const filled = fill_instance_buffer(slot, scene); !filled) {
      return filled;
    }
    if (auto const ready = ensure_frame(request.width, request.height); !ready) {
      return ready;
    }

    if (auto const encoded = encode(slot, scene, images, request); !encoded) {
      return encoded;
    }

    auto image_index = std::uint32_t{0};
    if (surface_ != 0) {
      if (auto const acquired = acquire(slot, request.width, request.height)) {
        image_index = *acquired;
      } else {
        return std::unexpected(acquired.error());
      }
      copy_to_swapchain(commands, image_index);
    }
    if (auto const ended = vkEndCommandBuffer(commands); ended != VK_SUCCESS) {
      return std::unexpected(failure("finish a command buffer", ended));
    }
    return submit_and_present(slot, image_index);
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

  auto choose_device() -> std::expected<void, render_error> {
    auto count = std::uint32_t{0};
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    auto devices = std::vector<VkPhysicalDevice>(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());

    auto best_rank = 0;
    for (auto const candidate : devices) {
      auto properties = VkPhysicalDeviceProperties{};
      vkGetPhysicalDeviceProperties(candidate, &properties);
      auto const rank = device_rank(properties);
      if (rank <= best_rank) {
        continue;
      }
      auto const family = find_queue_family(candidate);
      if (!family || (surface_ != 0 && !supports_swapchain(candidate))) {
        continue;
      }
      best_rank = rank;
      physical_ = candidate;
      family_ = *family;
    }
    if (physical_ == VK_NULL_HANDLE) {
      return std::unexpected(render_error{"this computer has no usable Vulkan device"});
    }
    return {};
  }

  [[nodiscard]] auto find_queue_family(VkPhysicalDevice candidate) const
      -> std::optional<std::uint32_t> {
    auto count = std::uint32_t{0};
    vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, nullptr);
    auto families = std::vector<VkQueueFamilyProperties>(count);
    vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, families.data());
    for (auto index = std::uint32_t{0}; index < count; ++index) {
      if ((families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) {
        continue;
      }
      if (surface_ != 0) {
        auto supported = VkBool32{VK_FALSE};
        vkGetPhysicalDeviceSurfaceSupportKHR(candidate, index, surface_handle(), &supported);
        if (supported == VK_FALSE) {
          continue;
        }
      }
      return index;
    }
    return std::nullopt;
  }

  [[nodiscard]] auto supports_swapchain(VkPhysicalDevice candidate) const -> bool {
    auto count = std::uint32_t{0};
    vkEnumerateDeviceExtensionProperties(candidate, nullptr, &count, nullptr);
    auto extensions = std::vector<VkExtensionProperties>(count);
    vkEnumerateDeviceExtensionProperties(candidate, nullptr, &count, extensions.data());
    return std::ranges::any_of(extensions, [](VkExtensionProperties const &extension) {
      return std::string_view{extension.extensionName} == VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    });
  }

  auto create_device() -> std::expected<void, render_error> {
    vkGetPhysicalDeviceMemoryProperties(physical_, &memory_);
    auto const priority = 1.0F;
    auto queue_info = VkDeviceQueueCreateInfo{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                              .queueFamilyIndex = family_,
                                              .queueCount = 1,
                                              .pQueuePriorities = &priority};
    auto extensions = std::vector<char const *>{};
    if (surface_ != 0) {
      extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }
    auto info =
        VkDeviceCreateInfo{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                           .queueCreateInfoCount = 1,
                           .pQueueCreateInfos = &queue_info,
                           .enabledExtensionCount = static_cast<std::uint32_t>(extensions.size()),
                           .ppEnabledExtensionNames = extensions.data()};
    if (auto const created = vkCreateDevice(physical_, &info, nullptr, &device_);
        created != VK_SUCCESS) {
      return std::unexpected(failure("create a device", created));
    }
    volkLoadDevice(device_);
    vkGetDeviceQueue(device_, family_, 0, &queue_);

    auto pool_info =
        VkCommandPoolCreateInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                .queueFamilyIndex = family_};
    auto pool = VkCommandPool{};
    if (auto const created = vkCreateCommandPool(device_, &pool_info, nullptr, &pool);
        created != VK_SUCCESS) {
      return std::unexpected(failure("create a command pool", created));
    }
    command_pool_ = own(device_, pool);
    return {};
  }

  auto create_render_pass(VkAttachmentLoadOp load, VkImageLayout initial,
                          VkImageLayout final_layout)
      -> std::expected<owned<VkRenderPass>, render_error> {
    auto const attachment =
        VkAttachmentDescription{.format = frame_format,
                                .samples = VK_SAMPLE_COUNT_1_BIT,
                                .loadOp = load,
                                .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                .initialLayout = initial,
                                .finalLayout = final_layout};
    auto const reference = VkAttachmentReference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    auto const subpass = VkSubpassDescription{.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                              .colorAttachmentCount = 1,
                                              .pColorAttachments = &reference};
    // A target is sampled after a pass that drew into it, and drawn into again after it was
    // sampled.
    auto const stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    auto const accesses = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT |
                          VK_ACCESS_TRANSFER_READ_BIT;
    auto const dependencies = std::array{
        VkSubpassDependency{
            VK_SUBPASS_EXTERNAL, 0, stages, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, accesses,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0},
        VkSubpassDependency{0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                            stages, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, accesses, 0}};
    auto const info =
        VkRenderPassCreateInfo{.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                               .attachmentCount = 1,
                               .pAttachments = &attachment,
                               .subpassCount = 1,
                               .pSubpasses = &subpass,
                               .dependencyCount = static_cast<std::uint32_t>(dependencies.size()),
                               .pDependencies = dependencies.data()};
    auto pass = VkRenderPass{};
    if (auto const created = vkCreateRenderPass(device_, &info, nullptr, &pass);
        created != VK_SUCCESS) {
      return std::unexpected(failure("create a render pass", created));
    }
    return own(device_, pass);
  }

  auto create_shader(std::span<std::uint32_t const> code)
      -> std::expected<owned<VkShaderModule>, render_error> {
    auto const info = VkShaderModuleCreateInfo{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                               .codeSize = code.size_bytes(),
                                               .pCode = code.data()};
    auto module = VkShaderModule{};
    if (auto const created = vkCreateShaderModule(device_, &info, nullptr, &module);
        created != VK_SUCCESS) {
      return std::unexpected(failure("create a shader", created));
    }
    return own(device_, module);
  }

  auto create_pipelines() -> std::expected<void, render_error> {
    constexpr auto color = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    constexpr auto readable = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    auto passes = std::array{
        create_render_pass(VK_ATTACHMENT_LOAD_OP_CLEAR, color, color),
        create_render_pass(VK_ATTACHMENT_LOAD_OP_LOAD, color, color),
        create_render_pass(VK_ATTACHMENT_LOAD_OP_CLEAR, VK_IMAGE_LAYOUT_UNDEFINED, readable),
        create_render_pass(VK_ATTACHMENT_LOAD_OP_LOAD, readable, readable)};
    for (auto &pass : passes) {
      if (!pass) {
        return std::unexpected(std::move(pass.error()));
      }
    }
    pipelines_.root_clear = std::move(*passes[0]);
    pipelines_.root_load = std::move(*passes[1]);
    pipelines_.layer_begin = std::move(*passes[2]);
    pipelines_.layer_resume = std::move(*passes[3]);

    auto sampling = VkSamplerCreateInfo{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                        .magFilter = VK_FILTER_LINEAR,
                                        .minFilter = VK_FILTER_LINEAR,
                                        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                        .maxLod = 0.25F};
    auto sampler = VkSampler{};
    if (auto const created = vkCreateSampler(device_, &sampling, nullptr, &sampler);
        created != VK_SUCCESS) {
      return std::unexpected(failure("create a sampler", created));
    }
    pipelines_.sampler = own(device_, sampler);

    // Set 0 holds the instances. Set 1 holds the texture a batch samples, with a fixed sampler.
    auto const both_stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    auto const instance_binding =
        VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, both_stages, nullptr};
    auto const immutable_sampler = pipelines_.sampler.get();
    auto const texture_binding =
        VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                     VK_SHADER_STAGE_FRAGMENT_BIT, &immutable_sampler};
    auto set_layouts = std::array<VkDescriptorSetLayout, 2>{};
    auto const bindings = std::array{instance_binding, texture_binding};
    for (auto index = std::size_t{0}; index < set_layouts.size(); ++index) {
      auto const info = VkDescriptorSetLayoutCreateInfo{
          .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
          .bindingCount = 1,
          .pBindings = &bindings[index]};
      if (auto const created =
              vkCreateDescriptorSetLayout(device_, &info, nullptr, &set_layouts[index]);
          created != VK_SUCCESS) {
        return std::unexpected(failure("create a descriptor set layout", created));
      }
    }
    pipelines_.instance_layout = own(device_, set_layouts[0]);
    pipelines_.texture_layout = own(device_, set_layouts[1]);

    auto const push_range = VkPushConstantRange{both_stages, 0, sizeof(target_uniforms)};
    auto const layout_info =
        VkPipelineLayoutCreateInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                   .setLayoutCount = 2,
                                   .pSetLayouts = set_layouts.data(),
                                   .pushConstantRangeCount = 1,
                                   .pPushConstantRanges = &push_range};
    auto layout = VkPipelineLayout{};
    if (auto const created = vkCreatePipelineLayout(device_, &layout_info, nullptr, &layout);
        created != VK_SUCCESS) {
      return std::unexpected(failure("create a pipeline layout", created));
    }
    pipelines_.layout = own(device_, layout);

    auto vertex = create_shader(vertex_spirv);
    auto fragment = create_shader(fragment_spirv);
    if (!vertex) {
      return std::unexpected(std::move(vertex.error()));
    }
    if (!fragment) {
      return std::unexpected(std::move(fragment.error()));
    }
    auto blend = create_pipeline(vertex->get(), fragment->get(), true);
    auto copy = create_pipeline(vertex->get(), fragment->get(), false);
    if (!blend) {
      return std::unexpected(std::move(blend.error()));
    }
    if (!copy) {
      return std::unexpected(std::move(copy.error()));
    }
    pipelines_.blend = std::move(*blend);
    pipelines_.copy = std::move(*copy);
    return {};
  }

  auto create_pipeline(VkShaderModule vertex, VkShaderModule fragment, bool blending)
      -> std::expected<owned<VkPipeline>, render_error> {
    auto const stages = std::array{VkPipelineShaderStageCreateInfo{
                                       .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                       .stage = VK_SHADER_STAGE_VERTEX_BIT,
                                       .module = vertex,
                                       .pName = "main"},
                                   VkPipelineShaderStageCreateInfo{
                                       .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                       .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                                       .module = fragment,
                                       .pName = "main"}};
    auto const vertex_input = VkPipelineVertexInputStateCreateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    auto const assembly = VkPipelineInputAssemblyStateCreateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP};
    auto const viewport = VkPipelineViewportStateCreateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1};
    auto const raster = VkPipelineRasterizationStateCreateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0F};
    auto const multisample = VkPipelineMultisampleStateCreateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    auto const attachment = VkPipelineColorBlendAttachmentState{
        .blendEnable = blending ? VK_TRUE : VK_FALSE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
    auto const blend_state = VkPipelineColorBlendStateCreateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &attachment};
    auto const dynamic_states = std::array{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    auto const dynamic = VkPipelineDynamicStateCreateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = static_cast<std::uint32_t>(dynamic_states.size()),
        .pDynamicStates = dynamic_states.data()};
    auto const info = VkGraphicsPipelineCreateInfo{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = static_cast<std::uint32_t>(stages.size()),
        .pStages = stages.data(),
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &assembly,
        .pViewportState = &viewport,
        .pRasterizationState = &raster,
        .pMultisampleState = &multisample,
        .pColorBlendState = &blend_state,
        .pDynamicState = &dynamic,
        .layout = pipelines_.layout.get(),
        // Every pass has the same attachment, so the pipelines work with all of them.
        .renderPass = pipelines_.root_load.get(),
        .subpass = 0};
    auto pipeline = VkPipeline{};
    if (auto const created =
            vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
        created != VK_SUCCESS) {
      return std::unexpected(failure("create a pipeline", created));
    }
    return own(device_, pipeline);
  }

  auto create_frame_slots() -> std::expected<void, render_error> {
    auto buffers = std::array<VkCommandBuffer, frames_in_flight>{};
    auto const allocate = VkCommandBufferAllocateInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = command_pool_.get(),
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = static_cast<std::uint32_t>(buffers.size())};
    if (auto const allocated = vkAllocateCommandBuffers(device_, &allocate, buffers.data());
        allocated != VK_SUCCESS) {
      return std::unexpected(failure("allocate command buffers", allocated));
    }

    auto const pool_size = VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                static_cast<std::uint32_t>(buffers.size())};
    auto const pool_info =
        VkDescriptorPoolCreateInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                   .maxSets = static_cast<std::uint32_t>(buffers.size()),
                                   .poolSizeCount = 1,
                                   .pPoolSizes = &pool_size};
    auto pool = VkDescriptorPool{};
    if (auto const created = vkCreateDescriptorPool(device_, &pool_info, nullptr, &pool);
        created != VK_SUCCESS) {
      return std::unexpected(failure("create a descriptor pool", created));
    }
    instance_pool_ = own(device_, pool);

    for (auto index = std::size_t{0}; index < slots_.size(); ++index) {
      auto &slot = slots_[index];
      slot.commands = buffers[index];

      auto const fence_info = VkFenceCreateInfo{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                                                .flags = VK_FENCE_CREATE_SIGNALED_BIT};
      auto fence = VkFence{};
      if (auto const created = vkCreateFence(device_, &fence_info, nullptr, &fence);
          created != VK_SUCCESS) {
        return std::unexpected(failure("create a fence", created));
      }
      slot.fence = own(device_, fence);

      auto acquired = create_semaphore();
      if (!acquired) {
        return std::unexpected(std::move(acquired.error()));
      }
      slot.acquired = std::move(*acquired);

      auto const layout = pipelines_.instance_layout.get();
      auto const set_info =
          VkDescriptorSetAllocateInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                      .descriptorPool = instance_pool_.get(),
                                      .descriptorSetCount = 1,
                                      .pSetLayouts = &layout};
      if (auto const allocated = vkAllocateDescriptorSets(device_, &set_info, &slot.instance_set);
          allocated != VK_SUCCESS) {
        return std::unexpected(failure("allocate a descriptor set", allocated));
      }
    }

    // Batches without a texture still bind one, so that the shader always has a valid texture.
    auto empty = create_image(1, 1, VK_FORMAT_R8_UNORM,
                              VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, false);
    if (!empty) {
      return std::unexpected(std::move(empty.error()));
    }
    empty_ = std::move(*empty);
    return submit_once([this](VkCommandBuffer commands) {
      barrier(commands, empty_->image.get(), VK_IMAGE_LAYOUT_UNDEFINED,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
      auto const zero = VkClearColorValue{};
      auto const range = VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      vkCmdClearColorImage(commands, empty_->image.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           &zero, 1, &range);
      barrier(commands, empty_->image.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
      empty_->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    });
  }

  // ---- Resources ----

  auto create_semaphore() -> std::expected<owned<VkSemaphore>, render_error> {
    auto const info = VkSemaphoreCreateInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    auto semaphore = VkSemaphore{};
    if (auto const created = vkCreateSemaphore(device_, &info, nullptr, &semaphore);
        created != VK_SUCCESS) {
      return std::unexpected(failure("create a semaphore", created));
    }
    return own(device_, semaphore);
  }

  [[nodiscard]] auto find_memory_type(std::uint32_t allowed, VkMemoryPropertyFlags wanted) const
      -> std::optional<std::uint32_t> {
    for (auto index = std::uint32_t{0}; index < memory_.memoryTypeCount; ++index) {
      if ((allowed & (1U << index)) != 0 &&
          (memory_.memoryTypes[index].propertyFlags & wanted) == wanted) {
        return index;
      }
    }
    return std::nullopt;
  }

  auto allocate_memory(VkMemoryRequirements const &requirements, VkMemoryPropertyFlags wanted) const
      -> std::expected<owned<VkDeviceMemory>, render_error> {
    auto const type = find_memory_type(requirements.memoryTypeBits, wanted);
    if (!type) {
      return std::unexpected(render_error{"Vulkan found no memory for a resource"});
    }
    auto const info = VkMemoryAllocateInfo{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                           .allocationSize = requirements.size,
                                           .memoryTypeIndex = *type};
    auto memory = VkDeviceMemory{};
    if (auto const allocated = vkAllocateMemory(device_, &info, nullptr, &memory);
        allocated != VK_SUCCESS) {
      return std::unexpected(failure("allocate memory", allocated));
    }
    return own(device_, memory);
  }

  // Memory that the CPU writes and the GPU reads, or the GPU writes and the CPU reads.
  auto create_buffer(std::size_t size, VkBufferUsageFlags usage) const
      -> std::expected<gpu_buffer, render_error> {
    auto const info = VkBufferCreateInfo{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                         .size = size,
                                         .usage = usage,
                                         .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    auto handle = VkBuffer{};
    if (auto const created = vkCreateBuffer(device_, &info, nullptr, &handle);
        created != VK_SUCCESS) {
      return std::unexpected(failure("create a buffer", created));
    }
    auto result = gpu_buffer{};
    result.buffer = own(device_, handle);
    result.size = size;
    auto requirements = VkMemoryRequirements{};
    vkGetBufferMemoryRequirements(device_, handle, &requirements);
    auto memory = allocate_memory(requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!memory) {
      return std::unexpected(std::move(memory.error()));
    }
    result.memory = std::move(*memory);
    if (auto const bound = vkBindBufferMemory(device_, handle, result.memory.get(), 0);
        bound != VK_SUCCESS) {
      return std::unexpected(failure("bind buffer memory", bound));
    }
    if (auto const mapped =
            vkMapMemory(device_, result.memory.get(), 0, VK_WHOLE_SIZE, 0, &result.mapped);
        mapped != VK_SUCCESS) {
      return std::unexpected(failure("map a buffer", mapped));
    }
    return result;
  }

  auto create_image(int width, int height, VkFormat format, VkImageUsageFlags usage,
                    bool render_target) -> std::expected<std::shared_ptr<gpu_image>, render_error> {
    auto result = std::make_shared<gpu_image>();
    result->width = width;
    result->height = height;
    auto const info = VkImageCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    auto handle = VkImage{};
    if (auto const created = vkCreateImage(device_, &info, nullptr, &handle);
        created != VK_SUCCESS) {
      return std::unexpected(failure("create an image", created));
    }
    result->image = own(device_, handle);
    auto requirements = VkMemoryRequirements{};
    vkGetImageMemoryRequirements(device_, handle, &requirements);
    auto memory = allocate_memory(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!memory) {
      memory = allocate_memory(requirements, 0);
    }
    if (!memory) {
      return std::unexpected(std::move(memory.error()));
    }
    result->memory = std::move(*memory);
    if (auto const bound = vkBindImageMemory(device_, handle, result->memory.get(), 0);
        bound != VK_SUCCESS) {
      return std::unexpected(failure("bind image memory", bound));
    }

    auto const view_info =
        VkImageViewCreateInfo{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                              .image = handle,
                              .viewType = VK_IMAGE_VIEW_TYPE_2D,
                              .format = format,
                              .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    auto view = VkImageView{};
    if (auto const created = vkCreateImageView(device_, &view_info, nullptr, &view);
        created != VK_SUCCESS) {
      return std::unexpected(failure("create an image view", created));
    }
    result->view = own(device_, view);

    if (render_target) {
      auto const attachment = result->view.get();
      auto const buffer_info =
          VkFramebufferCreateInfo{.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                  .renderPass = pipelines_.root_load.get(),
                                  .attachmentCount = 1,
                                  .pAttachments = &attachment,
                                  .width = static_cast<std::uint32_t>(width),
                                  .height = static_cast<std::uint32_t>(height),
                                  .layers = 1};
      auto framebuffer = VkFramebuffer{};
      if (auto const created = vkCreateFramebuffer(device_, &buffer_info, nullptr, &framebuffer);
          created != VK_SUCCESS) {
        return std::unexpected(failure("create a framebuffer", created));
      }
      result->framebuffer = own(device_, framebuffer);
    }
    return result;
  }

  // Keeps a resource alive until the GPU has finished the frame that is being recorded, which is
  // the latest frame that can still use it.
  auto retire(std::shared_ptr<void> resource) -> void {
    retired_.emplace_back(next_serial_, std::move(resource));
  }

  auto release_retired() -> void {
    std::erase_if(retired_, [this](auto const &entry) { return entry.first <= completed_; });
  }

  template <class record>
  auto submit_once(record &&recorder) const -> std::expected<void, render_error> {
    auto const allocate =
        VkCommandBufferAllocateInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                    .commandPool = command_pool_.get(),
                                    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                    .commandBufferCount = 1};
    auto commands = VkCommandBuffer{};
    if (auto const allocated = vkAllocateCommandBuffers(device_, &allocate, &commands);
        allocated != VK_SUCCESS) {
      return std::unexpected(failure("allocate a command buffer", allocated));
    }
    auto result = std::expected<void, render_error>{};
    auto const begin =
        VkCommandBufferBeginInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                 .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(commands, &begin);
    recorder(commands);
    vkEndCommandBuffer(commands);

    auto const fence_info = VkFenceCreateInfo{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    auto fence = VkFence{};
    if (auto const created = vkCreateFence(device_, &fence_info, nullptr, &fence);
        created != VK_SUCCESS) {
      result = std::unexpected(failure("create a fence", created));
    } else {
      auto const submit = VkSubmitInfo{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                       .commandBufferCount = 1,
                                       .pCommandBuffers = &commands};
      if (auto const submitted = vkQueueSubmit(queue_, 1, &submit, fence);
          submitted != VK_SUCCESS) {
        result = std::unexpected(failure("submit commands", submitted));
      } else {
        vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX);
      }
      vkDestroyFence(device_, fence, nullptr);
    }
    vkFreeCommandBuffers(device_, command_pool_.get(), 1, &commands);
    return result;
  }

  auto begin_commands(VkCommandBuffer commands) -> std::expected<void, render_error> {
    vkResetCommandBuffer(commands, 0);
    auto const begin =
        VkCommandBufferBeginInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                 .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if (auto const begun = vkBeginCommandBuffer(commands, &begin); begun != VK_SUCCESS) {
      return std::unexpected(failure("start a command buffer", begun));
    }
    return {};
  }

  // Space for pixels that the next copy commands read. The old buffer stays until the frame that
  // is being recorded has finished.
  auto reserve_staging(frame_slot &slot, std::size_t bytes)
      -> std::expected<std::pair<std::size_t, std::byte *>, render_error> {
    constexpr auto alignment = std::size_t{16};
    auto const offset = (slot.staging_used + alignment - 1) / alignment * alignment;
    if (!slot.staging.buffer || offset + bytes > slot.staging.size) {
      auto bigger = create_buffer(std::max(bytes * 2, smallest_staging_buffer),
                                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
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

  // Copies new atlas pages and the changed parts of old ones to their images.
  auto upload_atlas(frame_slot &slot, VkCommandBuffer commands)
      -> std::expected<void, render_error> {
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
        auto created =
            create_image(size, size, VK_FORMAT_R8_UNORM,
                         VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, false);
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
      auto const bytes =
          static_cast<std::size_t>(area.width) * static_cast<std::size_t>(area.height);
      auto staged = reserve_staging(slot, bytes);
      if (!staged) {
        return std::unexpected(std::move(staged.error()));
      }
      for (auto row = 0; row < area.height; ++row) {
        auto const source =
            static_cast<std::size_t>(area.y + row) * static_cast<std::size_t>(size) +
            static_cast<std::size_t>(area.x);
        std::memcpy(staged->second +
                        static_cast<std::size_t>(row) * static_cast<std::size_t>(area.width),
                    page.pixels.data() + source, static_cast<std::size_t>(area.width));
      }
      copy_to_image(commands, slot.staging.buffer.get(), staged->first, *image, area.x, area.y,
                    area.width, area.height, area.width);
    }
    atlas_.clear_dirty();
    return {};
  }

  // Copies tightly packed pixels from a buffer into a rectangle of an image and leaves the image
  // readable by shaders.
  auto copy_to_image(VkCommandBuffer commands, VkBuffer buffer, std::size_t offset,
                     gpu_image &image, int x, int y, int width, int height, int row_pixels)
      -> void {
    barrier(commands, image.image.get(), image.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    auto const region = VkBufferImageCopy{
        .bufferOffset = offset,
        .bufferRowLength = static_cast<std::uint32_t>(row_pixels),
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageOffset = {x, y, 0},
        .imageExtent = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), 1}};
    vkCmdCopyBufferToImage(commands, buffer, image.image.get(),
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    barrier(commands, image.image.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    image.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  }

  // The images of the scene, in scene order. Each image is copied to the GPU once.
  auto upload_images(frame_slot &slot, VkCommandBuffer commands, gpu_scene const &scene)
      -> std::expected<std::vector<std::shared_ptr<gpu_image>>, render_error> {
    return images_.textures_for(
        scene.images,
        [&](basalt::raster_image const &image) { return upload_image(slot, commands, image); },
        [this](std::shared_ptr<gpu_image> texture) { retire(std::move(texture)); });
  }

  auto upload_image(frame_slot &slot, VkCommandBuffer commands, basalt::raster_image const &source)
      -> std::expected<std::shared_ptr<gpu_image>, render_error> {
    auto const width = static_cast<int>(source.width());
    auto const height = static_cast<int>(source.height());
    auto created =
        create_image(width, height, VK_FORMAT_R8G8B8A8_UNORM,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, false);
    if (!created) {
      return std::unexpected(std::move(created.error()));
    }
    auto const texels = premultiplied_rgba(source.pixels());
    auto staged = reserve_staging(slot, texels.size());
    if (!staged) {
      return std::unexpected(std::move(staged.error()));
    }
    std::memcpy(staged->second, texels.data(), texels.size());
    copy_to_image(commands, slot.staging.buffer.get(), staged->first, **created, 0, 0, width,
                  height, width);
    return std::move(*created);
  }

  // Copies the instances into the slot's buffer. The caller has waited for the frame that used
  // it before.
  auto fill_instance_buffer(frame_slot &slot, gpu_scene const &scene)
      -> std::expected<void, render_error> {
    auto const bytes = std::max<std::size_t>(scene.instances.size(), 1) * sizeof(gpu_instance);
    if (!slot.instances.buffer || slot.instances.size < bytes) {
      auto bigger = create_buffer(std::max(bytes * 2, smallest_instance_buffer),
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
      if (!bigger) {
        return std::unexpected(std::move(bigger.error()));
      }
      slot.instances = std::move(*bigger);
      auto const buffer_info =
          VkDescriptorBufferInfo{slot.instances.buffer.get(), 0, VK_WHOLE_SIZE};
      auto const write = VkWriteDescriptorSet{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                              .dstSet = slot.instance_set,
                                              .dstBinding = 0,
                                              .descriptorCount = 1,
                                              .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                              .pBufferInfo = &buffer_info};
      vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    }
    std::memcpy(slot.instances.mapped, scene.instances.data(),
                scene.instances.size() * sizeof(gpu_instance));
    return {};
  }

  // The frame keeps its contents between frames. A new size starts it again, and the renderer
  // then sends a full redraw.
  auto ensure_frame(int width, int height) -> std::expected<void, render_error> {
    if (frame_ != nullptr && frame_->width == width && frame_->height == height) {
      return {};
    }
    auto created =
        create_image(width, height, frame_format,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, true);
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
      auto created =
          create_image(area.width, area.height, frame_format,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, true);
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

  auto texture_set(frame_slot &slot, texture_ref texture, gpu_image const &image)
      -> std::expected<VkDescriptorSet, render_error> {
    auto const key = (static_cast<std::uint64_t>(texture.from) << 32) | texture.index;
    if (auto const found = slot.texture_sets.find(key); found != slot.texture_sets.end()) {
      return found->second;
    }
    auto set = allocate_texture_set(slot);
    if (!set) {
      return std::unexpected(std::move(set.error()));
    }
    auto const image_info = VkDescriptorImageInfo{VK_NULL_HANDLE, image.view.get(),
                                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    auto const write =
        VkWriteDescriptorSet{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                             .dstSet = *set,
                             .dstBinding = 0,
                             .descriptorCount = 1,
                             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                             .pImageInfo = &image_info};
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    slot.texture_sets.emplace(key, *set);
    return *set;
  }

  auto allocate_texture_set(frame_slot &slot) -> std::expected<VkDescriptorSet, render_error> {
    for (;;) {
      if (slot.pool_index >= slot.pools.size()) {
        auto const size =
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, texture_sets_per_pool};
        auto const info =
            VkDescriptorPoolCreateInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .maxSets = texture_sets_per_pool,
                                       .poolSizeCount = 1,
                                       .pPoolSizes = &size};
        auto pool = VkDescriptorPool{};
        if (auto const created = vkCreateDescriptorPool(device_, &info, nullptr, &pool);
            created != VK_SUCCESS) {
          return std::unexpected(failure("create a descriptor pool", created));
        }
        slot.pools.push_back(own(device_, pool));
      }
      auto const layout = pipelines_.texture_layout.get();
      auto const info =
          VkDescriptorSetAllocateInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                      .descriptorPool = slot.pools[slot.pool_index].get(),
                                      .descriptorSetCount = 1,
                                      .pSetLayouts = &layout};
      auto set = VkDescriptorSet{};
      auto const allocated = vkAllocateDescriptorSets(device_, &info, &set);
      if (allocated == VK_SUCCESS) {
        return set;
      }
      if (allocated != VK_ERROR_OUT_OF_POOL_MEMORY && allocated != VK_ERROR_FRAGMENTED_POOL) {
        return std::unexpected(failure("allocate a descriptor set", allocated));
      }
      ++slot.pool_index;
    }
  }

  // The Vulkan side of `walk_scene`: records the draws of one frame into render passes.
  class scene_encoder {
  public:
    scene_encoder(vulkan_backend &owner, frame_slot &slot,
                  std::vector<std::shared_ptr<gpu_image>> const &images, VkClearValue clear,
                  bool full_redraw)
        : owner_(owner), slot_(slot), commands_(slot.commands), images_(images), clear_(clear),
          full_redraw_(full_redraw) {}

    auto start(pass_target const &root) -> void {
      open(root,
           full_redraw_ ? owner_.pipelines_.root_clear.get() : owner_.pipelines_.root_load.get(),
           clear_);
    }

    auto set_scissor(pass_target const &target, pixel_rect scissor) -> void {
      auto const rect = VkRect2D{
          {scissor.x - target.area.x, scissor.y - target.area.y},
          {static_cast<std::uint32_t>(scissor.width), static_cast<std::uint32_t>(scissor.height)}};
      vkCmdSetScissor(commands_, 0, 1, &rect);
    }

    auto draw_clear(std::size_t area_index) -> std::expected<void, render_error> {
      vkCmdBindPipeline(commands_, VK_PIPELINE_BIND_POINT_GRAPHICS, owner_.pipelines_.copy.get());
      if (auto const bound = bind_texture({}); !bound) {
        return bound;
      }
      vkCmdDraw(commands_, 4, 1, 0, static_cast<std::uint32_t>(area_index));
      vkCmdBindPipeline(commands_, VK_PIPELINE_BIND_POINT_GRAPHICS, owner_.pipelines_.blend.get());
      return {};
    }

    auto draw(draw_batch const &batch) -> std::expected<void, render_error> {
      if (auto const bound = bind_texture(batch.texture); !bound) {
        return bound;
      }
      vkCmdDraw(commands_, 4, batch.count, 0, batch.first);
      return {};
    }

    auto begin_layer(pass_target const &parent, std::uint32_t index, pixel_rect area)
        -> std::expected<pass_target, render_error> {
      close(parent);
      auto layer = owner_.layer_image(index, area);
      if (!layer) {
        return std::unexpected(std::move(layer.error()));
      }
      auto const target = pass_target{*layer, area, true};
      open(target, owner_.pipelines_.layer_begin.get(), VkClearValue{});
      return target;
    }

    auto end_layer(pass_target const &layer, pass_target const &parent) -> void {
      close(layer);
      open(parent,
           parent.is_layer ? owner_.pipelines_.layer_resume.get()
                           : owner_.pipelines_.root_load.get(),
           clear_);
    }

    auto finish(pass_target const &target) -> void { close(target); }

  private:
    auto open(pass_target const &target, VkRenderPass pass, VkClearValue clear) -> void {
      auto const begin =
          VkRenderPassBeginInfo{.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                .renderPass = pass,
                                .framebuffer = target.image->framebuffer.get(),
                                .renderArea = {{0, 0},
                                               {static_cast<std::uint32_t>(target.area.width),
                                                static_cast<std::uint32_t>(target.area.height)}},
                                .clearValueCount = 1,
                                .pClearValues = &clear};
      vkCmdBeginRenderPass(commands_, &begin, VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindPipeline(commands_, VK_PIPELINE_BIND_POINT_GRAPHICS, owner_.pipelines_.blend.get());
      auto const set = slot_.instance_set;
      vkCmdBindDescriptorSets(commands_, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              owner_.pipelines_.layout.get(), 0, 1, &set, 0, nullptr);
      auto const uniforms = target_uniforms{
          {static_cast<float>(target.area.x), static_cast<float>(target.area.y)},
          {static_cast<float>(target.area.width), static_cast<float>(target.area.height)}};
      vkCmdPushConstants(commands_, owner_.pipelines_.layout.get(),
                         VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                         sizeof(uniforms), &uniforms);
      auto const viewport = VkViewport{
          0.0F, 0.0F, static_cast<float>(target.area.width), static_cast<float>(target.area.height),
          0.0F, 1.0F};
      vkCmdSetViewport(commands_, 0, 1, &viewport);
    }

    auto close(pass_target const &target) -> void {
      vkCmdEndRenderPass(commands_);
      target.image->layout = target.is_layer ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                             : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
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
      auto set = owner_.texture_set(slot_, texture, image);
      if (!set) {
        return std::unexpected(std::move(set.error()));
      }
      vkCmdBindDescriptorSets(commands_, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              owner_.pipelines_.layout.get(), 1, 1, &*set, 0, nullptr);
      return {};
    }

    vulkan_backend &owner_;
    frame_slot &slot_;
    VkCommandBuffer commands_;
    std::vector<std::shared_ptr<gpu_image>> const &images_;
    VkClearValue clear_;
    bool full_redraw_;
  };

  auto encode(frame_slot &slot, gpu_scene const &scene,
              std::vector<std::shared_ptr<gpu_image>> const &images, frame_request const &request)
      -> std::expected<void, render_error> {
    if (frame_->layout == VK_IMAGE_LAYOUT_UNDEFINED) {
      barrier(slot.commands, frame_->image.get(), VK_IMAGE_LAYOUT_UNDEFINED,
              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
      frame_->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    auto clear = VkClearValue{};
    std::ranges::copy(premultiplied(request.clear), std::begin(clear.color.float32));
    auto encoder = scene_encoder{*this, slot, images, clear, request.full_redraw};
    return walk_scene(scene, request,
                      pass_target{frame_.get(), {0, 0, request.width, request.height}, false},
                      encoder);
  }

  // ---- Presentation ----

  auto create_swapchain(int width, int height) -> std::expected<void, render_error> {
    auto caps = VkSurfaceCapabilitiesKHR{};
    if (auto const queried =
            vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_handle(), &caps);
        queried != VK_SUCCESS) {
      return std::unexpected(failure("query the surface", queried));
    }
    if ((caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0) {
      return std::unexpected(render_error{"the Vulkan surface cannot receive copies"});
    }
    auto extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
      extent = {std::clamp(static_cast<std::uint32_t>(width), caps.minImageExtent.width,
                           caps.maxImageExtent.width),
                std::clamp(static_cast<std::uint32_t>(height), caps.minImageExtent.height,
                           caps.maxImageExtent.height)};
    }
    if (extent.width == 0 || extent.height == 0) {
      return std::unexpected(render_error{"the window has no drawable to present"});
    }

    auto format_count = std::uint32_t{0};
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_handle(), &format_count, nullptr);
    auto formats = std::vector<VkSurfaceFormatKHR>(format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_handle(), &format_count,
                                         formats.data());
    auto const has_format = std::ranges::any_of(formats, [](VkSurfaceFormatKHR const &format) {
      return format.format == frame_format &&
             format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    });
    if (!has_format) {
      return std::unexpected(render_error{"the Vulkan surface has no BGRA8 format"});
    }

    auto image_count = caps.minImageCount + 1;
    if (caps.maxImageCount != 0) {
      image_count = std::min(image_count, caps.maxImageCount);
    }
    auto const composite = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) != 0
                               ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                               : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    auto const info = VkSwapchainCreateInfoKHR{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surface_handle(),
        .minImageCount = image_count,
        .imageFormat = frame_format,
        .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform,
        .compositeAlpha = composite,
        // The first-in first-out mode is the one that waits for the display.
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
        .oldSwapchain = swapchain_.handle.get()};
    auto handle = VkSwapchainKHR{};
    vkDeviceWaitIdle(device_);
    if (auto const created = vkCreateSwapchainKHR(device_, &info, nullptr, &handle);
        created != VK_SUCCESS) {
      return std::unexpected(failure("create a swapchain", created));
    }
    auto replacement = swapchain_state{};
    replacement.handle = own(device_, handle);
    replacement.extent = extent;
    auto count = std::uint32_t{0};
    vkGetSwapchainImagesKHR(device_, handle, &count, nullptr);
    replacement.images.resize(count);
    vkGetSwapchainImagesKHR(device_, handle, &count, replacement.images.data());
    for (auto index = std::uint32_t{0}; index < count; ++index) {
      auto semaphore = create_semaphore();
      if (!semaphore) {
        return std::unexpected(std::move(semaphore.error()));
      }
      replacement.finished.push_back(std::move(*semaphore));
    }
    swapchain_ = std::move(replacement);
    swapchain_request_ = {width, height};
    swapchain_stale_ = false;
    return {};
  }

  auto acquire(frame_slot &slot, int width, int height)
      -> std::expected<std::uint32_t, render_error> {
    for (auto attempt = 0; attempt < 2; ++attempt) {
      if (!swapchain_.handle || swapchain_stale_ || swapchain_request_.first != width ||
          swapchain_request_.second != height) {
        if (auto const created = create_swapchain(width, height); !created) {
          return std::unexpected(created.error());
        }
      }
      auto index = std::uint32_t{0};
      auto const acquired = vkAcquireNextImageKHR(device_, swapchain_.handle.get(), UINT64_MAX,
                                                  slot.acquired.get(), VK_NULL_HANDLE, &index);
      if (acquired == VK_SUCCESS || acquired == VK_SUBOPTIMAL_KHR) {
        swapchain_stale_ = acquired == VK_SUBOPTIMAL_KHR;
        return index;
      }
      if (acquired != VK_ERROR_OUT_OF_DATE_KHR) {
        return std::unexpected(failure("get a swapchain image", acquired));
      }
      swapchain_stale_ = true;
    }
    return std::unexpected(render_error{"the window has no drawable to present"});
  }

  // Copies the frame into the swapchain image and leaves both ready for their next use.
  auto copy_to_swapchain(VkCommandBuffer commands, std::uint32_t image_index) -> void {
    auto const target = swapchain_.images[image_index];
    barrier(commands, frame_->image.get(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    barrier(commands, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    auto const width = std::min(static_cast<std::uint32_t>(frame_->width), swapchain_.extent.width);
    auto const height =
        std::min(static_cast<std::uint32_t>(frame_->height), swapchain_.extent.height);
    auto const region = VkImageCopy{.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                    .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                    .extent = {width, height, 1}};
    vkCmdCopyImage(commands, frame_->image.get(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    barrier(commands, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    barrier(commands, frame_->image.get(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  }

  auto submit_and_present(frame_slot &slot, std::uint32_t image_index)
      -> std::expected<void, render_error> {
    auto const waits_for_image = surface_ != 0;
    auto const wait_stage = VkPipelineStageFlags{VK_PIPELINE_STAGE_TRANSFER_BIT};
    auto const acquired = slot.acquired.get();
    auto const finished = waits_for_image ? swapchain_.finished[image_index].get() : VK_NULL_HANDLE;
    auto const submit = VkSubmitInfo{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                     .waitSemaphoreCount = waits_for_image ? 1U : 0U,
                                     .pWaitSemaphores = &acquired,
                                     .pWaitDstStageMask = &wait_stage,
                                     .commandBufferCount = 1,
                                     .pCommandBuffers = &slot.commands,
                                     .signalSemaphoreCount = waits_for_image ? 1U : 0U,
                                     .pSignalSemaphores = &finished};
    auto const fence = slot.fence.get();
    vkResetFences(device_, 1, &fence);
    if (auto const submitted = vkQueueSubmit(queue_, 1, &submit, fence); submitted != VK_SUCCESS) {
      // Leaves the fence signaled, so that the next frame in this slot does not wait for ever.
      auto const signal = VkSubmitInfo{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
      vkQueueSubmit(queue_, 1, &signal, fence);
      return std::unexpected(failure("submit a frame", submitted));
    }
    slot.serial = next_serial_++;
    next_slot_ = (next_slot_ + 1) % frames_in_flight;
    if (!waits_for_image) {
      return {};
    }
    auto const swapchain = swapchain_.handle.get();
    auto const present = VkPresentInfoKHR{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                                          .waitSemaphoreCount = 1,
                                          .pWaitSemaphores = &finished,
                                          .swapchainCount = 1,
                                          .pSwapchains = &swapchain,
                                          .pImageIndices = &image_index};
    auto const presented = vkQueuePresentKHR(queue_, &present);
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) {
      swapchain_stale_ = true;
    } else if (presented != VK_SUCCESS) {
      return std::unexpected(failure("present a frame", presented));
    }
    return {};
  }

  // Premultiplied BGRA pixels of the frame, row by row. Waits for the GPU. Empty on failure.
  [[nodiscard]] auto read_pixels(int x, int y, int width, int height) const
      -> std::vector<std::uint32_t> {
    auto const count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    auto buffer = create_buffer(count * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    if (!buffer) {
      return {};
    }
    auto const copied = submit_once([&](VkCommandBuffer commands) {
      barrier(commands, frame_->image.get(), frame_->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
      auto const region =
          VkBufferImageCopy{.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .imageOffset = {x, y, 0},
                            .imageExtent = {static_cast<std::uint32_t>(width),
                                            static_cast<std::uint32_t>(height), 1}};
      vkCmdCopyImageToBuffer(commands, frame_->image.get(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             buffer->buffer.get(), 1, &region);
      barrier(commands, frame_->image.get(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, frame_->layout);
    });
    if (!copied) {
      return {};
    }
    auto pixels = std::vector<std::uint32_t>(count);
    std::memcpy(pixels.data(), buffer->mapped, count * 4);
    return pixels;
  }

  [[nodiscard]] auto surface_handle() const -> VkSurfaceKHR {
    return reinterpret_cast<VkSurfaceKHR>(static_cast<std::uintptr_t>(surface_));
  }

  struct pipeline_set {
    owned<VkRenderPass> root_clear;
    owned<VkRenderPass> root_load;
    owned<VkRenderPass> layer_begin;
    owned<VkRenderPass> layer_resume;
    owned<VkSampler> sampler;
    owned<VkDescriptorSetLayout> instance_layout;
    owned<VkDescriptorSetLayout> texture_layout;
    owned<VkPipelineLayout> layout;
    owned<VkPipeline> blend;
    owned<VkPipeline> copy;
  };

  platform *platform_;
  window_id window_;
  VkInstance instance_ = VK_NULL_HANDLE;
  std::uint64_t surface_ = 0;
  VkPhysicalDevice physical_ = VK_NULL_HANDLE;
  VkPhysicalDeviceMemoryProperties memory_{};
  std::uint32_t family_ = 0;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  owned<VkCommandPool> command_pool_;
  owned<VkDescriptorPool> instance_pool_;
  pipeline_set pipelines_;
  std::array<frame_slot, frames_in_flight> slots_;
  std::size_t next_slot_ = 0;
  // The number of the frame that is being recorded, and the last one known to have finished.
  std::uint64_t next_serial_ = 1;
  std::uint64_t completed_ = 0;
  std::vector<std::pair<std::uint64_t, std::shared_ptr<void>>> retired_;

  swapchain_state swapchain_;
  std::pair<int, int> swapchain_request_{0, 0};
  bool swapchain_stale_ = false;

  std::shared_ptr<gpu_image> frame_;
  std::shared_ptr<gpu_image> empty_;
  glyph_atlas atlas_;
  gpu_scene_builder builder_;
  // By atlas page index: the page's id and its image.
  std::vector<std::pair<std::uint64_t, std::shared_ptr<gpu_image>>> page_images_;
  image_cache<std::shared_ptr<gpu_image>> images_{image_cache_limit};
  // By scene layer index.
  std::vector<std::shared_ptr<gpu_image>> layers_;
};

} // namespace

auto vulkan_device_available() -> bool {
  auto instance = create_instance({});
  if (!instance) {
    return false;
  }
  auto count = std::uint32_t{0};
  vkEnumeratePhysicalDevices(*instance, &count, nullptr);
  auto devices = std::vector<VkPhysicalDevice>(count);
  vkEnumeratePhysicalDevices(*instance, &count, devices.data());
  auto const usable = std::ranges::any_of(devices, [](VkPhysicalDevice device) {
    auto properties = VkPhysicalDeviceProperties{};
    vkGetPhysicalDeviceProperties(device, &properties);
    return device_rank(properties) > 0;
  });
  vkDestroyInstance(*instance, nullptr);
  return usable;
}

auto make_vulkan_backend(platform *platform, window_id window)
    -> std::expected<std::unique_ptr<render_backend>, render_error> {
  auto backend = std::make_unique<vulkan_backend>(platform, window);
  if (auto ready = backend->initialize(); !ready) {
    return std::unexpected(std::move(ready.error()));
  }
  return backend;
}

} // namespace magma::render
