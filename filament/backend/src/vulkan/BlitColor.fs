#version 320 es
precision mediump float;

layout(set = 0, binding = 0, std140) uniform ParamsBlock {
    mat4 tmp;
} params;

layout(location = 0) out highp vec4 color;

void main() {
    color = vec4(1);
}
