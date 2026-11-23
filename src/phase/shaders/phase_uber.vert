#version 460

void main() {
    // Single oversized fullscreen triangle (gl_VertexIndex 0..2):
    // Eliminates quad diagonal helper-lane overdraw and all per-instance vertex shading!
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
