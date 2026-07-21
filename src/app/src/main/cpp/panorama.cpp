#include "panorama.h"
#include "log.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include "stb_image.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"

panorama_bg *gPanorama = new panorama_bg();

static constexpr int kMaxPanoramaW = 4096;
static constexpr int kSphereSegs = 64;

void panorama_bg::build_shader() {
    if (program) return;
    const char *vs = R"(#version 300 es
        layout(location=0) in vec3 aPos;
        layout(location=1) in vec2 aUV;
        uniform mat4 uMVP;
        out vec2 vUV;
        void main() {
            gl_Position = uMVP * vec4(aPos, 1.0);
            vUV = aUV;
        })";
    const char *fs = R"(#version 300 es
        precision mediump float;
        in vec2 vUV;
        uniform sampler2D uTex;
        out vec4 frag;
        void main() {
            frag = texture(uTex, vUV);
        })";
    program = glCreateProgram();
    GLuint s1 = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(s1, 1, &vs, nullptr); glCompileShader(s1);
    { GLint ok; glGetShaderiv(s1, GL_COMPILE_STATUS, &ok);
      if (!ok) { char log[1024]; glGetShaderInfoLog(s1, sizeof(log), nullptr, log); LOGE("pano VS: %s", log); } }
    GLuint s2 = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(s2, 1, &fs, nullptr); glCompileShader(s2);
    { GLint ok; glGetShaderiv(s2, GL_COMPILE_STATUS, &ok);
      if (!ok) { char log[1024]; glGetShaderInfoLog(s2, sizeof(log), nullptr, log); LOGE("pano FS: %s", log); } }
    glAttachShader(program, s1); glAttachShader(program, s2);
    glLinkProgram(program);
    { GLint ok; glGetProgramiv(program, GL_LINK_STATUS, &ok);
      if (!ok) { char log[1024]; glGetProgramInfoLog(program, sizeof(log), nullptr, log); LOGE("pano link: %s", log); } }
    glDeleteShader(s1); glDeleteShader(s2);
    mvp_loc = glGetUniformLocation(program, "uMVP");
    tex_loc = glGetUniformLocation(program, "uTex");
}

void panorama_bg::build_sphere() {
    if (vao) return;
    // Inverted sphere (inside-out) radius 50, so we see the texture from inside.
    float r = 50.0f;
    std::vector<float> verts;
    std::vector<unsigned short> idx;
    int latSegs = kSphereSegs / 2, lonSegs = kSphereSegs;
    for (int lat = 0; lat <= latSegs; lat++) {
        float v = (float)lat / latSegs;
        float phi = v * M_PI;  // 0..pi
        float y = cosf(phi), xr = sinf(phi);
        for (int lon = 0; lon <= lonSegs; lon++) {
            float u = (float)lon / lonSegs;
            float theta = u * 2.0f * M_PI;
            float x = xr * cosf(theta);
            float z = xr * sinf(theta);
            // UV: u maps around, v maps top-to-bottom
            verts.push_back(x * r); verts.push_back(y * r); verts.push_back(z * r);
            verts.push_back(u); verts.push_back(v);
        }
    }
    for (int lat = 0; lat < latSegs; lat++) {
        for (int lon = 0; lon < lonSegs; lon++) {
            int a = lat * (lonSegs + 1) + lon;
            int b = a + lonSegs + 1;
            // Flip winding for inside-out view
            idx.push_back(a); idx.push_back(b); idx.push_back(a + 1);
            idx.push_back(a + 1); idx.push_back(b); idx.push_back(b + 1);
        }
    }
    index_count = (int)idx.size();

    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_STATIC_DRAW);
    glGenBuffers(1, &ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size() * sizeof(unsigned short), idx.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)(3 * sizeof(float)));
    glBindVertexArray(0);
}

void panorama_bg::upload_texture(const std::string &path) {
    if (path == current_path && has_texture) return;
    current_path = path;
    if (path.empty()) { has_texture = false; return; }

    int w, h, ch;
    unsigned char *data = stbi_load(path.c_str(), &w, &h, &ch, 3);
    if (!data) { LOGE("panorama load failed: %s", path.c_str()); has_texture = false; return; }

    // Downscale if too large
    if (w > kMaxPanoramaW) {
        int newW = kMaxPanoramaW;
        int newH = (int)((float)h * newW / w);
        unsigned char *scaled = (unsigned char *)malloc(newW * newH * 3);
        if (stbir_resize_uint8_linear(data, w, h, 0,
            scaled, newW, newH, 0, (stbir_pixel_layout)3)) {
            stbi_image_free(data);
            data = scaled; w = newW; h = newH;
            LOGI("panorama downscaled to %dx%d", w, h);
        } else {
            free(scaled);
        }
    }

    if (texture) glDeleteTextures(1, &texture);
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, data);
    glBindTexture(GL_TEXTURE_2D, 0);
    tex_w = w; tex_h = h;
    // stbi_load uses stbi_image_free, but if we downscaled, data is from malloc.
    // stbi_image_free is just free() anyway, so this works for both.
    free(data);
    has_texture = true;
    LOGI("panorama texture loaded: %s %dx%d", path.c_str(), w, h);
}

void panorama_bg::init() {
    if (gl_ready) return;
    build_shader();
    build_sphere();
    gl_ready = true;
}

void panorama_bg::load(const std::string &path) {
    if (!gl_ready) init();
    upload_texture(path);
}

void panorama_bg::draw(const float *proj, const float *view) {
    if (!gl_ready || !has_texture) return;
    // Use view rotation only (strip translation) so the sphere is centered on viewer
    // The view matrix is column-major 4x4. Copy and zero out translation.
    float v[16];
    memcpy(v, view, sizeof(v));
    v[12] = 0; v[13] = 0; v[14] = 0; v[15] = 1;
    // MVP = proj * view_rot
    float mvp[16];
    // matrix multiply: mvp = proj * v (both column-major)
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += proj[i * 4 + k] * v[k * 4 + j];
            mvp[i * 4 + j] = s;
        }
    }

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glUseProgram(program);
    glUniformMatrix4fv(mvp_loc, 1, GL_FALSE, mvp);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glUniform1i(tex_loc, 0);
    glBindVertexArray(vao);
    glDrawElements(GL_TRIANGLES, index_count, GL_UNSIGNED_SHORT, 0);
    glBindVertexArray(0);
    glEnable(GL_DEPTH_TEST);
}
