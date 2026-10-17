#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <utility>
#include <variant>
#include <vector>

#include "backend.h"
#include "gpu_scene.h"
#include "pixels.h"

namespace magma::render {

// Walks a scene once for each damaged area and calls a backend's `hooks` for the draws that touch
// the area. The walk skips the batches and layer passes that cannot reach it. `target` is the
// backend's description of a render target and has a `pixel_rect area` member: the frame device
// pixels the target covers.
//
// The hooks are:
//   start(root)                      Opens the root pass; once, before the first area.
//   set_scissor(target, scissor)     Limits drawing to `scissor`, in frame device pixels.
//   draw_clear(area_index)           Replaces the damaged area with the clear color, with the copy
//                                    pipeline. Only for partial redraws.
//   draw(batch)                      Binds the batch's texture and draws its instances.
//   begin_layer(parent, index, area) Leaves `parent` and opens a cleared pass for the layer. When
//   it
//                                    fails it leaves no pass open.
//   end_layer(layer, parent)         Leaves `layer` and reopens `parent` without clearing it.
//   finish(target)                   Closes the pass that is open. Also called after a failed
//                                    `draw_clear` or `draw`.
// `draw_clear`, `draw` and `begin_layer` return `std::expected`; the others return nothing.
template <class target, class hooks>
[[nodiscard]] auto walk_scene(gpu_scene const &scene, frame_request const &request,
                              target const &root, hooks &backend)
    -> std::expected<void, render_error> {
  if (request.areas.empty()) {
    return {};
  }
  auto stack = std::vector<target>{};
  auto current = root;
  auto scissor = pixel_rect{};
  backend.start(root);

  // Limits drawing to the damaged area inside the current target. False when they do not meet.
  auto const restrict_to = [&](pixel_rect area) {
    scissor = intersect(area, current.area);
    if (!has_area(scissor)) {
      return false;
    }
    backend.set_scissor(current, scissor);
    return true;
  };
  auto const fail = [&](render_error error) {
    backend.finish(current);
    return std::unexpected(std::move(error));
  };

  for (auto area_index = std::size_t{0}; area_index < request.areas.size(); ++area_index) {
    auto const area = request.areas[area_index];
    auto visible = restrict_to(area);
    if (visible && !request.full_redraw) {
      if (auto drawn = backend.draw_clear(area_index); !drawn) {
        return fail(std::move(drawn.error()));
      }
    }
    for (auto op_index = std::size_t{0}; op_index < scene.ops.size();) {
      auto const &op = scene.ops[op_index];
      if (auto const *const batch = std::get_if<draw_batch>(&op)) {
        if (visible && has_area(intersect(batch->bounds, scissor))) {
          if (auto drawn = backend.draw(*batch); !drawn) {
            return fail(std::move(drawn.error()));
          }
        }
        ++op_index;
      } else if (auto const *const begin = std::get_if<begin_layer_pass>(&op)) {
        auto const layer_area = scene.layers[begin->layer].area;
        if (!has_area(intersect(area, layer_area))) {
          // Nothing in the layer reaches the area, and neither does its composite.
          op_index = begin->end;
          continue;
        }
        auto layer = backend.begin_layer(current, begin->layer, layer_area);
        if (!layer) {
          return std::unexpected(std::move(layer.error()));
        }
        stack.push_back(std::move(current));
        current = std::move(*layer);
        visible = restrict_to(area);
        ++op_index;
      } else {
        if (!stack.empty()) {
          backend.end_layer(current, stack.back());
          current = std::move(stack.back());
          stack.pop_back();
          visible = restrict_to(area);
        }
        ++op_index;
      }
    }
  }
  backend.finish(current);
  return {};
}

} // namespace magma::render
