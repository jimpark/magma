// The shaders of the Direct3D 12 backend. One shader draws every instance kind, with the same
// coverage math as the Metal and Vulkan shaders. Change the three together. See `gpu_instance` in
// gpu_scene.h for the layout.

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

StructuredBuffer<gpu_instance> instances : register(t0);
Texture2D<float4> source : register(t1);
SamplerState bilinear : register(s0);

cbuffer target_uniforms : register(b0) {
  float2 origin;
  float2 size;
  // Direct3D does not add the first instance to SV_InstanceID.
  uint first_instance;
};

struct varyings {
  float4 position : SV_Position;
  nointerpolation uint index : INSTANCE;
};

varyings vertex_main(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID) {
  uint index = first_instance + instance_id;
  float4 bounds = instances[index].bounds;
  float2 corner = float2(float(vertex_id & 1u), float(vertex_id >> 1u));
  float2 position = lerp(bounds.xy, bounds.zw, corner) - origin;
  varyings result;
  result.position = float4(position / size * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
  result.index = index;
  return result;
}

// The part of a pixel inside a straight edge, for a box filter. `distance_to_edge` is from the
// pixel center to the edge in pixels, positive outside. `normal` is the edge's unit normal.
float edge_coverage(float distance_to_edge, float2 normal) {
  float2 n = abs(normal);
  float a = max(n.x, n.y);
  float b = min(n.x, n.y);
  if (b < 1.0e-3) {
    return saturate(0.5 - distance_to_edge);
  }
  float flat_reach = 0.5 * (a - b);
  float reach = 0.5 * (a + b);
  if (distance_to_edge >= reach) {
    return 0.0;
  }
  if (distance_to_edge <= -reach) {
    return 1.0;
  }
  if (distance_to_edge > flat_reach) {
    float t = reach - distance_to_edge;
    return t * t / (2.0 * a * b);
  }
  if (distance_to_edge < -flat_reach) {
    float t = reach + distance_to_edge;
    return 1.0 - t * t / (2.0 * a * b);
  }
  return 0.5 - distance_to_edge / a;
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
  static const int strips = 8;
  float low = q.y - half_pixel.y;
  float height = 2.0 * half_pixel.y;
  float width = 2.0 * half_pixel.x / float(strips);
  float covered = 0.0;
  [unroll] for (int strip = 0; strip < strips; ++strip) {
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

float4 fragment_main(varyings input) : SV_Target {
  gpu_instance item = instances[input.index];
  float2 device_position = input.position.xy + origin;
  float3 homogeneous = float3(device_position, 1.0);
  float2 local = float2(dot(item.to_local_x.xyz, homogeneous),
                        dot(item.to_local_y.xyz, homogeneous));
  float2 gradient_x = item.to_local_x.xy;
  float2 gradient_y = item.to_local_y.xy;
  float coverage = box_coverage(device_position, item.clip);
  float2 within = (local - item.outer.xy) / (item.outer.zw - item.outer.xy);
  float2 uv = lerp(item.inner.xy, item.inner.zw, within);

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
    color = item.color * source.Sample(bilinear, uv).r *
            rect_coverage(local, item.outer, 0.0, gradient_x, gradient_y);
  } else if (kind == 2u) {
    color = source.Sample(bilinear, uv) *
            rect_coverage(local, item.outer, 0.0, gradient_x, gradient_y);
  } else {
    float2 texel = (device_position - item.inner.xy) * item.inner.zw;
    color = source.Sample(bilinear, texel) * item.color *
            rect_coverage(local, item.outer, 0.0, gradient_x, gradient_y);
  }
  return color * coverage;
}
