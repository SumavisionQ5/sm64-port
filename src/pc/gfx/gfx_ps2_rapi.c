#ifdef TARGET_PS2

#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <string.h>
#include <kernel.h>
#include <stdio.h>
#include <malloc.h>

#include <gsKit.h>
#include <gsInline.h>
#include <dmaKit.h>

#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif
#include <PR/gbi.h>

#include "gfx_rendering_api.h"
#include "gfx_cc.h"

#define MAX_TEXTURES 3072

// GS_SETREG_ALPHA(A, B, C, D, FIX)
// A = 0 = Cs
// B = 1 = Cd
// C = 0 = As
// D = 1 = Cd
// FIX =  128 (const alpha, unused)
// RGB = (A - B) * C + D = (Cs - Cd) * As + Cd -> normal blending, alpha 0-128
#define BMODE_BLEND GS_SETREG_ALPHA(0, 1, 0, 1, 128)

extern GSGLOBAL *gs_global;

enum TexMode {
    TEXMODE_MODULATE,
    TEXMODE_DECAL,
    TEXMODE_REPLACE,
};

typedef union TexCoord { 
    struct {
        float s, t;
    };
    u64 word;
} __attribute__((packed, aligned(8))) TexCoord;

typedef union ColorQ {
    struct {
        u8 r, g, b, a;
        float q;
    };
    u32 rgba;
    u64 word;
} __attribute__((packed, aligned(8))) ColorQ;

struct ShaderProgram {
    uint32_t shader_id;
    uint8_t num_inputs;
    bool used_textures[2];
    bool use_alpha;
    bool use_fog;
    bool alpha_test;
    enum TexMode tex_mode;
};

struct Texture {
    GSTEXTURE tex;
    uint32_t clamp_s;
    uint32_t clamp_t;
};

static struct ShaderProgram shader_program_pool[64];
static uint8_t shader_program_pool_size;
static struct ShaderProgram *cur_shader;

static struct Texture tex_pool[MAX_TEXTURES];
static uint32_t tex_pool_size;

static struct Texture *cur_tex[2];
static struct Texture *last_tex;

static bool z_test = true;
static bool z_mask = false;
static bool z_decal = false;
static bool do_blend = false;

static const uint64_t c_white = GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x00, 0x00);

static bool gfx_ps2_z_is_from_0_to_1(void) {
    return true;
}

static void gfx_ps2_unload_shader(struct ShaderProgram *old_prg) {

}

static void gfx_ps2_load_shader(struct ShaderProgram *new_prg) {
    cur_shader = new_prg;
}

static struct ShaderProgram *gfx_ps2_create_and_load_new_shader(uint32_t shader_id) {
    struct CCFeatures ccf;
    gfx_cc_get_features(shader_id, &ccf);

    struct ShaderProgram *prg = &shader_program_pool[shader_program_pool_size++];
    prg->shader_id = shader_id;
    prg->num_inputs = ccf.num_inputs;
    prg->used_textures[0] = ccf.used_textures[0];
    prg->used_textures[1] = ccf.used_textures[1];
    prg->use_alpha = ccf.opt_alpha;
    prg->use_fog = ccf.opt_fog;
    prg->alpha_test = ccf.opt_texture_edge;
    if (shader_id == 0x01045A00 || shader_id == 0x01200A00 || shader_id == 0x0000038D)
        prg->tex_mode = TEXMODE_DECAL;
    else
        prg->tex_mode = TEXMODE_MODULATE;

    gfx_ps2_load_shader(prg);

    return prg;
}

static struct ShaderProgram *gfx_ps2_lookup_shader(uint32_t shader_id) {
    for (size_t i = 0; i < shader_program_pool_size; i++) {
        if (shader_program_pool[i].shader_id == shader_id) {
            return &shader_program_pool[i];
        }
    }
    return NULL;
}

static void gfx_ps2_shader_get_info(struct ShaderProgram *prg, uint8_t *num_inputs, bool used_textures[2]) {
    *num_inputs = prg->num_inputs;
    used_textures[0] = prg->used_textures[0];
    used_textures[1] = prg->used_textures[1];
}


static uint32_t gfx_ps2_new_texture(void) {
    return tex_pool_size++;
}

static void gfx_ps2_select_texture(int tile, uint32_t texture_id) {
    cur_tex[tile] = last_tex = tex_pool + texture_id;
}

static void gfx_ps2_upload_texture(const uint8_t *rgba32_buf, int width, int height) {
    last_tex->tex.Width = width;
    last_tex->tex.Height = height;
    last_tex->tex.PSM = GS_PSM_CT32;
    last_tex->tex.Filter = GS_FILTER_NEAREST;

    const int in_size = gsKit_texture_size_ee(width, height, GS_PSM_CT32);
    const int out_size = gsKit_texture_size(width, height, GS_PSM_CT32);

    last_tex->tex.Mem = memalign(128, in_size);
    if (!last_tex->tex.Mem) {
        printf("gfx_ps2_upload_texture(%p, %d, %d) failed: out of RAM\n", rgba32_buf, width, height);
        return;
    }

    memcpy(last_tex->tex.Mem, rgba32_buf, in_size);
}

static inline uint32_t cm_to_ps2(const uint32_t val) {
    return (val & G_TX_CLAMP) ? GS_CMODE_CLAMP : GS_CMODE_REPEAT;
}

static void gfx_ps2_set_sampler_parameters(int tile, bool linear_filter, uint32_t cms, uint32_t cmt) {
    cur_tex[tile]->tex.Filter = linear_filter ? GS_FILTER_LINEAR : GS_FILTER_NEAREST;
    cur_tex[tile]->clamp_s = cm_to_ps2(cms);
    cur_tex[tile]->clamp_t = cm_to_ps2(cmt);
}

static void gfx_ps2_set_depth_test(bool depth_test) {
    z_test = depth_test;
}

static void gfx_ps2_set_depth_mask(bool z_upd) {
    z_mask = !z_upd;

    u64 *p_data = gsKit_heap_alloc(gs_global, 1, 16, GIF_AD);

    *p_data++ = GIF_TAG_AD(1);
    *p_data++ = GIF_AD;

    *p_data++ = GS_SETREG_ZBUF_1(gs_global->ZBuffer / 8192, gs_global->PSMZ, z_mask);
    *p_data++ = GS_ZBUF_1 + gs_global->PrimContext;
}

static void gfx_ps2_set_zmode_decal(bool zmode_decal) {
    z_decal = zmode_decal;
}

static void gfx_ps2_set_viewport(int x, int y, int width, int height) {
    // ???
}

static void gfx_ps2_set_scissor(int x, int y, int width, int height) {
    // ???
}

static void gfx_ps2_set_use_alpha(bool use_alpha) {
    do_blend = use_alpha;

    gs_global->PrimAlphaEnable = use_alpha;
    gs_global->PrimAlpha = use_alpha ? BMODE_BLEND : 0;
    gs_global->PABE = 0;

    u64 *p_data = gsKit_heap_alloc(gs_global, 1, 16, GIF_AD);

    *p_data++ = GIF_TAG_AD(1);
    *p_data++ = GIF_AD;

    *p_data++ = gs_global->PrimAlpha;
    *p_data++ = GS_ALPHA_1 + gs_global->PrimContext;
}

static inline void viewport_transform(float *v) {
    v[0] = v[0] * 320.f + 320.f;
    v[1] = v[1] * -224.f + 224.f;
    v[2] = (1.0f - v[2]) * 65535.f;
}

// these are exactly the same as their regular varieties, but the mapping type is set to ST (UV / w)

#define GIF_TAG_TRIANGLE_GORAUD_TEXTURED_ST_REGS(ctx) \
    ((u64)(GS_TEX0_1 + ctx) << 0 ) | \
    ((u64)(GS_PRIM)         << 4 ) | \
    ((u64)(GS_RGBAQ)        << 8 ) | \
    ((u64)(GS_ST)           << 12) | \
    ((u64)(GS_XYZ2)         << 16) | \
    ((u64)(GS_RGBAQ)        << 20) | \
    ((u64)(GS_ST)           << 24) | \
    ((u64)(GS_XYZ2)         << 28) | \
    ((u64)(GS_RGBAQ)        << 32) | \
    ((u64)(GS_ST)           << 36) | \
    ((u64)(GS_XYZ2)         << 40) | \
    ((u64)(GIF_NOP)         << 44)

static inline u32 lzw(u32 val) {
    u32 res;
    __asm__ __volatile__ ("   plzcw   %0, %1    " : "=r" (res) : "r" (val));
    return(res);
}

static inline void gsKit_set_tw_th(const GSTEXTURE *Texture, int *tw, int *th) {
    *tw = 31 - (lzw(Texture->Width) + 1);
    if(Texture->Width > (1<<*tw))
        (*tw)++;

    *th = 31 - (lzw(Texture->Height) + 1);
    if(Texture->Height > (1<<*th))
        (*th)++;
}


static void gsKit_prim_triangle_goraud_texture_3d_st(GSGLOBAL *gsGlobal, GSTEXTURE *Texture,
                float x1, float y1, int iz1, float u1, float v1,
                float x2, float y2, int iz2, float u2, float v2,
                float x3, float y3, int iz3, float u3, float v3,
                u64 color1, u64 color2, u64 color3) {
    gsKit_set_texfilter(gsGlobal, Texture->Filter);
    u64* p_store;
    u64* p_data;
    int qsize = 6;
    int bsize = 96;

    int tw, th;
    gsKit_set_tw_th(Texture, &tw, &th);

    int ix1 = gsKit_float_to_int_x(gsGlobal, x1);
    int ix2 = gsKit_float_to_int_x(gsGlobal, x2);
    int ix3 = gsKit_float_to_int_x(gsGlobal, x3);
    int iy1 = gsKit_float_to_int_y(gsGlobal, y1);
    int iy2 = gsKit_float_to_int_y(gsGlobal, y2);
    int iy3 = gsKit_float_to_int_y(gsGlobal, y3);

    TexCoord st1 = (TexCoord) { { u1, v1 } };
    TexCoord st2 = (TexCoord) { { u2, v2 } };
    TexCoord st3 = (TexCoord) { { u3, v3 } };

    p_store = p_data = gsKit_heap_alloc(gsGlobal, qsize, bsize, GSKIT_GIF_PRIM_TRIANGLE_TEXTURED);

    *p_data++ = GIF_TAG_TRIANGLE_GORAUD_TEXTURED(0);
    *p_data++ = GIF_TAG_TRIANGLE_GORAUD_TEXTURED_ST_REGS(gsGlobal->PrimContext);

    const int replace = cur_shader->tex_mode == TEXMODE_REPLACE;
    const int alpha = gsGlobal->PrimAlphaEnable;

    if (Texture->VramClut == 0) {
        *p_data++ = GS_SETREG_TEX0(Texture->Vram/256, Texture->TBW, Texture->PSM,
            tw, th, alpha, replace,
            0, 0, 0, 0, GS_CLUT_STOREMODE_NOLOAD);
    } else {
        *p_data++ = GS_SETREG_TEX0(Texture->Vram/256, Texture->TBW, Texture->PSM,
            tw, th, alpha, replace,
            Texture->VramClut/256, Texture->ClutPSM, 0, 0, GS_CLUT_STOREMODE_LOAD);
    }

    *p_data++ = GS_SETREG_PRIM( GS_PRIM_PRIM_TRIANGLE, 1, 1, gsGlobal->PrimFogEnable,
                gsGlobal->PrimAlphaEnable, gsGlobal->PrimAAEnable,
                0, gsGlobal->PrimContext, 0);


    *p_data++ = color1;
    *p_data++ = st1.word;
    *p_data++ = GS_SETREG_XYZ2( ix1, iy1, iz1 );

    *p_data++ = color2;
    *p_data++ = st2.word;
    *p_data++ = GS_SETREG_XYZ2( ix2, iy2, iz2 );

    *p_data++ = color3;
    *p_data++ = st3.word;
    *p_data++ = GS_SETREG_XYZ2( ix3, iy3, iz3 );
}

static inline void update_tests(const bool atest, const int ztest) {
    if (atest) {
        gs_global->Test->ATE = 1;
        gs_global->Test->ATST = 5; // ATEST_METHOD_LESS
        gs_global->Test->AREF = 0x50;
    } else {
        gs_global->Test->ATE = 0;
        gs_global->Test->ATST = 1; // ATEST_METHOD_ALLPASS
    }

    gs_global->Test->ZTST = ztest; // 1 is ALLPASS, 2 is GREATER

    u64 *p_data = gsKit_heap_alloc(gs_global, 1, 16, GIF_AD);

    *p_data++ = GIF_TAG_AD(1);
    *p_data++ = GIF_AD;

    *p_data++ = GS_SETREG_TEST(
        gs_global->Test->ATE,  gs_global->Test->ATST,
        gs_global->Test->AREF, gs_global->Test->AFAIL,
        gs_global->Test->DATE, gs_global->Test->DATM,
        gs_global->Test->ZTE,  gs_global->Test->ZTST
    );
    *p_data++ = GS_TEST_1 + gs_global->PrimContext;
}

static inline void draw_triangles_tex_col(float buf_vbo[], const size_t buf_vbo_num_tris, const size_t vtx_stride, const size_t tri_stride) {
    ColorQ c0 = (ColorQ) { { 0x80, 0x80, 0x80, 0x80, 1.f } };
    ColorQ c1 = (ColorQ) { { 0x80, 0x80, 0x80, 0x80, 1.f } };
    ColorQ c2 = (ColorQ) { { 0x80, 0x80, 0x80, 0x80, 1.f } };

    register float *v0, *v1, *v2;
    register float *p = buf_vbo;
    register size_t i;

    const int cofs = cur_shader->use_fog ? 7 : 6;
    for (i = 0; i < buf_vbo_num_tris; ++i, p += tri_stride) {
        v0 = p + 0;           viewport_transform(v0);
        v1 = v0 + vtx_stride; viewport_transform(v1);
        v2 = v1 + vtx_stride; viewport_transform(v2);
        c0.rgba = ((u32 *)v0)[cofs]; c0.q = v0[3];
        c1.rgba = ((u32 *)v1)[cofs]; c1.q = v1[3];
        c2.rgba = ((u32 *)v2)[cofs]; c2.q = v2[3];
        gsKit_prim_triangle_goraud_texture_3d_st(
            gs_global, &cur_tex[0]->tex,
            v0[0], v0[1], v0[2], v0[4], v0[5],
            v1[0], v1[1], v1[2], v1[4], v1[5],
            v2[0], v2[1], v2[2], v2[4], v2[5],
            c0.word, c1.word, c2.word
        );
    }
}

static inline void draw_triangles_tex(float buf_vbo[], const size_t buf_vbo_num_tris, const size_t vtx_stride, const size_t tri_stride) {
    ColorQ c0 = (ColorQ) { { 0x80, 0x80, 0x80, 0x80, 1.f } };
    ColorQ c1 = (ColorQ) { { 0x80, 0x80, 0x80, 0x80, 1.f } };
    ColorQ c2 = (ColorQ) { { 0x80, 0x80, 0x80, 0x80, 1.f } };

    register float *v0, *v1, *v2;
    register float *p = buf_vbo;
    register size_t i;

    for (i = 0; i < buf_vbo_num_tris; ++i, p += tri_stride) {
        v0 = p + 0;           viewport_transform(v0);
        v1 = v0 + vtx_stride; viewport_transform(v1);
        v2 = v1 + vtx_stride; viewport_transform(v2);
        c0.q = v0[3];
        c1.q = v1[3];
        c2.q = v2[3];
        gsKit_prim_triangle_goraud_texture_3d_st(
            gs_global, &cur_tex[0]->tex,
            v0[0], v0[1], v0[2], v0[4], v0[5],
            v1[0], v1[1], v1[2], v1[4], v1[5],
            v2[0], v2[1], v2[2], v2[4], v2[5],
            c0.word, c1.word, c2.word
        );
    }
}

static inline void draw_triangles_col(float buf_vbo[], const size_t buf_vbo_num_tris, const size_t vtx_stride, const size_t tri_stride, const size_t rgba_add) {
    ColorQ c0 = (ColorQ) { { 0x80, 0x80, 0x80, 0x80, 1.f } };
    ColorQ c1 = (ColorQ) { { 0x80, 0x80, 0x80, 0x80, 1.f } };
    ColorQ c2 = (ColorQ) { { 0x80, 0x80, 0x80, 0x80, 1.f } };

    register float *v0, *v1, *v2;
    register float *p = buf_vbo;
    register size_t i;

    const int cofs = (cur_shader->use_fog ? 5 : 4) + rgba_add;
    for (i = 0; i < buf_vbo_num_tris; ++i, p += tri_stride) {
        v0 = p + 0;           viewport_transform(v0);
        v1 = v0 + vtx_stride; viewport_transform(v1);
        v2 = v1 + vtx_stride; viewport_transform(v2);
        c0.rgba = ((u32 *)v0)[cofs]; c0.q = v0[3];
        c1.rgba = ((u32 *)v1)[cofs]; c1.q = v1[3];
        c2.rgba = ((u32 *)v2)[cofs]; c2.q = v2[3];
        gsKit_prim_triangle_gouraud_3d(
            gs_global,
            v0[0], v0[1], v0[2],
            v1[0], v1[1], v1[2],
            v2[0], v2[1], v2[2],
            c0.word, c1.word, c2.word
        );
    }
}

static inline void draw_triangles_tex_col_decal(float buf_vbo[], const size_t buf_vbo_num_tris, const size_t vtx_stride, const size_t tri_stride) {
    // draw color base, color offset is 2 because we skip UVs
    draw_triangles_col(buf_vbo, buf_vbo_num_tris, vtx_stride, tri_stride, 2);

    // alpha test on, blending on, ztest to GEQUAL
    gfx_ps2_set_use_alpha(true);
    update_tests(cur_shader->alpha_test, 2);

    // draw texture with blending on top, don't need to transform this time

    ColorQ c0 = (ColorQ) { { 0x80, 0x80, 0x80, 0x80, 1.f } };
    ColorQ c1 = (ColorQ) { { 0x80, 0x80, 0x80, 0x80, 1.f } };
    ColorQ c2 = (ColorQ) { { 0x80, 0x80, 0x80, 0x80, 1.f } };

    register float *v0, *v1, *v2;
    register float *p = buf_vbo;
    register size_t i;

    for (i = 0; i < buf_vbo_num_tris; ++i, p += tri_stride) {
        v0 = p + 0;
        v1 = v0 + vtx_stride;
        v2 = v1 + vtx_stride;
        c0.q = v0[3];
        c1.q = v1[3];
        c2.q = v2[3];
        gsKit_prim_triangle_goraud_texture_3d_st(
            gs_global, &cur_tex[0]->tex,
            v0[0], v0[1], v0[2], v0[4], v0[5],
            v1[0], v1[1], v1[2], v1[4], v1[5],
            v2[0], v2[1], v2[2], v2[4], v2[5],
            c0.word, c1.word, c2.word
        );
    }
}

static void gfx_ps2_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    const size_t vtx_stride = buf_vbo_len / (buf_vbo_num_tris * 3);
    const size_t tri_stride = vtx_stride * 3;

    const bool zge = z_test && z_decal;
    update_tests(cur_shader->alpha_test, z_test + zge + 1);

    if (cur_shader->used_textures[0]) {
        gsKit_set_clamp(gs_global, cur_tex[0]->clamp_s);
        gsKit_TexManager_bind(gs_global, &cur_tex[0]->tex);
        if (cur_shader->num_inputs) {
            if (cur_shader->tex_mode == TEXMODE_DECAL)
                draw_triangles_tex_col_decal(buf_vbo, buf_vbo_num_tris, vtx_stride, tri_stride);
            else
                draw_triangles_tex_col(buf_vbo, buf_vbo_num_tris, vtx_stride, tri_stride);
        } else {
            draw_triangles_tex(buf_vbo, buf_vbo_num_tris, vtx_stride, tri_stride);
        }
    } else if (cur_shader->num_inputs) {
        draw_triangles_col(buf_vbo, buf_vbo_num_tris, vtx_stride, tri_stride, 0);
    }
}

static void gfx_ps2_init(void) {
    gsKit_mode_switch(gs_global, GS_ONESHOT);
    gs_global->Test->ZTST = 2;
}

static void gfx_ps2_on_resize(void) {

}

static void gfx_ps2_start_frame(void) {
    const bool old_zmask = z_mask;
    if (old_zmask) gfx_ps2_set_depth_mask(true);
    gsKit_clear(gs_global, c_white);
    if (old_zmask) gfx_ps2_set_depth_mask(false);
}

static void gfx_ps2_end_frame(void) {
    gsKit_queue_exec(gs_global);
}

static void gfx_ps2_finish_render(void) {
    gsKit_queue_reset(gs_global->Os_Queue);
}

struct GfxRenderingAPI gfx_ps2_rapi = {
    gfx_ps2_z_is_from_0_to_1,
    gfx_ps2_unload_shader,
    gfx_ps2_load_shader,
    gfx_ps2_create_and_load_new_shader,
    gfx_ps2_lookup_shader,
    gfx_ps2_shader_get_info,
    gfx_ps2_new_texture,
    gfx_ps2_select_texture,
    gfx_ps2_upload_texture,
    gfx_ps2_set_sampler_parameters,
    gfx_ps2_set_depth_test,
    gfx_ps2_set_depth_mask,
    gfx_ps2_set_zmode_decal,
    gfx_ps2_set_viewport,
    gfx_ps2_set_scissor,
    gfx_ps2_set_use_alpha,
    gfx_ps2_draw_triangles,
    gfx_ps2_init,
    gfx_ps2_on_resize,
    gfx_ps2_start_frame,
    gfx_ps2_end_frame,
    gfx_ps2_finish_render
};


#endif // TARGET_PS2