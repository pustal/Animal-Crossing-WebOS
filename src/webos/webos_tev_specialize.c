/**
 * @file webos_tev_specialize.c
 * @brief Rewrites the port's TEV shader variants into straight-line GLSL.
 *
 * The port emulates the GameCube's texture environment (TEV) in one generic
 * fragment shader (pc/shaders/default.frag): loops over stages, switch
 * statements on the configuration, and dynamically indexed local arrays. For a
 * "specialised" variant pc_gx_tev.c only turns the configuration uniforms into
 * const arrays and relies on the driver to fold everything away. The Mali
 * driver on webOS 6 TVs (r9p0) does not: the GPU timing mode measured single
 * screen-sized draws at 20-70 ms, which made dialogue scenes run at 6-10 fps.
 *
 * This file reads the const block of a variant and generates a new main()
 * with every configuration choice resolved: one block per TEV stage, plain
 * swizzles for the swap tables, literal konst values, only the lights that are
 * enabled, and the alpha test and fog only when used. The rest of the source
 * (uniforms, inputs, helper functions) is kept unchanged, so uniform names and
 * locations stay what pc_gx_tev.c expects; unused declarations cost nothing.
 *
 * Sources without the const block (the uber shader, the vertex shader, the
 * port's other shaders) are left alone, as is any variant whose const block
 * does not parse exactly as expected.
 *
 * The generated code mirrors default.frag's main() statement for statement;
 * the comments below name the original function each part replaces.
 */
#include "webos_tev_specialize.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Maximum TEV stages in a variant (PC_GX_MAX_TEV_STAGES in the port). */
#define MAX_STAGES 3

/** The variant configuration, as written by pc_gx_tev.c's const block. */
typedef struct {
    int num_stages;
    int color_in[MAX_STAGES][4];
    int alpha_in[MAX_STAGES][4];
    int color_op[MAX_STAGES];
    int alpha_op[MAX_STAGES];
    int tc_src[MAX_STAGES];
    int bsc[MAX_STAGES][4];
    int out[MAX_STAGES][4];
    int swap[MAX_STAGES][2];
    int ksel[MAX_STAGES][3];
    int ind_cfg[MAX_STAGES][4];
    int ind_wrap[MAX_STAGES][3];
    int use_texture[MAX_STAGES];
    int lighting_cfg0[4];
    int lighting_cfg1[4];
    int swap_table[4][4];
    int alpha_ctrl[3];
    int num_ind_stages;
    int fog_enable;
} TevConfig;

/* ---- Parsing the const block ---- */

/**
 * Reads the integers of the declaration "const <type> <name>... = <value>;"
 * into @p out. Type names (ivec4, int[3], ...) are skipped, so only the
 * literal values are read.
 * @return 1 if exactly @p count integers were found.
 */
static int parse_const(const char *source, const char *name, int *out, int count)
{
    char needle[64];
    snprintf(needle, sizeof(needle), " %s", name);

    const char *line = source;
    while ((line = strstr(line, "const ")) != NULL) {
        const char *line_end = strchr(line, ';');
        if (!line_end)
            return 0;
        const char *found = strstr(line, needle);
        if (found && found < line_end) {
            const char *after_name = found + strlen(needle);
            if (*after_name == ' ' || *after_name == '[') {
                const char *p = strchr(after_name, '=');
                if (!p || p > line_end)
                    return 0;
                int n = 0;
                for (++p; p < line_end;) {
                    if (isalpha((unsigned char)*p) || *p == '_') {
                        while (p < line_end && (isalnum((unsigned char)*p) || *p == '_'))
                            ++p; /* type name such as ivec4 */
                    } else if (*p == '[') {
                        while (p < line_end && *p != ']')
                            ++p; /* array size such as [3] */
                    } else if (isdigit((unsigned char)*p) || (*p == '-' && isdigit((unsigned char)p[1]))) {
                        char *next = NULL;
                        const long value = strtol(p, &next, 10);
                        if (n >= count)
                            return 0;
                        out[n++] = (int)value;
                        p = next;
                    } else {
                        ++p;
                    }
                }
                return n == count;
            }
        }
        line = line_end + 1;
    }
    return 0;
}

static int parse_config(const char *source, TevConfig *config)
{
    return parse_const(source, "u_num_tev_stages", &config->num_stages, 1)
           && parse_const(source, "u_tev_color_in", &config->color_in[0][0], MAX_STAGES * 4)
           && parse_const(source, "u_tev_alpha_in", &config->alpha_in[0][0], MAX_STAGES * 4)
           && parse_const(source, "u_tev_color_op", config->color_op, MAX_STAGES)
           && parse_const(source, "u_tev_alpha_op", config->alpha_op, MAX_STAGES)
           && parse_const(source, "u_tev_tc_src", config->tc_src, MAX_STAGES)
           && parse_const(source, "u_tev_bsc", &config->bsc[0][0], MAX_STAGES * 4)
           && parse_const(source, "u_tev_out", &config->out[0][0], MAX_STAGES * 4)
           && parse_const(source, "u_tev_swap", &config->swap[0][0], MAX_STAGES * 2)
           && parse_const(source, "u_tev_ksel", &config->ksel[0][0], MAX_STAGES * 3)
           && parse_const(source, "u_tev_ind_cfg", &config->ind_cfg[0][0], MAX_STAGES * 4)
           && parse_const(source, "u_tev_ind_wrap", &config->ind_wrap[0][0], MAX_STAGES * 3)
           && parse_const(source, "u_use_texture", config->use_texture, MAX_STAGES)
           && parse_const(source, "u_lighting_cfg0", config->lighting_cfg0, 4)
           && parse_const(source, "u_lighting_cfg1", config->lighting_cfg1, 4)
           && parse_const(source, "u_swap_table", &config->swap_table[0][0], 16)
           && parse_const(source, "u_alpha_ctrl", config->alpha_ctrl, 3)
           && parse_const(source, "u_num_ind_stages", &config->num_ind_stages, 1)
           && parse_const(source, "u_fog_enable", &config->fog_enable, 1);
}

/** Rejects values outside the ranges the generator handles (they would index out of bounds). */
static int config_is_sane(const TevConfig *config)
{
    if (config->num_stages < 0 || config->num_stages > MAX_STAGES)
        return 0;
    for (int s = 0; s < config->num_stages; ++s) {
        if (config->tc_src[s] < 0 || config->tc_src[s] > 1)
            return 0;
        if (config->swap[s][0] < 0 || config->swap[s][0] > 3 || config->swap[s][1] < 0 || config->swap[s][1] > 3)
            return 0;
        if (config->ksel[s][0] < 0 || config->ksel[s][1] < 0)
            return 0;
    }
    for (int t = 0; t < 4; ++t)
        for (int c = 0; c < 4; ++c)
            if (config->swap_table[t][c] < 0 || config->swap_table[t][c] > 3)
                return 0;
    return 1;
}

/* ---- Output buffer ---- */

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
    int failed;
} Buffer;

static void emit(Buffer *buffer, const char *format, ...) __attribute__((format(printf, 2, 3)));

static void emit(Buffer *buffer, const char *format, ...)
{
    if (buffer->failed)
        return;
    for (;;) {
        va_list args;
        va_start(args, format);
        const size_t room = buffer->capacity - buffer->length;
        const int written = vsnprintf(buffer->data + buffer->length, room, format, args);
        va_end(args);
        if (written < 0) {
            buffer->failed = 1;
            return;
        }
        if ((size_t)written < room) {
            buffer->length += (size_t)written;
            return;
        }
        const size_t capacity = (buffer->capacity + (size_t)written + 1) * 2;
        char *grown = (char *)realloc(buffer->data, capacity);
        if (!grown) {
            buffer->failed = 1;
            return;
        }
        buffer->data = grown;
        buffer->capacity = capacity;
    }
}

/* ---- Expressions (replacing default.frag's helper functions) ---- */

/** Swizzle for a swap table entry, e.g. "rgba" or "rrra" (applySwap). */
static void swizzle(const int table[4], char out[5])
{
    static const char kChannels[4] = { 'r', 'g', 'b', 'a' };
    for (int i = 0; i < 4; ++i)
        out[i] = kChannels[table[i]];
    out[4] = '\0';
}

/** KONST colour for a selector (getKonstC). */
static void konst_color(int sel, char *out, size_t size)
{
    static const char kChannels[4] = { 'r', 'g', 'b', 'a' };
    if (sel <= 7)
        snprintf(out, size, "vec3(%d.0 / 8.0)", 8 - sel);
    else if (sel <= 11)
        snprintf(out, size, "vec3(0.0)");
    else if (sel <= 15)
        snprintf(out, size, "u_kcolor[%d].rgb", sel - 12);
    else {
        const int ki = (sel - 16) & 3;
        int ch = (sel - 16) >> 2;
        if (ch > 3)
            ch = 3; /* default: alpha */
        snprintf(out, size, "vec3(u_kcolor[%d].%c)", ki, kChannels[ch]);
    }
}

/** KONST alpha for a selector (getKonstA). */
static void konst_alpha(int sel, char *out, size_t size)
{
    static const char kChannels[4] = { 'r', 'g', 'b', 'a' };
    if (sel <= 7)
        snprintf(out, size, "(%d.0 / 8.0)", 8 - sel);
    else if (sel <= 15)
        snprintf(out, size, "0.0");
    else {
        const int ki = (sel - 16) & 3;
        int ch = (sel - 16) >> 2;
        if (ch > 3)
            ch = 3;
        snprintf(out, size, "u_kcolor[%d].%c", ki, kChannels[ch]);
    }
}

/** Colour operand for a GXTevColorArg (getTevC); stage values are named tex<s>/ras<s>/kc<s>. */
static void color_arg(int id, int stage, char *out, size_t size)
{
    switch (id) {
    case 0: snprintf(out, size, "prev.rgb"); break;
    case 1: snprintf(out, size, "vec3(prev.a)"); break;
    case 2: snprintf(out, size, "r0.rgb"); break;
    case 3: snprintf(out, size, "vec3(r0.a)"); break;
    case 4: snprintf(out, size, "r1.rgb"); break;
    case 5: snprintf(out, size, "vec3(r1.a)"); break;
    case 6: snprintf(out, size, "r2.rgb"); break;
    case 7: snprintf(out, size, "vec3(r2.a)"); break;
    case 8: snprintf(out, size, "tex%d.rgb", stage); break;
    case 9: snprintf(out, size, "vec3(tex%d.a)", stage); break;
    case 10: snprintf(out, size, "ras%d.rgb", stage); break;
    case 11: snprintf(out, size, "vec3(ras%d.a)", stage); break;
    case 12: snprintf(out, size, "vec3(1.0)"); break;
    case 13: snprintf(out, size, "vec3(0.5)"); break;
    case 14: snprintf(out, size, "kc%d", stage); break;
    default: snprintf(out, size, "vec3(0.0)"); break;
    }
}

/** Alpha operand for a GXTevAlphaArg (getTevA). */
static void alpha_arg(int id, int stage, char *out, size_t size)
{
    switch (id) {
    case 0: snprintf(out, size, "prev.a"); break;
    case 1: snprintf(out, size, "r0.a"); break;
    case 2: snprintf(out, size, "r1.a"); break;
    case 3: snprintf(out, size, "r2.a"); break;
    case 4: snprintf(out, size, "tex%d.a", stage); break;
    case 5: snprintf(out, size, "ras%d.a", stage); break;
    case 6: snprintf(out, size, "ka%d", stage); break;
    default: snprintf(out, size, "0.0"); break;
    }
}

/** Register written by a TEV stage output selector (writeToReg). */
static const char *register_name(int selector)
{
    switch (selector) {
    case 0: return "prev";
    case 1: return "r0";
    case 2: return "r1";
    default: return "r2";
    }
}

/** Boolean expression for an alpha comparison (alphaTest). */
static void alpha_test(int comp, const char *ref, char *out, size_t size)
{
    switch (comp) {
    case 0: snprintf(out, size, "false"); break;
    case 1: snprintf(out, size, "(prev.a < %s)", ref); break;
    case 2: snprintf(out, size, "(abs(prev.a - %s) < (0.5 / 255.0))", ref); break;
    case 3: snprintf(out, size, "(prev.a <= %s)", ref); break;
    case 4: snprintf(out, size, "(prev.a > %s)", ref); break;
    case 5: snprintf(out, size, "(abs(prev.a - %s) >= (0.5 / 255.0))", ref); break;
    case 6: snprintf(out, size, "(prev.a >= %s)", ref); break;
    default: snprintf(out, size, "true"); break;
    }
}

/* ---- main() generation ---- */

static void emit_rasterized_color(Buffer *b, const TevConfig *c)
{
    const int *cfg0 = c->lighting_cfg0;
    const int *cfg1 = c->lighting_cfg1;
    if (cfg0[3] == 0) {
        emit(b, "    vec4 rasColor = vec4(1.0);\n");
        return;
    }
    emit(b, "    vec4 rasColor;\n");
    const char *mat = cfg0[1] != 0 ? "v_color.rgb" : "u_chan_color[0].rgb";
    const char *amb = cfg0[2] != 0 ? "v_color.rgb" : "u_chan_color[1].rgb";
    if (cfg0[0] != 0) {
        emit(b, "    vec3 lightAccum = %s;\n", amb);
        for (int i = 0; i < 8; ++i)
            if ((cfg1[2] & (1 << i)) != 0)
                emit(b, "    lightAccum += clamp(dot(v_normal, normalize(u_light_pos[%d])), 0.0, 1.0) * u_light_color[%d].rgb;\n",
                     i, i);
        emit(b, "    rasColor.rgb = %s * clamp(lightAccum, 0.0, 1.0);\n", mat);
    } else {
        emit(b, "    rasColor.rgb = %s;\n", mat);
    }
    const char *mat_alpha = cfg1[1] != 0 ? "v_color.a" : "u_chan_color[0].a";
    if (cfg1[0] != 0)
        emit(b, "    rasColor.a = %s * u_chan_color[1].a;\n", mat_alpha);
    else
        emit(b, "    rasColor.a = %s;\n", mat_alpha);
}

static void emit_stage(Buffer *b, const TevConfig *c, int s)
{
    char swz_tex[5], swz_ras[5], kc[64], ka[64];
    swizzle(c->swap_table[c->swap[s][1]], swz_tex);
    swizzle(c->swap_table[c->swap[s][0]], swz_ras);
    konst_color(c->ksel[s][0], kc, sizeof(kc));
    konst_alpha(c->ksel[s][1], ka, sizeof(ka));

    emit(b, "    /* stage %d */\n", s);
    emit(b, "    vec4 tex%d = texColor%d.%s;\n", s, s, swz_tex);
    emit(b, "    vec4 ras%d = rasColor.%s;\n", s, swz_ras);
    emit(b, "    vec3 kc%d = %s;\n", s, kc);
    emit(b, "    float ka%d = %s;\n", s, ka);

    char a[48], bb[48], cc[48], d[48];
    color_arg(c->color_in[s][0], s, a, sizeof(a));
    color_arg(c->color_in[s][1], s, bb, sizeof(bb));
    color_arg(c->color_in[s][2], s, cc, sizeof(cc));
    color_arg(c->color_in[s][3], s, d, sizeof(d));
    emit(b, "    vec3 c%d = %s %c mix(%s, %s, %s);\n", s, d, c->color_op[s] == 1 ? '-' : '+', a, bb, cc);

    alpha_arg(c->alpha_in[s][0], s, a, sizeof(a));
    alpha_arg(c->alpha_in[s][1], s, bb, sizeof(bb));
    alpha_arg(c->alpha_in[s][2], s, cc, sizeof(cc));
    alpha_arg(c->alpha_in[s][3], s, d, sizeof(d));
    emit(b, "    float a%d = %s %c mix(%s, %s, %s);\n", s, d, c->alpha_op[s] == 1 ? '-' : '+', a, bb, cc);

    /* applyBSC: bias then scale, colour and alpha. */
    const int *bsc = c->bsc[s];
    if (bsc[0] == 1) emit(b, "    c%d += 0.5;\n", s);
    if (bsc[0] == 2) emit(b, "    c%d -= 0.5;\n", s);
    if (bsc[1] == 1) emit(b, "    c%d *= 2.0;\n", s);
    if (bsc[1] == 2) emit(b, "    c%d *= 4.0;\n", s);
    if (bsc[1] == 3) emit(b, "    c%d *= 0.5;\n", s);
    if (bsc[2] == 1) emit(b, "    a%d += 0.5;\n", s);
    if (bsc[2] == 2) emit(b, "    a%d -= 0.5;\n", s);
    if (bsc[3] == 1) emit(b, "    a%d *= 2.0;\n", s);
    if (bsc[3] == 2) emit(b, "    a%d *= 4.0;\n", s);
    if (bsc[3] == 3) emit(b, "    a%d *= 0.5;\n", s);

    /* writeToReg */
    const int *out = c->out[s];
    if (out[0] != 0)
        emit(b, "    %s.rgb = clamp(c%d, 0.0, 1.0);\n", register_name(out[2]), s);
    else
        emit(b, "    %s.rgb = c%d;\n", register_name(out[2]), s);
    if (out[1] != 0)
        emit(b, "    %s.a = clamp(a%d, 0.0, 1.0);\n", register_name(out[3]), s);
    else
        emit(b, "    %s.a = a%d;\n", register_name(out[3]), s);
}

static void emit_main(Buffer *b, const TevConfig *c)
{
    emit(b, "/* main() generated by webos_tev_specialize.c for this variant. */\n");
    emit(b, "void main() {\n");

    /* Texture coordinates, with the indirect chain only when the variant uses it. */
    const int any_indirect = c->num_ind_stages > 0;
    if (any_indirect)
        emit(b, "    vec2 ind_prev_offset = vec2(0.0);\n");
    for (int s = 0; s < c->num_stages; ++s) {
        const char *base = c->tc_src[s] == 0 ? "v_texcoord0" : "v_texcoord1";
        if (any_indirect && c->ind_cfg[s][1] != 0) {
            const int *ic = c->ind_cfg[s];
            const int *iw = c->ind_wrap[s];
            emit(b, "    vec2 stc%d = applyIndirect(ivec4(%d, %d, %d, %d), ivec3(%d, %d, %d), %s, ind_prev_offset, "
                    "v_texcoord0, v_texcoord1);\n",
                 s, ic[0], ic[1], ic[2], ic[3], iw[0], iw[1], iw[2], base);
            emit(b, "    ind_prev_offset = stc%d - %s;\n", s, base);
        } else {
            emit(b, "    vec2 stc%d = %s;\n", s, base);
        }
        if (c->use_texture[s] != 0)
            emit(b, "    vec4 texColor%d = texture(u_texture%d, stc%d);\n", s, s, s);
        else
            emit(b, "    vec4 texColor%d = vec4(1.0);\n", s);
    }

    emit_rasterized_color(b, c);

    emit(b, "    vec4 prev = u_tev_prev;\n");
    emit(b, "    vec4 r0 = u_tev_reg0;\n");
    emit(b, "    vec4 r1 = u_tev_reg1;\n");
    emit(b, "    vec4 r2 = u_tev_reg2;\n");
    for (int s = 0; s < c->num_stages; ++s)
        emit_stage(b, c, s);

    /* Alpha compare, only when either comparison is not ALWAYS. */
    if (c->alpha_ctrl[0] != 7 || c->alpha_ctrl[2] != 7) {
        char pass0[96], pass1[96];
        alpha_test(c->alpha_ctrl[0], "(float(u_alpha_refs.x) / 255.0)", pass0, sizeof(pass0));
        alpha_test(c->alpha_ctrl[2], "(float(u_alpha_refs.y) / 255.0)", pass1, sizeof(pass1));
        const char *op;
        switch (c->alpha_ctrl[1]) {
        case 0: op = "&&"; break;
        case 1: op = "||"; break;
        case 2: op = "!="; break;
        default: op = "=="; break;
        }
        emit(b, "    if (!(%s %s %s)) discard;\n", pass0, op, pass1);
    }

    emit(b, "    fragColor = prev;\n");
    if (c->fog_enable != 0) {
        emit(b, "    float fog_denom = max(u_fog_params.z - u_fog_params.y, 1e-6);\n");
        emit(b, "    float fog_factor = clamp((v_fog_z - u_fog_params.y) / fog_denom, 0.0, 1.0);\n");
        emit(b, "    fragColor.rgb = mix(fragColor.rgb, u_fog_color.rgb, fog_factor);\n");
    }
    emit(b, "}\n");
}

char *webos_tev_specialize(const char *source)
{
    /* Only specialised TEV variants carry this constant (see pc_gx_tev.c). */
    if (!strstr(source, "const int u_num_tev_stages"))
        return NULL;
    /* main() is the last function in default.frag; everything before it is kept. */
    const char *main_start = strstr(source, "\nvoid main()");
    if (!main_start)
        return NULL;

    TevConfig config;
    memset(&config, 0, sizeof(config));
    if (!parse_config(source, &config) || !config_is_sane(&config)) {
        fprintf(stderr, "[AC/webOS] TEV variant not specialised: unexpected const block\n");
        return NULL;
    }

    Buffer buffer = { NULL, 0, 0, 0 };
    const size_t kept = (size_t)(main_start - source) + 1;
    buffer.capacity = kept + 8192;
    buffer.data = (char *)malloc(buffer.capacity);
    if (!buffer.data)
        return NULL;
    memcpy(buffer.data, source, kept);
    buffer.length = kept;
    buffer.data[kept] = '\0';

    emit_main(&buffer, &config);
    if (buffer.failed) {
        free(buffer.data);
        return NULL;
    }
    return buffer.data;
}
