#include "lobby_map.h"
#include "gl_util.h"
#include "log.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>

static const char *kMapVtx = R"(#version 300 es
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
uniform mat4 uMVP;
uniform mat4 uModel;
uniform mat3 uNormalMat;
out vec3 vNormal;
out vec3 vWorldPos;
void main() {
    vNormal = normalize(uNormalMat * aNormal);
    vWorldPos = (uModel * vec4(aPos, 1.0)).xyz;
    gl_Position = uMVP * vec4(aPos, 1.0);
})";

static const char *kMapFrag = R"(#version 300 es
precision mediump float;
in vec3 vNormal;
in vec3 vWorldPos;
uniform vec3 uLightDir;
out vec4 frag;
void main() {
    vec3 n = normalize(vNormal);
    vec3 l = normalize(uLightDir);
    float diff = max(dot(n, l), 0.0);
    float amb = 0.35;
    vec3 base = vec3(0.45, 0.48, 0.52);
    vec3 col = base * (amb + diff * 0.65);
    frag = vec4(col, 1.0);
})";

lobby_map::lobby_map() {
    model_ = mat4Identity();
}

lobby_map::~lobby_map() {
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (prog_) glDeleteProgram(prog_);
}

void lobby_map::load(const char *obj_path) {
    FILE *f = fopen(obj_path, "r");
    if (!f) { LOGE("lobby_map: cannot open %s", obj_path); return; }

    std::vector<float> vx, vy, vz, vnx, vny, vnz;
    struct FaceIdx { int v, n; };
    std::vector<std::vector<FaceIdx>> faces;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        if (line[0]=='v' && line[1]==' ') {
            float x,y,z;
            if (sscanf(line+2, "%f %f %f", &x,&y,&z)==3) {
                vx.push_back(x); vy.push_back(y); vz.push_back(z);
            }
        } else if (line[0]=='v' && line[1]=='n' && line[2]==' ') {
            float x,y,z;
            if (sscanf(line+3, "%f %f %f", &x,&y,&z)==3) {
                vnx.push_back(x); vny.push_back(y); vnz.push_back(z);
            }
        } else if (line[0]=='f' && (line[1]==' '||line[1]=='\t')) {
            std::vector<FaceIdx> face;
            char *tok = strtok(line+2, " \t\r\n");
            while (tok) {
                int v_idx = atoi(tok);
                const char *s1 = strchr(tok, '/');
                int nn_idx = 0;
                if (s1) {
                    const char *s2 = strchr(s1+1, '/');
                    if (s2) nn_idx = atoi(s2+1);
                }
                if (v_idx != 0) {
                    FaceIdx fi;
                    fi.v = (v_idx > 0) ? v_idx-1 : (int)vx.size()+v_idx;
                    fi.n = (nn_idx > 0) ? nn_idx-1 : -1;
                    face.push_back(fi);
                }
                tok = strtok(nullptr, " \t\r\n");
            }
            if (face.size() >= 3) faces.push_back(std::move(face));
        }
    }
    fclose(f);

    std::vector<float> verts;
    for (const auto &face : faces) {
        for (int i=1; i+1<(int)face.size(); i++) {
            const FaceIdx *tri[3] = { &face[0], &face[i], &face[i+1] };
            for (int j=0; j<3; j++) {
                int v = tri[j]->v;
                if (v<0 || v>=(int)vx.size()) v = 0;
                verts.push_back(vx[v]);
                verts.push_back(vy[v]);
                verts.push_back(vz[v]);
                int nn = tri[j]->n;
                if (nn>=0 && nn<(int)vnx.size()) {
                    verts.push_back(vnx[nn]);
                    verts.push_back(vny[nn]);
                    verts.push_back(vnz[nn]);
                } else {
                    verts.push_back(0.0f);
                    verts.push_back(1.0f);
                    verts.push_back(0.0f);
                }
            }
        }
    }

    vert_count_ = (int)(verts.size() / 6);
    if (vert_count_ == 0) { LOGE("lobby_map: no vertices loaded"); return; }

    // shader
    prog_ = glCreateProgram();
    GLuint vs = compile(GL_VERTEX_SHADER, kMapVtx);
    GLuint fs = compile(GL_FRAGMENT_SHADER, kMapFrag);
    glAttachShader(prog_, vs);
    glAttachShader(prog_, fs);
    glLinkProgram(prog_);
    GLint ok=0; glGetProgramiv(prog_, GL_LINK_STATUS, &ok);
    if (!ok) { char log[512]; glGetProgramInfoLog(prog_, 512, nullptr, log); LOGE("lobby_map link: %s", log); }
    glDeleteShader(vs);
    glDeleteShader(fs);
    mvp_loc_ = glGetUniformLocation(prog_, "uMVP");
    model_loc_ = glGetUniformLocation(prog_, "uModel");
    normal_mat_loc_ = glGetUniformLocation(prog_, "uNormalMat");
    light_dir_loc_ = glGetUniformLocation(prog_, "uLightDir");

    // VAO/VBO
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, verts.size()*sizeof(float), verts.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6*sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6*sizeof(float), (void*)(3*sizeof(float)));
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    loaded_ = true;
    LOGI("lobby_map loaded: %d vertices", vert_count_);
}

void lobby_map::draw(const Mat4 &proj, const Mat4 &view) {
    if (!loaded_ || vert_count_ <= 0) return;

    Mat4 mvp = mat4Mul(proj, mat4Mul(view, model_));

    // normal matrix = upper 3x3 of model (no non-uniform scale so this is fine)
    float nm[9] = {
        model_.m[0], model_.m[1], model_.m[2],
        model_.m[4], model_.m[5], model_.m[6],
        model_.m[8], model_.m[9], model_.m[10]
    };

    glUseProgram(prog_);
    glUniformMatrix4fv(mvp_loc_, 1, GL_FALSE, mvp.m);
    glUniformMatrix4fv(model_loc_, 1, GL_FALSE, model_.m);
    glUniformMatrix3fv(normal_mat_loc_, 1, GL_FALSE, nm);
    float light[3] = { 0.4f, 0.8f, 0.3f };
    glUniform3fv(light_dir_loc_, 1, light);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);

    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, vert_count_);
    glBindVertexArray(0);

    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
}
