#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "backend.h"
#include "glyph_atlas.h"
#include "gpu_scene.h"

#if !__has_feature(objc_arc)
#error "metal_backend.mm must be compiled with -fobjc-arc"
#endif

namespace magma::render {
namespace {

// Frames the CPU may encode before the GPU has finished the oldest of them.
constexpr auto frames_in_flight = std::size_t{3};
// Images are not evicted one at a time. A cache this large means the caller has moved on through
// many images, so starting over is cheaper than tracking use.
constexpr auto image_cache_limit = std::size_t{64};
constexpr auto smallest_instance_buffer = std::size_t{64} * 1024;

// One shader draws every instance kind. See `gpu_instance` in gpu_scene.h for the layout.
constexpr char const shader_source[] = R"(
#include <metal_stdlib>
using namespace metal;

struct gpu_instance {
  float4 bounds;
  float4 outer;
  float4 inner;
  float4 params;
  float4 color;
  float4 to_local_x;
  float4 to_local_y;
  float4 clip;
};

struct target {
  float2 origin;
  float2 size;
};

struct varyings {
  float4 position [[position]];
  uint index [[flat]];
};

vertex varyings vertex_main(uint vertex_id [[vertex_id]], uint instance_id [[instance_id]],
                            device gpu_instance const *instances [[buffer(0)]],
                            constant target &frame [[buffer(1)]]) {
  float4 bounds = instances[instance_id].bounds;
  float2 corner = float2(float(vertex_id & 1u), float(vertex_id >> 1u));
  float2 position = mix(bounds.xy, bounds.zw, corner) - frame.origin;
  varyings result;
  result.position =
      float4(position / frame.size * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
  result.index = instance_id;
  return result;
}

// The part of a pixel inside a straight edge, for a box filter. `distance` is from the pixel
// center to the edge in pixels, positive outside. `normal` is the edge's unit normal.
float edge_coverage(float distance, float2 normal) {
  float2 n = abs(normal);
  float a = max(n.x, n.y);
  float b = min(n.x, n.y);
  if (b < 1.0e-3) {
    return saturate(0.5 - distance);
  }
  float flat_reach = 0.5 * (a - b);
  float reach = 0.5 * (a + b);
  if (distance >= reach) {
    return 0.0;
  }
  if (distance <= -reach) {
    return 1.0;
  }
  if (distance > flat_reach) {
    float t = reach - distance;
    return t * t / (2.0 * a * b);
  }
  if (distance < -flat_reach) {
    float t = reach + distance;
    return 1.0 - t * t / (2.0 * a * b);
  }
  return 0.5 - distance / a;
}

// The part of a pixel between two parallel edges at local `low` and `high`. `gradient` is how
// fast the local coordinate changes per device pixel.
float slab_coverage(float u, float low, float high, float2 gradient) {
  float scale = length(gradient);
  if (scale == 0.0) {
    return 0.0;
  }
  float2 normal = gradient / scale;
  return saturate(edge_coverage((low - u) / scale, normal) +
                  edge_coverage((u - high) / scale, normal) - 1.0);
}

// The part of a pixel's footprint inside a rounded corner, for an axis-aligned pixel. `q` is
// the pixel center from the corner's circle center, with both axes pointing out of the rect, and
// `half_pixel` is half the footprint, in the same units. The footprint is cut into vertical
// strips, and the arc's height is exact in each.
float corner_coverage(float2 q, float2 half_pixel, float radius) {
  constexpr int strips = 8;
  float low = q.y - half_pixel.y;
  float height = 2.0 * half_pixel.y;
  float width = 2.0 * half_pixel.x / float(strips);
  float covered = 0.0;
  for (int strip = 0; strip < strips; ++strip) {
    float x = q.x - half_pixel.x + (float(strip) + 0.5) * width;
    if (x < radius) {
      float top = x <= 0.0 ? radius : sqrt(radius * radius - x * x);
      covered += clamp(top - low, 0.0, height);
    }
  }
  return covered / (float(strips) * height);
}

float rect_coverage(float2 local, float4 rect, float radius, float2 gradient_x,
                    float2 gradient_y) {
  if (rect.z <= rect.x || rect.w <= rect.y) {
    return 0.0;
  }
  if (radius > 0.0) {
    float2 center = 0.5 * (rect.xy + rect.zw);
    float2 corner = abs(local - center) - (0.5 * (rect.zw - rect.xy) - radius);
    if (gradient_x.y == 0.0 && gradient_y.x == 0.0) {
      float2 half_pixel = 0.5 * float2(abs(gradient_x.x), abs(gradient_y.y));
      if (corner.x + half_pixel.x > 0.0 && corner.y + half_pixel.y > 0.0) {
        return corner_coverage(corner, half_pixel, radius);
      }
    } else if (corner.x > 0.0 && corner.y > 0.0) {
      // Near the pixel the arc is close to a straight edge along its normal.
      float from_center = length(corner);
      float2 normal = sign(local - center) * corner / from_center;
      float2 gradient = normal.x * gradient_x + normal.y * gradient_y;
      float scale = length(gradient);
      return edge_coverage((from_center - radius) / scale, gradient / scale);
    }
  }
  return slab_coverage(local.x, rect.x, rect.z, gradient_x) *
         slab_coverage(local.y, rect.y, rect.w, gradient_y);
}

// The part of the pixel at `center` inside an axis-aligned device box.
float box_coverage(float2 center, float4 box) {
  float2 extent = max(min(center + 0.5, box.zw) - max(center - 0.5, box.xy), 0.0);
  return extent.x * extent.y;
}

fragment float4 fragment_main(varyings input [[stage_in]],
                              device gpu_instance const *instances [[buffer(0)]],
                              constant target &frame [[buffer(1)]],
                              texture2d<float> source [[texture(0)]],
                              sampler bilinear [[sampler(0)]]) {
  gpu_instance item = instances[input.index];
  float2 device_position = input.position.xy + frame.origin;
  float3 homogeneous = float3(device_position, 1.0);
  float2 local = float2(dot(item.to_local_x.xyz, homogeneous),
                        dot(item.to_local_y.xyz, homogeneous));
  float2 gradient_x = item.to_local_x.xy;
  float2 gradient_y = item.to_local_y.xy;
  float coverage = box_coverage(device_position, item.clip);
  float2 within = (local - item.outer.xy) / (item.outer.zw - item.outer.xy);
  float2 uv = mix(item.inner.xy, item.inner.zw, within);

  float4 color;
  uint kind = uint(item.params.z);
  if (kind == 0u) {
    float shape = rect_coverage(local, item.outer, item.params.x, gradient_x, gradient_y);
    if (item.params.w > 0.0) {
      shape = saturate(shape - rect_coverage(local, item.inner, item.params.y, gradient_x,
                                             gradient_y));
    }
    color = item.color * shape;
  } else if (kind == 1u) {
    color = item.color * source.sample(bilinear, uv).r *
            rect_coverage(local, item.outer, 0.0, gradient_x, gradient_y);
  } else if (kind == 2u) {
    color = source.sample(bilinear, uv) *
            rect_coverage(local, item.outer, 0.0, gradient_x, gradient_y);
  } else {
    float2 texel = (device_position - item.inner.xy) * item.inner.zw;
    color = source.sample(bilinear, texel) * item.color *
            rect_coverage(local, item.outer, 0.0, gradient_x, gradient_y);
  }
  return color * coverage;
}
)";

struct target_uniforms {
  std::array<float, 2> origin;
  std::array<float, 2> size;
};

// How a window frame reaches the screen. `retained` keeps the frame in a texture, redraws the
// damaged areas and copies the texture into each drawable. `full` redraws the whole frame into
// the drawable, so the application must send every command on each frame.
enum class redraw_mode { retained, full };

auto redraw_mode_from_environment() -> redraw_mode {
  auto const *const value = std::getenv("MAGMA_METAL_REDRAW");
  return value != nullptr && std::string_view{value} == "full" ? redraw_mode::full
                                                               : redraw_mode::retained;
}

auto describe(NSError *error) -> std::string {
  return error == nil ? std::string{"unknown error"}
                      : std::string{error.localizedDescription.UTF8String};
}

auto intersect(pixel_rect a, pixel_rect b) -> pixel_rect {
  auto const left = std::max(a.x, b.x);
  auto const top = std::max(a.y, b.y);
  auto const right = std::min(a.x + a.width, b.x + b.width);
  auto const bottom = std::min(a.y + a.height, b.y + b.height);
  return {left, top, std::max(0, right - left), std::max(0, bottom - top)};
}

// One render target: a texture and the frame device pixels it covers.
struct pass_target {
  id<MTLTexture> texture;
  pixel_rect area;
};

class metal_backend final : public render_backend {
public:
  metal_backend(id<MTLDevice> device, CAMetalLayer *layer)
      : device_(device), layer_(layer),
        full_(layer != nil && redraw_mode_from_environment() == redraw_mode::full) {}

  metal_backend(metal_backend const &) = delete;
  auto operator=(metal_backend const &) -> metal_backend & = delete;

  ~metal_backend() override {
    // The window's layer may go away with the renderer, so no frame may be left on its way to it.
    if (last_ != nil) {
      [last_ waitUntilCompleted];
    }
  }

  [[nodiscard]] auto initialize() -> std::expected<void, render_error> {
    queue_ = [device_ newCommandQueue];
    if (queue_ == nil) {
      return std::unexpected(render_error{"could not create a Metal command queue"});
    }
    NSError *error = nil;
    auto const library = [device_ newLibraryWithSource:@(shader_source) options:nil error:&error];
    if (library == nil) {
      return std::unexpected(
          render_error{"could not compile the Metal shaders: " + describe(error)});
    }
    auto *const descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = [library newFunctionWithName:@"vertex_main"];
    descriptor.fragmentFunction = [library newFunctionWithName:@"fragment_main"];
    descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    copy_pipeline_ = [device_ newRenderPipelineStateWithDescriptor:descriptor error:&error];
    auto *const attachment = descriptor.colorAttachments[0];
    attachment.blendingEnabled = YES;
    attachment.rgbBlendOperation = MTLBlendOperationAdd;
    attachment.alphaBlendOperation = MTLBlendOperationAdd;
    attachment.sourceRGBBlendFactor = MTLBlendFactorOne;
    attachment.sourceAlphaBlendFactor = MTLBlendFactorOne;
    attachment.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    attachment.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    blend_pipeline_ = [device_ newRenderPipelineStateWithDescriptor:descriptor error:&error];
    if (copy_pipeline_ == nil || blend_pipeline_ == nil) {
      return std::unexpected(
          render_error{"could not create the Metal pipelines: " + describe(error)});
    }

    auto *const sampling = [MTLSamplerDescriptor new];
    sampling.minFilter = MTLSamplerMinMagFilterLinear;
    sampling.magFilter = MTLSamplerMinMagFilterLinear;
    sampling.sAddressMode = MTLSamplerAddressModeClampToEdge;
    sampling.tAddressMode = MTLSamplerAddressModeClampToEdge;
    sampler_ = [device_ newSamplerStateWithDescriptor:sampling];

    // Batches without a texture still bind one, so that the shader always has a valid texture.
    empty_texture_ =
        [device_ newTextureWithDescriptor:texture_descriptor(MTLPixelFormatR8Unorm, 1, 1)];
    auto const zero = std::uint8_t{0};
    [empty_texture_ replaceRegion:MTLRegionMake2D(0, 0, 1, 1)
                      mipmapLevel:0
                        withBytes:&zero
                      bytesPerRow:1];

    if (layer_ != nil) {
      layer_.device = device_;
      layer_.pixelFormat = MTLPixelFormatBGRA8Unorm;
      // A retained frame is copied into the drawable, which a framebuffer-only texture forbids.
      layer_.framebufferOnly = full_ ? YES : NO;
    }
    in_flight_ = dispatch_semaphore_create(static_cast<long>(frames_in_flight));
    return {};
  }

  [[nodiscard]] auto kind() const -> backend override { return backend::metal; }

  [[nodiscard]] auto retains_frame() const -> bool override { return !full_; }

  [[nodiscard]] auto report_presentation(presented_handler handler) -> bool override {
    if (layer_ == nil) {
      return false;
    }
    pending_presentation_ = std::move(handler);
    return true;
  }

  [[nodiscard]] auto draw(frame_request const &request)
      -> std::expected<void, render_error> override {
    @autoreleasepool {
      auto handler = std::exchange(pending_presentation_, nullptr);
      atlas_.begin_frame();
      auto const &scene =
          builder_.build(request.commands, request.areas, request.scale, request.clear, atlas_);
      upload_atlas();
      auto const images = upload_images(scene);

      dispatch_semaphore_wait(in_flight_, DISPATCH_TIME_FOREVER);
      auto signal = true;
      auto const release = [this, &signal] {
        if (signal) {
          dispatch_semaphore_signal(in_flight_);
        }
      };

      auto const instances = fill_instance_buffer(scene);
      if (instances == nil) {
        release();
        return std::unexpected(render_error{"could not allocate the instance buffer"});
      }

      id<CAMetalDrawable> drawable = nil;
      if (layer_ != nil) {
        auto const size = CGSizeMake(request.width, request.height);
        if (!CGSizeEqualToSize(layer_.drawableSize, size)) {
          layer_.drawableSize = size;
        }
        drawable = [layer_ nextDrawable];
        if (drawable == nil) {
          release();
          return std::unexpected(render_error{"the window has no drawable to present"});
        }
      }
      auto target = full_ ? drawable.texture : frame_texture(request.width, request.height);
      if (target == nil) {
        release();
        return std::unexpected(render_error{"could not create a frame of that size"});
      }

      auto const commands = [queue_ commandBuffer];
      encode(commands, scene, images, instances, target, request);
      if (drawable != nil) {
        if (!full_) {
          copy_to_drawable(commands, drawable.texture);
        }
        if (handler) {
          auto const clock_offset = platform::event_clock_now().count() -
                                    static_cast<std::int64_t>(CACurrentMediaTime() * 1e9);
          [drawable addPresentedHandler:^(id<MTLDrawable> shown) {
            auto const time = shown.presentedTime;
            if (time == 0) {
              handler(std::nullopt);
            } else {
              handler(
                  std::chrono::nanoseconds{static_cast<std::int64_t>(time * 1e9) + clock_offset});
            }
          }];
        }
        [commands presentDrawable:drawable];
      }
      auto const semaphore = in_flight_;
      [commands addCompletedHandler:^(id<MTLCommandBuffer> completed) {
        (void)completed;
        dispatch_semaphore_signal(semaphore);
      }];
      signal = false;
      [commands commit];
      last_ = commands;
      return {};
    }
  }

  [[nodiscard]] auto read_pixel(int x, int y) const -> std::optional<basalt::rgba> override {
    if (frame_ == nil || x < 0 || y < 0 || x >= static_cast<int>(frame_.width) ||
        y >= static_cast<int>(frame_.height)) {
      return std::nullopt;
    }
    return unpremultiply(read_pixels(x, y, 1, 1).front());
  }

  [[nodiscard]] auto read_frame() const -> basalt::raster_image_ptr override {
    if (frame_ == nil) {
      return nullptr;
    }
    auto const width = static_cast<int>(frame_.width);
    auto const height = static_cast<int>(frame_.height);
    auto pixels = std::vector<std::uint8_t>{};
    pixels.reserve(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    for (auto const pixel : read_pixels(0, 0, width, height)) {
      auto const color = unpremultiply(pixel);
      pixels.insert(pixels.end(), {color.r, color.g, color.b, color.a});
    }
    return basalt::raster_image::create(static_cast<std::uint32_t>(width),
                                        static_cast<std::uint32_t>(height), std::move(pixels));
  }

private:
  // Textures that the CPU writes. Without unified memory they need a copy on each side.
  [[nodiscard]] auto texture_descriptor(MTLPixelFormat format, int width, int height) const
      -> MTLTextureDescriptor * {
    auto *const descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                           width:static_cast<NSUInteger>(width)
                                                          height:static_cast<NSUInteger>(height)
                                                       mipmapped:NO];
    descriptor.usage = MTLTextureUsageShaderRead;
    descriptor.storageMode =
        device_.hasUnifiedMemory ? MTLStorageModeShared : MTLStorageModeManaged;
    return descriptor;
  }

  [[nodiscard]] auto render_target(int width, int height) const -> id<MTLTexture> {
    auto *const descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:static_cast<NSUInteger>(width)
                                                          height:static_cast<NSUInteger>(height)
                                                       mipmapped:NO];
    descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    descriptor.storageMode = MTLStorageModePrivate;
    return [device_ newTextureWithDescriptor:descriptor];
  }

  // The frame keeps its contents between frames. A new size starts it again, and the renderer
  // then sends a full redraw.
  auto frame_texture(int width, int height) -> id<MTLTexture> {
    if (frame_ == nil || static_cast<int>(frame_.width) != width ||
        static_cast<int>(frame_.height) != height) {
      frame_ = render_target(width, height);
    }
    return frame_;
  }

  auto layer_texture(std::uint32_t index, pixel_rect area) -> id<MTLTexture> {
    if (layer_textures_.size() <= index) {
      layer_textures_.resize(index + 1);
    }
    auto &texture = layer_textures_[index];
    if (texture == nil || static_cast<int>(texture.width) != area.width ||
        static_cast<int>(texture.height) != area.height) {
      texture = render_target(area.width, area.height);
    }
    return texture;
  }

  // Copies new atlas pages and the changed parts of old ones to their textures.
  auto upload_atlas() -> void {
    auto const pages = atlas_.pages();
    auto const size = atlas_.page_size();
    page_textures_.resize(pages.size());
    for (auto index = std::size_t{0}; index < pages.size(); ++index) {
      auto const &page = pages[index];
      auto &[id, texture] = page_textures_[index];
      auto area = page.dirty;
      if (id != page.id || texture == nil) {
        id = page.id;
        texture = [device_
            newTextureWithDescriptor:texture_descriptor(MTLPixelFormatR8Unorm, size, size)];
        area = {0, 0, size, size};
      }
      if (area.width > 0 && area.height > 0) {
        auto const offset = static_cast<std::size_t>(area.y) * static_cast<std::size_t>(size) +
                            static_cast<std::size_t>(area.x);
        [texture replaceRegion:MTLRegionMake2D(static_cast<NSUInteger>(area.x),
                                               static_cast<NSUInteger>(area.y),
                                               static_cast<NSUInteger>(area.width),
                                               static_cast<NSUInteger>(area.height))
                   mipmapLevel:0
                     withBytes:page.pixels.data() + offset
                   bytesPerRow:static_cast<NSUInteger>(size)];
      }
    }
    atlas_.clear_dirty();
  }

  // The textures of the scene's images, in scene order. Each image is copied to the GPU once.
  auto upload_images(gpu_scene const &scene) -> std::vector<id<MTLTexture>> {
    if (images_.size() + scene.images.size() > image_cache_limit) {
      images_.clear();
    }
    auto textures = std::vector<id<MTLTexture>>{};
    textures.reserve(scene.images.size());
    for (auto const *const image : scene.images) {
      auto &texture = images_[image->id()];
      if (texture == nil) {
        texture = upload_image(*image);
      }
      textures.push_back(texture != nil ? texture : empty_texture_);
    }
    return textures;
  }

  [[nodiscard]] auto upload_image(basalt::raster_image const &image) const -> id<MTLTexture> {
    auto const width = static_cast<int>(image.width());
    auto const height = static_cast<int>(image.height());
    auto const texture = [device_
        newTextureWithDescriptor:texture_descriptor(MTLPixelFormatRGBA8Unorm, width, height)];
    if (texture == nil) {
      return nil;
    }
    auto premultiplied = image.pixels();
    for (auto pixel = std::size_t{0}; pixel < premultiplied.size(); pixel += 4) {
      auto const alpha = std::uint32_t{premultiplied[pixel + 3]};
      for (auto channel = std::size_t{0}; channel < 3; ++channel) {
        premultiplied[pixel + channel] =
            static_cast<std::uint8_t>((premultiplied[pixel + channel] * alpha + 127) / 255);
      }
    }
    [texture replaceRegion:MTLRegionMake2D(0, 0, image.width(), image.height())
               mipmapLevel:0
                 withBytes:premultiplied.data()
               bytesPerRow:static_cast<NSUInteger>(width) * 4];
    return texture;
  }

  // Copies the instances into the next buffer of the ring. The caller has waited for the frame
  // that used it before.
  auto fill_instance_buffer(gpu_scene const &scene) -> id<MTLBuffer> {
    auto &buffer = instance_buffers_[next_buffer_];
    next_buffer_ = (next_buffer_ + 1) % frames_in_flight;
    auto const bytes = std::max<std::size_t>(scene.instances.size(), 1) * sizeof(gpu_instance);
    if (buffer == nil || buffer.length < bytes) {
      buffer = [device_
          newBufferWithLength:std::max(bytes * 2, smallest_instance_buffer)
                      options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined];
      if (buffer == nil) {
        return nil;
      }
    }
    std::memcpy(buffer.contents, scene.instances.data(),
                scene.instances.size() * sizeof(gpu_instance));
    return buffer;
  }

  auto encode(id<MTLCommandBuffer> commands, gpu_scene const &scene,
              std::vector<id<MTLTexture>> const &images, id<MTLBuffer> instances,
              id<MTLTexture> target, frame_request const &request) -> void {
    auto const root = pass_target{target, {0, 0, request.width, request.height}};
    auto stack = std::vector<pass_target>{};
    auto current = root;
    id<MTLRenderCommandEncoder> encoder = nil;
    auto scissor = pixel_rect{};

    auto const open = [&](pass_target const &pass, MTLLoadAction load, MTLClearColor clear) {
      auto *const descriptor = [MTLRenderPassDescriptor renderPassDescriptor];
      descriptor.colorAttachments[0].texture = pass.texture;
      descriptor.colorAttachments[0].loadAction = load;
      descriptor.colorAttachments[0].clearColor = clear;
      descriptor.colorAttachments[0].storeAction = MTLStoreActionStore;
      encoder = [commands renderCommandEncoderWithDescriptor:descriptor];
      [encoder setRenderPipelineState:blend_pipeline_];
      [encoder setVertexBuffer:instances offset:0 atIndex:0];
      [encoder setFragmentBuffer:instances offset:0 atIndex:0];
      auto const uniforms = target_uniforms{
          {static_cast<float>(pass.area.x), static_cast<float>(pass.area.y)},
          {static_cast<float>(pass.area.width), static_cast<float>(pass.area.height)}};
      [encoder setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:1];
      [encoder setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:1];
      [encoder setFragmentSamplerState:sampler_ atIndex:0];
    };
    // Limits drawing to the damaged area inside the current target. False when they do not meet.
    auto const restrict_to = [&](pixel_rect area) {
      scissor = intersect(area, current.area);
      if (scissor.width <= 0 || scissor.height <= 0) {
        return false;
      }
      [encoder setScissorRect:MTLScissorRect{static_cast<NSUInteger>(scissor.x - current.area.x),
                                             static_cast<NSUInteger>(scissor.y - current.area.y),
                                             static_cast<NSUInteger>(scissor.width),
                                             static_cast<NSUInteger>(scissor.height)}];
      return true;
    };
    auto const draw = [&](std::uint32_t first, std::uint32_t count) {
      auto const offset = static_cast<NSUInteger>(first) * sizeof(gpu_instance);
      [encoder setVertexBufferOffset:offset atIndex:0];
      [encoder setFragmentBufferOffset:offset atIndex:0];
      [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip
                  vertexStart:0
                  vertexCount:4
                instanceCount:count];
    };
    auto const texture_for = [&](texture_ref texture) -> id<MTLTexture> {
      switch (texture.from) {
      case texture_ref::source::atlas_page:
        return page_textures_[texture.index].second;
      case texture_ref::source::image:
        return images[texture.index];
      case texture_ref::source::layer:
        return layer_textures_[texture.index];
      case texture_ref::source::none:
        break;
      }
      return empty_texture_;
    };

    auto const clear = request.clear;
    auto const alpha = clear.a / 255.0;
    auto const clear_color = MTLClearColorMake(clear.r / 255.0 * alpha, clear.g / 255.0 * alpha,
                                               clear.b / 255.0 * alpha, alpha);
    for (auto area_index = std::size_t{0}; area_index < request.areas.size(); ++area_index) {
      auto const area = request.areas[area_index];
      if (encoder == nil) {
        open(root, request.full_redraw ? MTLLoadActionClear : MTLLoadActionLoad, clear_color);
      }
      auto visible = restrict_to(area);
      if (visible && !request.full_redraw) {
        [encoder setRenderPipelineState:copy_pipeline_];
        [encoder setFragmentTexture:empty_texture_ atIndex:0];
        draw(static_cast<std::uint32_t>(area_index), 1);
        [encoder setRenderPipelineState:blend_pipeline_];
      }
      for (auto const &op : scene.ops) {
        if (auto const *const batch = std::get_if<draw_batch>(&op)) {
          if (visible) {
            [encoder setFragmentTexture:texture_for(batch->texture) atIndex:0];
            draw(batch->first, batch->count);
          }
        } else if (auto const *const begin = std::get_if<begin_layer_pass>(&op)) {
          [encoder endEncoding];
          stack.push_back(current);
          auto const layer_area = scene.layers[begin->layer].area;
          current = {layer_texture(begin->layer, layer_area), layer_area};
          open(current, MTLLoadActionClear, MTLClearColorMake(0, 0, 0, 0));
          visible = restrict_to(area);
        } else if (!stack.empty()) {
          [encoder endEncoding];
          current = stack.back();
          stack.pop_back();
          open(current, MTLLoadActionLoad, clear_color);
          visible = restrict_to(area);
        }
      }
    }
    if (encoder != nil) {
      [encoder endEncoding];
    }
  }

  auto copy_to_drawable(id<MTLCommandBuffer> commands, id<MTLTexture> drawable) -> void {
    auto const blit = [commands blitCommandEncoder];
    [blit copyFromTexture:frame_
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(std::min(frame_.width, drawable.width),
                                      std::min(frame_.height, drawable.height), 1)
                toTexture:drawable
         destinationSlice:0
         destinationLevel:0
        destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];
  }

  // Premultiplied BGRA pixels of the frame, row by row. Waits for the GPU.
  [[nodiscard]] auto read_pixels(int x, int y, int width, int height) const
      -> std::vector<std::uint32_t> {
    @autoreleasepool {
      auto const row_bytes = static_cast<NSUInteger>(width) * 4;
      auto const bytes = row_bytes * static_cast<NSUInteger>(height);
      auto const buffer = [device_ newBufferWithLength:bytes options:MTLResourceStorageModeShared];
      auto const commands = [queue_ commandBuffer];
      auto const blit = [commands blitCommandEncoder];
      [blit copyFromTexture:frame_
                       sourceSlice:0
                       sourceLevel:0
                      sourceOrigin:MTLOriginMake(static_cast<NSUInteger>(x),
                                                 static_cast<NSUInteger>(y), 0)
                        sourceSize:MTLSizeMake(static_cast<NSUInteger>(width),
                                               static_cast<NSUInteger>(height), 1)
                          toBuffer:buffer
                 destinationOffset:0
            destinationBytesPerRow:row_bytes
          destinationBytesPerImage:bytes];
      [blit endEncoding];
      [commands commit];
      [commands waitUntilCompleted];
      auto pixels = std::vector<std::uint32_t>(static_cast<std::size_t>(width) *
                                               static_cast<std::size_t>(height));
      std::memcpy(pixels.data(), buffer.contents, bytes);
      return pixels;
    }
  }

  id<MTLDevice> device_;
  CAMetalLayer *layer_;
  bool full_;
  id<MTLCommandQueue> queue_;
  id<MTLRenderPipelineState> blend_pipeline_;
  id<MTLRenderPipelineState> copy_pipeline_;
  id<MTLSamplerState> sampler_;
  id<MTLTexture> empty_texture_;
  id<MTLTexture> frame_;
  id<MTLCommandBuffer> last_;
  dispatch_semaphore_t in_flight_;
  std::array<id<MTLBuffer>, frames_in_flight> instance_buffers_;
  std::size_t next_buffer_ = 0;
  glyph_atlas atlas_;
  gpu_scene_builder builder_;
  // By atlas page index: the page's id and its texture.
  std::vector<std::pair<std::uint64_t, id<MTLTexture>>> page_textures_;
  std::unordered_map<std::uint64_t, id<MTLTexture>> images_;
  // By scene layer index.
  std::vector<id<MTLTexture>> layer_textures_;
  presented_handler pending_presentation_;
};

} // namespace

auto metal_device_available() -> bool {
  @autoreleasepool {
    return MTLCreateSystemDefaultDevice() != nil;
  }
}

auto make_metal_backend(platform *platform, window_id window)
    -> std::expected<std::unique_ptr<render_backend>, render_error> {
  @autoreleasepool {
    auto const device = MTLCreateSystemDefaultDevice();
    if (device == nil) {
      return std::unexpected(render_error{"this computer has no Metal device"});
    }
    CAMetalLayer *layer = nil;
    if (platform != nullptr) {
      layer = (__bridge CAMetalLayer *)platform->metal_layer(window);
      if (layer == nil) {
        return std::unexpected(render_error{"the window was not created for Metal"});
      }
    }
    auto backend = std::make_unique<metal_backend>(device, layer);
    if (auto ready = backend->initialize(); !ready) {
      return std::unexpected(std::move(ready.error()));
    }
    return backend;
  }
}

} // namespace magma::render
