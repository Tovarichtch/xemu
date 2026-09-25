/*
 * Geforce NV2A PGRAPH OpenGL Renderer
 *
 * Copyright (c) 2026 Réda Chérif-Touil
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "hw/xbox/nv2a/nv2a.h"
#include "renderer.h"

/* Self-test, once at context creation, of what the optimization pack asks
 * of a driver beyond its version string: separable stage programs, two
 * pipelines and a switch, stage uniforms, a textured draw, a texture copy.
 * A GL error or a wrong pixel keeps the pack off for the session. */

static const char *vs_src =
    "#version 400 core\n"
    "out gl_PerVertex { vec4 gl_Position; };\n"
    "uniform mat4 m;\n"
    "out vec4 v_color;\n"
    "void main() {\n"
    "  vec2 p = vec2((gl_VertexID == 1) ? 3.0 : -1.0,\n"
    "                (gl_VertexID == 2) ? 3.0 : -1.0);\n"
    "  gl_Position = vec4(p, 0.0, 1.0);\n"
    "  v_color = m[0];\n"
    "}\n";

static const char *gs_src =
    "#version 400 core\n"
    "layout(triangles) in;\n"
    "layout(triangle_strip, max_vertices = 3) out;\n"
    "in gl_PerVertex { vec4 gl_Position; } gl_in[];\n"
    "out gl_PerVertex { vec4 gl_Position; };\n"
    "in vec4 v_color[];\n"
    "out vec4 g_color;\n"
    "void main() {\n"
    "  for (int i = 0; i < 3; i++) {\n"
    "    gl_Position = gl_in[i].gl_Position;\n"
    "    g_color = v_color[i];\n"
    "    EmitVertex();\n"
    "  }\n"
    "  EndPrimitive();\n"
    "}\n";

static const char *fs_src =
    "#version 400 core\n"
    "uniform sampler2D tex;\n"
    "uniform vec4 tint;\n"
    "in vec4 g_color;\n"
    "out vec4 frag;\n"
    "void main() {\n"
    "  frag = texture(tex, vec2(0.5)) * tint + g_color;\n"
    "}\n";

/* One stage as a separable program, the way the pack builds them. On
 * failure the driver's link log lands in why. */
static GLuint probe_stage(GLenum kind, const char *src, char *why,
                          size_t why_len)
{
    GLuint sh;
    GLuint prog = pgraph_gl_separable_program(kind, src, &sh);
    glDetachShader(prog, sh);
    glDeleteShader(sh);
    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[256] = "";
        glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        for (char *c = log; *c; c++) {
            if (*c == '\n') {
                *c = ' ';
            }
        }
        snprintf(why, why_len, "separable link: %s", log);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

static bool probe_error(const char *step, char *why, size_t why_len)
{
    GLenum err = glGetError();
    if (err == GL_NO_ERROR) {
        return false;
    }
    snprintf(why, why_len, "%s: GL error 0x%x", step, err);
    return true;
}

bool nv2a_gl_probe_optimizations(char *why, size_t why_len)
{
    static const uint8_t texel[4] = { 64, 128, 192, 255 };
    const float zero[16] = { 0 };
    GLuint vs = 0, gs = 0, fs = 0, pipeline = 0, pipeline2 = 0, vao = 0;
    GLuint tex[2] = { 0, 0 }, fbo = 0;
    uint8_t px[4] = { 0 };
    bool ok = false;

    while (glGetError() != GL_NO_ERROR) {
    }
    snprintf(why, why_len, "ok");

    vs = probe_stage(GL_VERTEX_SHADER, vs_src, why, why_len);
    gs = vs ? probe_stage(GL_GEOMETRY_SHADER, gs_src, why, why_len) : 0;
    fs = gs ? probe_stage(GL_FRAGMENT_SHADER, fs_src, why, why_len) : 0;
    if (!vs || !gs || !fs) {
        goto out;
    }
    if (probe_error("separable link", why, why_len)) {
        goto out;
    }

    glGenProgramPipelines(1, &pipeline);
    glUseProgramStages(pipeline, GL_VERTEX_SHADER_BIT, vs);
    glUseProgramStages(pipeline, GL_GEOMETRY_SHADER_BIT, gs);
    glUseProgramStages(pipeline, GL_FRAGMENT_SHADER_BIT, fs);
    glUseProgram(0);
    glBindProgramPipeline(pipeline);
    glProgramUniformMatrix4fv(vs, glGetUniformLocation(vs, "m"), 1, GL_FALSE,
                              zero);
    glProgramUniform1i(fs, glGetUniformLocation(fs, "tex"), 0);
    glProgramUniform4f(fs, glGetUniformLocation(fs, "tint"), 1.0f, 1.0f,
                       1.0f, 1.0f);
    if (probe_error("pipeline", why, why_len)) {
        goto out;
    }

    glGenTextures(2, tex);
    for (int i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, tex[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 2, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, NULL);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex[0]);
    for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 2; x++) {
            glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, 1, 1, GL_RGBA,
                            GL_UNSIGNED_BYTE, texel);
        }
    }

    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex[1], 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        snprintf(why, why_len, "framebuffer incomplete");
        goto out;
    }
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glViewport(0, 0, 2, 2);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    if (probe_error("pipeline draw", why, why_len)) {
        goto out;
    }
    for (int i = 0; i < 4; i++) {
        if (abs((int)px[i] - (int)texel[i]) > 2) {
            snprintf(why, why_len, "pipeline draw gave %u,%u,%u,%u", px[0],
                     px[1], px[2], px[3]);
            goto out;
        }
    }

    /* Flip-time services switch pipelines between draws (texture touch,
     * seed builds): a second pipeline, a draw with it, then back. */
    glGenProgramPipelines(1, &pipeline2);
    glUseProgramStages(pipeline2, GL_VERTEX_SHADER_BIT, vs);
    glUseProgramStages(pipeline2, GL_GEOMETRY_SHADER_BIT, gs);
    glUseProgramStages(pipeline2, GL_FRAGMENT_SHADER_BIT, fs);
    glBindProgramPipeline(pipeline2);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindProgramPipeline(pipeline);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    memset(px, 0, sizeof(px));
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    if (probe_error("pipeline switch", why, why_len)) {
        goto out;
    }
    for (int i = 0; i < 4; i++) {
        if (abs((int)px[i] - (int)texel[i]) > 2) {
            snprintf(why, why_len, "pipeline switch gave %u,%u,%u,%u", px[0],
                     px[1], px[2], px[3]);
            goto out;
        }
    }

    /* The GPU blit path copies surfaces texture to texture. */
    if (epoxy_gl_version() >= 43 ||
        epoxy_has_gl_extension("GL_ARB_copy_image")) {
        glCopyImageSubData(tex[0], GL_TEXTURE_2D, 0, 0, 0, 0,
                           tex[1], GL_TEXTURE_2D, 0, 0, 0, 0, 2, 2, 1);
        memset(px, 0, sizeof(px));
        glReadPixels(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        if (probe_error("copy image", why, why_len)) {
            goto out;
        }
        if (memcmp(px, texel, 4) != 0) {
            snprintf(why, why_len, "copy image gave %u,%u,%u,%u", px[0],
                     px[1], px[2], px[3]);
            goto out;
        }
    }
    ok = true;

out:
    glBindProgramPipeline(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    /* GL ignores the name 0. */
    glDeleteFramebuffers(1, &fbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteTextures(2, tex);
    glDeleteProgramPipelines(1, &pipeline);
    glDeleteProgramPipelines(1, &pipeline2);
    glDeleteProgram(vs);
    glDeleteProgram(gs);
    glDeleteProgram(fs);
    while (glGetError() != GL_NO_ERROR) {
    }
    return ok;
}
