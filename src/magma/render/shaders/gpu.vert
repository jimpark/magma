#version 450

// The vertex stage of the Vulkan backend. See `gpu_instance` in gpu_scene.h for the layout.

struct gpu_instance {
  vec4 bounds;
  vec4 outer;
  vec4 inner;
  vec4 params;
  vec4 color;
  vec4 to_local_x;
  vec4 to_local_y;
  vec4 clip;
};

layout(std430, set = 0, binding = 0) readonly buffer instance_buffer {
  gpu_instance items[];
};

layout(push_constant) uniform target_uniforms {
  vec2 origin;
  vec2 size;
} frame;

layout(location = 0) flat out uint instance_index;

void main() {
  vec4 bounds = items[gl_InstanceIndex].bounds;
  vec2 corner = vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1));
  vec2 position = mix(bounds.xy, bounds.zw, corner) - frame.origin;
  gl_Position = vec4(position / frame.size * 2.0 - 1.0, 0.0, 1.0);
  instance_index = uint(gl_InstanceIndex);
}
