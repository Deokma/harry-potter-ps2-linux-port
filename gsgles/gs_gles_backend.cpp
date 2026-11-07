#define GSGLES_FRONTEND_STRIDE 640u // kHostFrameWidth in the runtime's gs_frontend.cpp
#include "gs_gles_backend.h"
#include "gsplay_gl.h"

extern "C"
{
#include "gl_dyn.h"
}

#include "runtime/gs/ps2_gs_psmct16.h"
#include "runtime/gs/ps2_gs_psmct32.h"
#include "runtime/gs/ps2_gs_psmt4.h"
#include "runtime/gs/ps2_gs_psmt8.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace
{
    double nowMs()
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    constexpr uint32_t kPageBytes = 8192u;
    constexpr uint32_t kPageCount = 512u;

    bool isIndexedPsm(uint8_t psm)
    {
        return psm == GS_PSM_T8 || psm == GS_PSM_T4 || psm == GS_PSM_T8H || psm == GS_PSM_T4HL || psm == GS_PSM_T4HH;
    }

    bool isFourBitPsm(uint8_t psm)
    {
        return psm == GS_PSM_T4 || psm == GS_PSM_T4HL || psm == GS_PSM_T4HH;
    }

    void pageSize(uint8_t psm, uint32_t &w, uint32_t &h)
    {
        switch (psm)
        {
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
            w = 64u;
            h = 64u;
            break;
        case GS_PSM_T8:
            w = 128u;
            h = 64u;
            break;
        case GS_PSM_T4:
            w = 128u;
            h = 128u;
            break;
        default: // the 32-bit layouts, and T8H / T4HL / T4HH which live in 32-bit pages
            w = 64u;
            h = 32u;
            break;
        }
    }

    uint8_t expand5(uint32_t v) { return static_cast<uint8_t>(v << 3); }

    // A texel as the GS sees it: RGB, and alpha from TEXA for the formats without eight bits of it.
    uint32_t texelToRgba(uint8_t psm, uint32_t raw, const GSTexaReg &texa)
    {
        switch (psm)
        {
        case GS_PSM_CT32:
            return raw;
        case GS_PSM_CT24:
        {
            const uint32_t rgb = raw & 0x00FFFFFFu;
            const uint32_t a = (texa.aem && rgb == 0u) ? 0u : texa.ta0;
            return rgb | (a << 24);
        }
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        {
            const uint32_t r = expand5(raw & 0x1Fu), g = expand5((raw >> 5) & 0x1Fu), b = expand5((raw >> 10) & 0x1Fu);
            const uint32_t rgb = r | (g << 8) | (b << 16);
            uint32_t a;
            if (raw & 0x8000u)
                a = texa.ta1;
            else
                a = (texa.aem && rgb == 0u) ? 0u : texa.ta0;
            return rgb | (a << 24);
        }
        default:
            return raw;
        }
    }

    uint64_t hashBytes(const uint8_t *data, size_t size)
    {
        // FNV-1a over 64-bit words, which is enough to tell re-uploads of the same picture apart.
        uint64_t h = 1469598103934665603ull;
        size_t i = 0;
        for (; i + 8u <= size; i += 8u)
        {
            uint64_t w;
            std::memcpy(&w, data + i, 8);
            h = (h ^ w) * 1099511628211ull;
        }
        for (; i < size; ++i)
            h = (h ^ data[i]) * 1099511628211ull;
        return h;
    }

    // Program key bits
    constexpr uint32_t kTex = 1u << 0;
    constexpr uint32_t kPal = 1u << 1;
    constexpr uint32_t kLinear = 1u << 2;
    constexpr uint32_t kTfxShift = 3;  // 2 bits
    constexpr uint32_t kTcc = 1u << 5;
    constexpr uint32_t kFog = 1u << 6;
    constexpr uint32_t kAtstShift = 7; // 3 bits, ATST; 1 (always) means none
    constexpr uint32_t kAinv = 1u << 10;
    constexpr uint32_t kRegion = 1u << 11;
    constexpr uint32_t kFbBlend = 1u << 12;
    constexpr uint32_t kBlendShift = 13; // 8 bits, ALPHA's A B C D
    constexpr uint32_t kDateShift = 21;  // 2 bits, 0 none, 1 DATM 0, 2 DATM 1
    constexpr uint32_t kPabe = 1u << 23;
    constexpr uint32_t kFba = 1u << 24;
    constexpr uint32_t kPremul = 1u << 25;
    constexpr uint32_t kModDst = 1u << 26;
    constexpr uint32_t kRt16 = 1u << 27; // the texture is a render target, sampled as CT16
    constexpr uint32_t kRt24 = 1u << 28; // ... as CT24

    const char *kVertexShader = R"(#version 300 es
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_stq;
layout(location = 2) in vec4 a_col;
layout(location = 3) in float a_fog;
uniform vec4 u_scale;
out vec3 v_stq;
out vec4 v_col;
out float v_fog;
void main()
{
    gl_Position = vec4(a_pos.xy * u_scale.xy + u_scale.zw, a_pos.z * 2.0 - 1.0, 1.0);
    v_stq = a_stq;
    v_col = a_col;
    v_fog = a_fog;
}
)";

    const char *kFragmentBody = R"(
precision highp int;
precision highp sampler2D;
in highp vec3 v_stq;
in vec4 v_col;
in float v_fog;
uniform sampler2D u_tex;
uniform sampler2D u_clut;
uniform highp vec2 u_texSize;
uniform highp vec2 u_uvScale;
uniform vec3 u_fog;
uniform float u_aref;
uniform float u_fix;
uniform float u_rtAlpha;
uniform vec3 u_ta;
uniform highp vec4 u_region;
uniform ivec2 u_regionMode;

#if TEX
vec4 fetchTexel(highp vec2 uv)
{
#if PAL
    float index = texture(u_tex, uv).r;
    return texelFetch(u_clut, ivec2(int(index * 255.0 + 0.5), 0), 0);
#else
    return texture(u_tex, uv);
#endif
}

vec4 sampleTexture(highp vec2 uv)
{
#if REGION
    highp vec2 t = floor(uv * u_texSize);
    if (u_regionMode.x == 2) t.x = clamp(t.x, u_region.x, u_region.y);
    if (u_regionMode.y == 2) t.y = clamp(t.y, u_region.z, u_region.w);
    if (u_regionMode.x == 3) t.x = float((int(t.x) & int(u_region.x)) | int(u_region.y));
    if (u_regionMode.y == 3) t.y = float((int(t.y) & int(u_region.z)) | int(u_region.w));
    uv = (t + 0.5) / u_texSize;
#endif
#if PAL && LINEAR
    highp vec2 f = uv * u_texSize - 0.5;
    highp vec2 b = floor(f);
    vec2 w = f - b;
    highp vec2 p0 = (b + 0.5) / u_texSize;
    highp vec2 p1 = (b + 1.5) / u_texSize;
    return mix(mix(fetchTexel(p0), fetchTexel(vec2(p1.x, p0.y)), w.x),
               mix(fetchTexel(vec2(p0.x, p1.y)), fetchTexel(p1), w.x), w.y);
#else
    return fetchTexel(uv);
#endif
}
#endif

bool alphaPasses(float a8)
{
    float r = floor(a8 + 0.5);
#if ATST == 0
    return false;
#elif ATST == 2
    return r < u_aref;
#elif ATST == 3
    return r <= u_aref;
#elif ATST == 4
    return r == u_aref;
#elif ATST == 5
    return r >= u_aref;
#elif ATST == 6
    return r > u_aref;
#elif ATST == 7
    return r != u_aref;
#else
    return true;
#endif
}

void main()
{
    vec4 c = v_col;
#if TEX
    vec4 t = sampleTexture(v_stq.xy / v_stq.z * u_uvScale);
#if RT16 || RT24
    bool black = u_ta.z > 0.5 && dot(t.rgb, vec3(1.0)) == 0.0;
#if RT16
    t.a = t.a >= 0.999 ? u_ta.y : (black ? 0.0 : u_ta.x);
#else
    t.a = black ? 0.0 : u_ta.x;
#endif
#else
    t.a *= u_rtAlpha;
#endif
    const float k = 255.0 / 128.0;
#if TFX == 0
    c.rgb = t.rgb * c.rgb * k;
#if TCC
    c.a = t.a * c.a * k;
#endif
#elif TFX == 1
    c.rgb = t.rgb;
#if TCC
    c.a = t.a;
#endif
#elif TFX == 2
    c.rgb = t.rgb * c.rgb * k + c.a;
#if TCC
    c.a = t.a + c.a;
#endif
#else
    c.rgb = t.rgb * c.rgb * k + c.a;
#if TCC
    c.a = t.a;
#endif
#endif
    c = min(c, vec4(1.0));
#endif
#if FOG
    c.rgb = mix(u_fog, c.rgb, v_fog);
#endif
    float a8 = c.a * 255.0;
#if AINV
    if (alphaPasses(a8)) discard;
#elif ATST != 1
    if (!alphaPasses(a8)) discard;
#endif
    float storedAlpha = min(a8 / 128.0, 1.0);
#if DATE != 0
    bool dstBit = LAST_COLOR.a >= 0.999;
#if DATE == 1
    if (dstBit) discard;
#else
    if (!dstBit) discard;
#endif
#endif
#if FBBLEND
    vec3 cs = c.rgb;
    vec3 cd = LAST_COLOR.rgb;
    float ad = LAST_COLOR.a;
    float factor = BLEND_C == 0 ? a8 / 128.0 : (BLEND_C == 1 ? ad : u_fix / 128.0);
    vec3 va = BLEND_A == 0 ? cs : (BLEND_A == 1 ? cd : vec3(0.0));
    vec3 vb = BLEND_B == 0 ? cs : (BLEND_B == 1 ? cd : vec3(0.0));
    vec3 vd = BLEND_D == 0 ? cs : (BLEND_D == 1 ? cd : vec3(0.0));
    vec3 blended = (va - vb) * factor + vd;
#if PABE
    if (a8 >= 128.0) c.rgb = clamp(blended, 0.0, 1.0);
#else
    c.rgb = clamp(blended, 0.0, 1.0);
#endif
#endif
#if PREMUL
    c.rgb = min(c.rgb * (BLEND_C == 0 ? a8 / 128.0 : u_fix / 128.0), vec3(1.0));
#elif MODDST
    float modFactor = BLEND_C == 0 ? a8 / 128.0 : u_fix / 128.0;
    c.rgb = vec3(max(modFactor - 1.0, 0.0));
    storedAlpha = min(modFactor, 1.0);
#endif
#if FBA
    storedAlpha = 1.0;
#endif
    o_col = vec4(c.rgb, storedAlpha);
}
)";

    const char *kPresentVertex = R"(#version 300 es
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_stq;
out vec2 v_uv;
void main()
{
    gl_Position = vec4(a_pos.xy, 0.0, 1.0);
    v_uv = a_stq.xy;
}
)";

    const char *kPresentFragment = R"(#version 300 es
precision mediump float;
in vec2 v_uv;
uniform sampler2D u_tex;
out vec4 o_col;
void main()
{
    o_col = texture(u_tex, v_uv);
}
)";

    unsigned compileShader(unsigned type, const std::string &source)
    {
        const unsigned shader = glCreateShader(type);
        const char *text = source.c_str();
        glShaderSource(shader, 1, &text, nullptr);
        glCompileShader(shader);
        int ok = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (!ok)
        {
            char log[2048];
            glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
            std::fprintf(stderr, "[gles] shader compile failed: %s\n", log);
        }
        return shader;
    }

    unsigned linkProgram(const std::string &vs, const std::string &fs)
    {
        const unsigned program = glCreateProgram();
        const unsigned v = compileShader(GL_VERTEX_SHADER, vs);
        const unsigned f = compileShader(GL_FRAGMENT_SHADER, fs);
        glAttachShader(program, v);
        glAttachShader(program, f);
        glLinkProgram(program);
        int ok = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &ok);
        if (!ok)
        {
            char log[2048];
            glGetProgramInfoLog(program, sizeof(log), nullptr, log);
            std::fprintf(stderr, "[gles] program link failed: %s\n", log);
        }
        glDeleteShader(v);
        glDeleteShader(f);
        return program;
    }

    // One coefficient of (A - B) * C + D: alpha + beta * C.
    struct Coefficient
    {
        int alpha = 0, beta = 0;
    };

    // GL factor for a non-negative coefficient, 0 if it has no factor.
    unsigned glFactor(const Coefficient &k, unsigned factorC, unsigned factorOneMinusC)
    {
        if (k.alpha == 0 && k.beta == 0)
            return GL_ZERO;
        if (k.alpha == 1 && k.beta == 0)
            return GL_ONE;
        if (k.alpha == 0 && k.beta == 1)
            return factorC;
        if (k.alpha == 1 && k.beta == -1)
            return factorOneMinusC;
        return 0xFFFFFFFFu;
    }

    // Maps a GS blend onto fixed function. False if GLES can't express it.
    bool mapBlend(uint32_t a, uint32_t b, uint32_t c, uint32_t d, unsigned &src, unsigned &dst, unsigned &equation)
    {
        Coefficient s, t; // coefficients of Cs and Cd
        if (a == 0) s.beta += 1;
        if (a == 1) t.beta += 1;
        if (b == 0) s.beta -= 1;
        if (b == 1) t.beta -= 1;
        if (d == 0) s.alpha += 1;
        if (d == 1) t.alpha += 1;

        const bool sNegative = s.alpha < 0 || (s.alpha == 0 && s.beta < 0);
        const bool tNegative = t.alpha < 0 || (t.alpha == 0 && t.beta < 0);
        if (sNegative && tNegative)
            return false;
        Coefficient sa = s, ta = t;
        if (sNegative)
        {
            sa.alpha = -s.alpha;
            sa.beta = -s.beta;
        }
        if (tNegative)
        {
            ta.alpha = -t.alpha;
            ta.beta = -t.beta;
        }
        const unsigned fc = c == 0 ? GL_SRC_ALPHA : (c == 1 ? GL_DST_ALPHA : GL_CONSTANT_ALPHA);
        const unsigned fm = c == 0 ? GL_ONE_MINUS_SRC_ALPHA : (c == 1 ? GL_ONE_MINUS_DST_ALPHA : GL_ONE_MINUS_CONSTANT_ALPHA);
        src = glFactor(sa, fc, fm);
        dst = glFactor(ta, fc, fm);
        if (src == 0xFFFFFFFFu || dst == 0xFFFFFFFFu)
            return false;
        equation = sNegative ? GL_FUNC_REVERSE_SUBTRACT : (tNegative ? GL_FUNC_SUBTRACT : GL_FUNC_ADD);
        return true;
    }
}

GSGlesBackend::GSGlesBackend() = default;

GSGlesBackend::~GSGlesBackend()
{
    if (m_glReady)
        destroyGlObjects();
}

void GSGlesBackend::Initialize(uint8_t *vram, uint32_t vramSize)
{
    m_vram = vram;
    m_mem.Initialize(vram, vramSize);
    ensureGlObjects();
}

void GSGlesBackend::Reset()
{
    m_mem.Reset();
    // Draws go, but GL objects that later frames will name still have to be made.
    m_commands.erase(std::remove_if(m_commands.begin(), m_commands.end(), [](const Command &c) { return c.type != Command::Type::GlOp; }), m_commands.end());
    m_vertices.clear();
    m_uploadPixels.clear();
    m_needResolve = true;
}

void GSGlesBackend::ensureGlObjects()
{
    if (m_glReady)
        return;
    m_glReady = true;
    m_fbfetchArm = gl_has_ext("GL_ARM_shader_framebuffer_fetch");
    m_fbfetch = m_fbfetchArm || gl_has_ext("GL_EXT_shader_framebuffer_fetch");

    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    const int stride = static_cast<int>(sizeof(HostVertex));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void *>(offsetof(HostVertex, x)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void *>(offsetof(HostVertex, s)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, reinterpret_cast<void *>(offsetof(HostVertex, r)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_UNSIGNED_BYTE, GL_TRUE, stride, reinterpret_cast<void *>(offsetof(HostVertex, fog)));

    m_presentProgram.id = linkProgram(kPresentVertex, kPresentFragment);
    m_presentProgram.uTex = glGetUniformLocation(m_presentProgram.id, "u_tex");
    glGenFramebuffers(2, m_copyFbo);
    glGenSamplers(1, &m_presentSampler);
    glSamplerParameteri(m_presentSampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glSamplerParameteri(m_presentSampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glSamplerParameteri(m_presentSampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(m_presentSampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    refillNames();
}

// GL names need the context but no storage, so a pool made on the context's thread lets recording hand them out.
// A frame that made thousands of new textures would run out, and those would draw as texture 0.
void GSGlesBackend::refillNames()
{
    constexpr size_t kTextures = 4096u, kOthers = 64u;
    auto fill = [](std::vector<unsigned> &pool, size_t want, void (*gen)(int, unsigned *))
    {
        if (pool.size() >= want / 2u)
            return;
        const size_t have = pool.size();
        pool.resize(want);
        gen(static_cast<int>(want - have), pool.data() + have);
    };
    fill(m_texNames, kTextures, [](int n, unsigned *out) { glGenTextures(n, out); });
    fill(m_samplerNames, kOthers, [](int n, unsigned *out) { glGenSamplers(n, out); });
    fill(m_rbNames, kOthers, [](int n, unsigned *out) { glGenRenderbuffers(n, out); });
}

unsigned GSGlesBackend::takeName(std::vector<unsigned> &pool, const char *what)
{
    if (pool.empty())
    {
        static bool told = false;
        if (!told)
            std::fprintf(stderr, "[gles] out of pooled %s names in one frame\n", what);
        told = true;
        return 0u;
    }
    const unsigned name = pool.back();
    pool.pop_back();
    return name;
}

void GSGlesBackend::pushOp(std::function<void()> op)
{
    Command cmd;
    cmd.type = Command::Type::GlOp;
    cmd.opIndex = static_cast<uint32_t>(m_glOps.size());
    m_glOps.push_back(std::move(op));
    m_commands.push_back(cmd);
}

void GSGlesBackend::setTarget(Command &cmd, RenderTarget *rt)
{
    cmd.rt = rt;
    cmd.rtTex = rt->tex;
    cmd.copyTex = rt->copyTex;
    cmd.rtW = rt->width;
    cmd.rtH = rt->height;
}

void GSGlesBackend::destroyGlObjects()
{
    for (auto &rt : m_rts)
    {
        glDeleteTextures(1, &rt->tex);
        if (rt->copyTex)
            glDeleteTextures(1, &rt->copyTex);
    }
    for (auto &d : m_depths)
        glDeleteRenderbuffers(1, &d.second->rb);
    for (auto &f : m_fbos)
        glDeleteFramebuffers(1, &f.second);
    for (auto &p : m_programs)
        glDeleteProgram(p.second->id);
    for (auto &t : m_contentTextures)
        glDeleteTextures(1, &t.second.tex);
    for (auto &t : m_samplers)
        glDeleteSamplers(1, &t.second);
    for (auto &c : m_cluts)
        glDeleteTextures(1, &c.second.tex);
    if (m_captureFbo)
    {
        glDeleteFramebuffers(1, &m_captureFbo);
        glDeleteTextures(1, &m_captureTex);
    }
    glDeleteFramebuffers(2, m_copyFbo);
    glDeleteProgram(m_presentProgram.id);
    glDeleteBuffers(1, &m_vbo);
    glDeleteVertexArrays(1, &m_vao);
}

GSGlesBackend::Program *GSGlesBackend::getProgram(uint32_t key)
{
    auto it = m_programs.find(key);
    if (it != m_programs.end())
        return it->second.get();

    std::string fs = "#version 300 es\n";
    const bool usesFetch = (key & (kFbBlend | (3u << kDateShift))) != 0u;
    if (usesFetch)
        fs += m_fbfetchArm ? "#extension GL_ARM_shader_framebuffer_fetch : require\n" : "#extension GL_EXT_shader_framebuffer_fetch : require\n";
    fs += "precision mediump float;\n";
    if (usesFetch && !m_fbfetchArm)
        fs += "#define LAST_COLOR o_col\ninout vec4 o_col;\n";
    else if (usesFetch)
        fs += "#define LAST_COLOR gl_LastFragColorARM\nout vec4 o_col;\n";
    else
        fs += "#define LAST_COLOR vec4(0.0)\nout vec4 o_col;\n";
    auto define = [&](const char *name, uint32_t value)
    {
        fs += "#define ";
        fs += name;
        fs += " ";
        fs += std::to_string(value);
        fs += "\n";
    };
    define("TEX", (key & kTex) ? 1 : 0);
    define("PAL", (key & kPal) ? 1 : 0);
    define("LINEAR", (key & kLinear) ? 1 : 0);
    define("TFX", (key >> kTfxShift) & 3u);
    define("TCC", (key & kTcc) ? 1 : 0);
    define("FOG", (key & kFog) ? 1 : 0);
    define("ATST", (key >> kAtstShift) & 7u);
    define("AINV", (key & kAinv) ? 1 : 0);
    define("REGION", (key & kRegion) ? 1 : 0);
    define("FBBLEND", (key & kFbBlend) ? 1 : 0);
    define("BLEND_A", (key >> kBlendShift) & 3u);
    define("BLEND_B", (key >> (kBlendShift + 2)) & 3u);
    define("BLEND_C", (key >> (kBlendShift + 4)) & 3u);
    define("BLEND_D", (key >> (kBlendShift + 6)) & 3u);
    define("DATE", (key >> kDateShift) & 3u);
    define("PABE", (key & kPabe) ? 1 : 0);
    define("FBA", (key & kFba) ? 1 : 0);
    define("PREMUL", (key & kPremul) ? 1 : 0);
    define("MODDST", (key & kModDst) ? 1 : 0);
    define("RT16", (key & kRt16) ? 1 : 0);
    define("RT24", (key & kRt24) ? 1 : 0);
    fs += kFragmentBody;

    auto owned = std::make_unique<Program>();
    Program *program = owned.get();
    m_programs[key] = std::move(owned);
    pushOp([program, fs = std::move(fs)]()
           {
               program->id = linkProgram(kVertexShader, fs);
               program->uScale = glGetUniformLocation(program->id, "u_scale");
               program->uTex = glGetUniformLocation(program->id, "u_tex");
               program->uClut = glGetUniformLocation(program->id, "u_clut");
               program->uTexSize = glGetUniformLocation(program->id, "u_texSize");
               program->uUvScale = glGetUniformLocation(program->id, "u_uvScale");
               program->uFog = glGetUniformLocation(program->id, "u_fog");
               program->uAref = glGetUniformLocation(program->id, "u_aref");
               program->uFix = glGetUniformLocation(program->id, "u_fix");
               program->uRtAlpha = glGetUniformLocation(program->id, "u_rtAlpha");
               program->uTa = glGetUniformLocation(program->id, "u_ta");
               program->uRegion = glGetUniformLocation(program->id, "u_region");
               program->uRegionMode = glGetUniformLocation(program->id, "u_regionMode");
               glUseProgram(program->id);
               if (program->uTex >= 0)
                   glUniform1i(program->uTex, 0);
               if (program->uClut >= 0)
                   glUniform1i(program->uClut, 1);
           });
    return program;
}

GSGlesBackend::RenderTarget *GSGlesBackend::findRenderTarget(uint32_t fbp, uint32_t fbw)
{
    for (auto &rt : m_rts)
        if (rt->fbp == fbp && (fbw == 0u || rt->fbw == fbw))
            return rt.get();
    return nullptr;
}

void GSGlesBackend::loadRtFromVram(RenderTarget &rt, int x, int y, int w, int h, std::vector<uint8_t> &out)
{
    out.resize(static_cast<size_t>(w) * h * 4u);
    const uint32_t base = rt.fbp * 32u;
    for (int j = 0; j < h; ++j)
    {
        uint8_t *dst = out.data() + static_cast<size_t>(j) * w * 4u;
        for (int i = 0; i < w; ++i, dst += 4)
        {
            const uint32_t raw = m_mem.ReadVram(rt.psm, base, rt.fbw, x + i, y + j);
            // Alpha as stored in the target: CT24 has none, so it reads as 0x80; CT16's bit picks 0x80 or 0.
            GSTexaReg texa{};
            texa.ta0 = rt.psm == GS_PSM_CT24 ? 0x80 : 0x00;
            texa.ta1 = 0x80;
            const uint32_t c = texelToRgba(rt.psm, raw, texa);
            dst[0] = static_cast<uint8_t>(c);
            dst[1] = static_cast<uint8_t>(c >> 8);
            dst[2] = static_cast<uint8_t>(c >> 16);
            const uint32_t a = (c >> 24) * 2u;
            dst[3] = static_cast<uint8_t>(a > 255u ? 255u : a);
        }
    }
}

GSGlesBackend::RenderTarget *GSGlesBackend::getRenderTarget(uint32_t fbp, uint32_t fbw, uint8_t psm, int needHeight)
{
    if (fbw == 0u)
        fbw = 1u;
    RenderTarget *rt = findRenderTarget(fbp, fbw);
    if (rt && needHeight <= rt->height)
        return rt;

    const int width = static_cast<int>(fbw) * 64;
    const int height = needHeight <= 512 ? 512 : 1024;
    if (rt)
    {
        // A draw past the bottom: the render target grows, keeping what is there, at this point in the stream.
        const unsigned tex = takeName(m_texNames, "texture");
        const unsigned old = rt->tex;
        const int oldW = rt->width, oldH = rt->height;
        pushOp([this, tex, old, oldW, oldH, width, height]()
               {
                   glActiveTexture(GL_TEXTURE0);
                   glBindTexture(GL_TEXTURE_2D, tex);
                   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                   glBindFramebuffer(GL_READ_FRAMEBUFFER, m_copyFbo[0]);
                   glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, old, 0);
                   glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_copyFbo[1]);
                   glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
                   glDisable(GL_SCISSOR_TEST);
                   glBlitFramebuffer(0, 0, oldW, oldH, 0, 0, oldW, oldH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
                   for (auto it = m_fbos.begin(); it != m_fbos.end();)
                   {
                       if (static_cast<unsigned>(it->first >> 32) == old)
                       {
                           glDeleteFramebuffers(1, &it->second);
                           it = m_fbos.erase(it);
                       }
                       else
                           ++it;
                   }
                   glDeleteTextures(1, &old);
               });
        rt->tex = tex;
        rt->height = height;
        return rt;
    }

    auto created = std::make_unique<RenderTarget>();
    created->fbp = fbp;
    created->fbw = fbw;
    created->psm = psm;
    created->width = width;
    created->height = height;
    created->tex = takeName(m_texNames, "texture");
    // What the dump (or anything before the first draw) left in local memory is the starting picture.
    std::vector<uint8_t> pixels;
    loadRtFromVram(*created, 0, 0, width, height, pixels);
    pushOp([tex = created->tex, width, height, pixels = std::move(pixels)]()
           {
               glActiveTexture(GL_TEXTURE0);
               glBindTexture(GL_TEXTURE_2D, tex);
               glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
               glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
               glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
               glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
               glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
           });
    m_rts.push_back(std::move(created));
    return m_rts.back().get();
}

GSGlesBackend::DepthBuffer *GSGlesBackend::getDepthBuffer(uint32_t zbp, int width, int height)
{
    const uint64_t key = (static_cast<uint64_t>(zbp) << 32) | (static_cast<uint32_t>(width) << 16) | static_cast<uint32_t>(height);
    auto it = m_depths.find(key);
    if (it != m_depths.end())
        return it->second.get();
    auto depth = std::make_unique<DepthBuffer>();
    depth->width = width;
    depth->height = height;
    depth->rb = takeName(m_rbNames, "renderbuffer");
    pushOp([rb = depth->rb, width, height]()
           {
               glBindRenderbuffer(GL_RENDERBUFFER, rb);
               glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
           });
    DepthBuffer *raw = depth.get();
    m_depths[key] = std::move(depth);
    return raw;
}

unsigned GSGlesBackend::getFramebuffer(unsigned tex, DepthBuffer *depth, int w, int h)
{
    const uint64_t key = (static_cast<uint64_t>(tex) << 32) | (depth ? depth->rb : 0u);
    auto it = m_fbos.find(key);
    if (it != m_fbos.end())
        return it->second;
    unsigned fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    if (depth)
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depth->rb);
    const unsigned status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
        std::fprintf(stderr, "[gles] framebuffer incomplete: 0x%x (%dx%d)\n", status, w, h);
    m_fbos[key] = fbo;
    return fbo;
}

void GSGlesBackend::texturePages(uint32_t tbp0, uint32_t tbw, uint8_t psm, uint32_t w, uint32_t h, uint32_t &first, uint32_t &count) const
{
    uint32_t pw, ph;
    pageSize(psm, pw, ph);
    const uint32_t widthUnits = tbw ? tbw : 1u;
    const uint32_t pagesPerRow = std::max<uint32_t>(1u, (widthUnits * 64u + pw - 1u) / pw);
    const uint32_t rows = (h + ph - 1u) / ph;
    first = tbp0 / 32u;
    count = rows * std::max(pagesPerRow, (w + pw - 1u) / pw) + ((tbp0 & 31u) ? 1u : 0u);
}

uint64_t GSGlesBackend::pageSignature(uint32_t tbp0, uint32_t tbw, uint8_t psm, uint32_t w, uint32_t h) const
{
    uint32_t first, count;
    texturePages(tbp0, tbw, psm, w, h, first, count);
    uint64_t sig = 0;
    for (uint32_t i = 0; i < count && first + i < kPageCount; ++i)
        sig = sig * 31u + m_pageGen[first + i];
    return sig;
}

// Reads a texture out of swizzled local memory into m_decode: one byte per texel for the indexed
// formats, RGBA8 for the rest. The address functions are inline and take no lock.
void GSGlesBackend::decodeTexture(const GSTex0Reg &t, const GSTexaReg &texa, uint32_t w, uint32_t h)
{
    const uint8_t *vram = m_vram;
    constexpr uint32_t kMask = 4u * 1024u * 1024u - 1u;
    const uint32_t tbp = t.tbp0, tbw = t.tbw;
    uint8_t *out = m_decode.data();
    uint32_t *out32 = reinterpret_cast<uint32_t *>(m_decode.data());
    switch (t.psm)
    {
    case GS_PSM_T8:
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
                *out++ = vram[GSPSMT8::addrPSMT8(tbp, tbw, x, y) & kMask];
        break;
    case GS_PSM_T4:
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
            {
                const uint32_t a = GSPSMT4::addrPSMT4(tbp, tbw, x, y);
                const uint8_t b = vram[(a >> 1) & kMask];
                *out++ = (a & 1u) ? (b >> 4) : (b & 0x0Fu);
            }
        break;
    case GS_PSM_T8H:
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
                *out++ = vram[(GSPSMCT32::addrPSMCT32(tbp, tbw, x, y) + 3u) & kMask];
        break;
    case GS_PSM_T4HL:
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
                *out++ = vram[(GSPSMCT32::addrPSMCT32(tbp, tbw, x, y) + 3u) & kMask] & 0x0Fu;
        break;
    case GS_PSM_T4HH:
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
                *out++ = vram[(GSPSMCT32::addrPSMCT32(tbp, tbw, x, y) + 3u) & kMask] >> 4;
        break;
    case GS_PSM_CT32:
    case GS_PSM_CT24:
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
            {
                uint32_t raw;
                std::memcpy(&raw, vram + (GSPSMCT32::addrPSMCT32(tbp, tbw, x, y) & (kMask & ~3u)), 4);
                *out32++ = texelToRgba(t.psm, raw, texa);
            }
        break;
    case GS_PSM_CT16:
    case GS_PSM_CT16S:
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
            {
                const uint32_t a = t.psm == GS_PSM_CT16 ? GSPSMCT16::addrPSMCT16(tbp, tbw, x, y) : GSPSMCT16::addrPSMCT16S(tbp, tbw, x, y);
                uint16_t raw;
                std::memcpy(&raw, vram + (a & (kMask & ~1u)), 2);
                *out32++ = texelToRgba(t.psm, raw, texa);
            }
        break;
    default:
        // depth formats and anything unusual, through the slow path
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
                *out32++ = texelToRgba(t.psm, m_mem.ReadVram(t.psm, tbp, tbw, x, y), texa);
        break;
    }

    static const bool verify = std::getenv("GSPLAY_VERIFY") != nullptr;
    if (verify)
    {
        const bool indexed = isIndexedPsm(t.psm);
        uint32_t bad = 0;
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
            {
                const uint32_t slow = m_mem.ReadVram(t.psm, tbp, tbw, x, y);
                const size_t i = static_cast<size_t>(y) * w + x;
                if (indexed ? (m_decode[i] != static_cast<uint8_t>(slow))
                            : (reinterpret_cast<const uint32_t *>(m_decode.data())[i] != texelToRgba(t.psm, slow, texa)))
                    ++bad;
            }
        if (bad)
            std::printf("[gles] decode mismatch: psm %02x tbp %x tbw %u %ux%u, %u texels\n", t.psm, tbp, tbw, w, h, bad);
    }
}

void GSGlesBackend::bumpPages(uint32_t dbp, uint32_t dbw, uint8_t psm, uint32_t y, uint32_t h)
{
    uint32_t pw, ph;
    pageSize(psm, pw, ph);
    const uint32_t widthUnits = dbw ? dbw : 1u;
    const uint32_t pagesPerRow = std::max<uint32_t>(1u, (widthUnits * 64u + pw - 1u) / pw);
    const uint32_t firstRow = y / ph;
    const uint32_t lastRow = (y + (h ? h : 1u) - 1u) / ph;
    const uint32_t first = dbp / 32u + firstRow * pagesPerRow;
    const uint32_t end = dbp / 32u + (lastRow + 1u) * pagesPerRow + ((dbp & 31u) ? 1u : 0u);
    for (uint32_t p = first; p < end && p < kPageCount; ++p)
        ++m_pageGen[p];
}

GSGlesBackend::TextureEntry *GSGlesBackend::resolveTexture(const GSDrawState &state)
{
    const GSTex0Reg &t = state.context.tex0;
    const uint32_t w = 1u << std::min<uint32_t>(t.tw, 10u);
    const uint32_t h = 1u << std::min<uint32_t>(t.th, 10u);
    const bool indexed = isIndexedPsm(t.psm);
    ++m_stats.texResolves;

    uint64_t key = (static_cast<uint64_t>(t.tbp0) << 32) | (static_cast<uint64_t>(t.tbw) << 24) |
                   (static_cast<uint64_t>(t.psm) << 16) | (static_cast<uint64_t>(t.tw) << 8) | t.th;
    if (!indexed && t.psm != GS_PSM_CT32)
    {
        // TEXA is baked into these formats' alpha
        key ^= (static_cast<uint64_t>(state.texa.ta0) << 40) ^ (static_cast<uint64_t>(state.texa.ta1) << 48) ^
               (static_cast<uint64_t>(state.texa.aem) << 56);
    }
    TextureSlot &slot = m_textureSlots[key];
    const uint64_t sig = pageSignature(t.tbp0, t.tbw, t.psm, w, h);
    if (slot.current && slot.pageSig == sig)
    {
        ++m_stats.texPageHits;
        slot.current->lastUsed = m_frameIndex;
        return slot.current;
    }

    // Something wrote to these pages. The game streams textures through the same addresses, so what is
    // there now may be a picture already seen: look it up by the raw pages' content, which is far cheaper
    // than decoding, and needs no upload at all when it hits. A false miss (something else changed on a
    // shared page) only costs a decode.
    const double t0 = nowMs();
    uint32_t firstPage, pageCount;
    texturePages(t.tbp0, t.tbw, t.psm, w, h, firstPage, pageCount);
    if (firstPage + pageCount > kPageCount)
        pageCount = firstPage < kPageCount ? kPageCount - firstPage : 0u;
    const uint64_t rawHash = hashBytes(m_vram + firstPage * kPageBytes, pageCount * kPageBytes);
    const uint64_t contentKey = rawHash ^ (key * 0x9E3779B97F4A7C15ull);
    const double tRaw = nowMs();
    m_stats.hashMs += tRaw - t0;
    slot.pageSig = sig;
    auto found = m_contentTextures.find(contentKey);
    if (found != m_contentTextures.end())
    {
        ++m_stats.texRawHits;
        found->second.lastUsed = m_frameIndex;
        slot.current = &found->second;
        return slot.current;
    }

    const size_t bytes = static_cast<size_t>(w) * h * (indexed ? 1u : 4u);
    if (m_decode.size() < bytes)
        m_decode.resize(bytes);
    decodeTexture(t, state.texa, w, h);
    const double t1 = nowMs();
    m_stats.decodeMs += t1 - tRaw;

    TextureEntry &entry = m_contentTextures[contentKey];
    entry.tex = takeName(m_texNames, "texture");
    pushOp([tex = entry.tex, w, h, indexed, pixels = std::vector<uint8_t>(m_decode.begin(), m_decode.begin() + bytes)]()
           {
               glActiveTexture(GL_TEXTURE0);
               glBindTexture(GL_TEXTURE_2D, tex);
               glTexImage2D(GL_TEXTURE_2D, 0, indexed ? GL_R8 : GL_RGBA8, w, h, 0, indexed ? GL_RED : GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
           });
    entry.w = static_cast<int>(w);
    entry.h = static_cast<int>(h);
    entry.indexed = indexed;
    entry.bytes = bytes;
    entry.lastUsed = m_frameIndex;
    m_textureBytes += bytes;
    slot.current = &entry;
    ++m_stats.texUploads;
    m_stats.texBytes += bytes;
    m_stats.uploadMs += nowMs() - t1;
    return &entry;
}

// Old textures go when the cache is over budget, between frames, when nothing recorded refers to them.
void GSGlesBackend::evictTextures()
{
    constexpr size_t kBudget = 96u * 1024u * 1024u;
    if (m_textureBytes <= kBudget)
        return;
    for (auto it = m_contentTextures.begin(); it != m_contentTextures.end();)
    {
        if (m_frameIndex - it->second.lastUsed > 120u)
        {
            pushOp([tex = it->second.tex]() { glDeleteTextures(1, &tex); });
            m_textureBytes -= it->second.bytes;
            it = m_contentTextures.erase(it);
        }
        else
            ++it;
    }
    m_textureSlots.clear();
}

unsigned GSGlesBackend::getSampler(unsigned filter, unsigned wrapS, unsigned wrapT)
{
    const uint64_t key = (static_cast<uint64_t>(filter) << 40) | (static_cast<uint64_t>(wrapS) << 20) | wrapT;
    auto it = m_samplers.find(key);
    if (it != m_samplers.end())
        return it->second;
    const unsigned sampler = takeName(m_samplerNames, "sampler");
    pushOp([sampler, filter, wrapS, wrapT]()
           {
               glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, static_cast<int>(filter));
               glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, static_cast<int>(filter));
               glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, static_cast<int>(wrapS));
               glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, static_cast<int>(wrapT));
           });
    m_samplers[key] = sampler;
    return sampler;
}

unsigned GSGlesBackend::resolveClut(const GSTex0Reg &tex0, const GSTexaReg &texa)
{
    const bool sixteenBit = tex0.cpsm == GS_PSM_CT16 || tex0.cpsm == GS_PSM_CT16S;
    const uint32_t csaMask = sixteenBit ? 0x1Fu : 0x0Fu;
    const uint32_t clutBase = (static_cast<uint32_t>(tex0.csa) & csaMask) << 4u;
    const bool fourBit = isFourBitPsm(tex0.psm);

    uint64_t key = m_clutRamHash ^ ((static_cast<uint64_t>(clutBase) | (static_cast<uint64_t>(tex0.cpsm) << 16) |
                                     (static_cast<uint64_t>(tex0.csm) << 24) | (static_cast<uint64_t>(fourBit) << 32)) *
                                    0x9E3779B97F4A7C15ull);
    if (tex0.cpsm != GS_PSM_CT32)
        key ^= (static_cast<uint64_t>(texa.ta0) << 40) ^ (static_cast<uint64_t>(texa.ta1) << 48) ^ (static_cast<uint64_t>(texa.aem) << 56);
    auto it = m_clutKeys.find(key);
    if (it != m_clutKeys.end())
    {
        it->second->lastUsed = m_frameIndex;
        return it->second->tex;
    }

    std::vector<uint32_t> palette(256u, 0u);
    const uint32_t entries = fourBit ? 16u : 256u;
    for (uint32_t i = 0; i < entries; ++i)
    {
        uint32_t index = (clutBase + i) & (sixteenBit ? 0x1FFu : 0xFFu);
        if (!sixteenBit && tex0.csm == 0u && !fourBit)
            index = std::min((i & 0xF0u) + clutBase, 240u) + (i & 0x0Fu);
        uint32_t raw;
        if (sixteenBit)
            raw = m_clutRam[index];
        else
            raw = static_cast<uint32_t>(m_clutRam[index]) | (static_cast<uint32_t>(m_clutRam[index + 256u]) << 16);
        palette[i] = texelToRgba(tex0.cpsm, raw, texa);
    }
    const uint64_t content = hashBytes(reinterpret_cast<const uint8_t *>(palette.data()), entries * 4u) ^ (static_cast<uint64_t>(entries) * 0x9E3779B97F4A7C15ull);
    const auto placed = m_cluts.try_emplace(content);
    ClutEntry &entry = placed.first->second;
    entry.lastUsed = m_frameIndex;
    m_clutKeys[key] = &entry;
    if (!placed.second)
        return entry.tex;

    const unsigned tex = takeName(m_texNames, "texture");
    entry.tex = tex;
    pushOp([tex, palette = std::move(palette)]()
           {
               glActiveTexture(GL_TEXTURE1);
               glBindTexture(GL_TEXTURE_2D, tex);
               glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, palette.data());
               glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
               glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
               glActiveTexture(GL_TEXTURE0);
           });
    ++m_stats.clutUploads;
    return tex;
}

void GSGlesBackend::evictCluts()
{
    constexpr size_t kKeys = 8192u, kPalettes = 1024u;
    if (m_cluts.size() > kPalettes)
    {
        for (auto it = m_cluts.begin(); it != m_cluts.end();)
        {
            if (m_frameIndex - it->second.lastUsed > 120u)
            {
                pushOp([tex = it->second.tex]() { glDeleteTextures(1, &tex); });
                it = m_cluts.erase(it);
            }
            else
                ++it;
        }
        m_clutKeys.clear();
    }
    else if (m_clutKeys.size() > kKeys)
        m_clutKeys.clear();
}

bool GSGlesBackend::sameState(const GSDrawState &a, const GSDrawState &b) const
{
    const GSContext &x = a.context, &y = b.context;
    return x.frame.fbp == y.frame.fbp && x.frame.fbw == y.frame.fbw && x.frame.psm == y.frame.psm && x.frame.fbmsk == y.frame.fbmsk &&
           x.scissor.x0 == y.scissor.x0 && x.scissor.x1 == y.scissor.x1 && x.scissor.y0 == y.scissor.y0 && x.scissor.y1 == y.scissor.y1 &&
           x.tex0.tbp0 == y.tex0.tbp0 && x.tex0.tbw == y.tex0.tbw && x.tex0.psm == y.tex0.psm && x.tex0.tw == y.tex0.tw && x.tex0.th == y.tex0.th &&
           x.tex0.tcc == y.tex0.tcc && x.tex0.tfx == y.tex0.tfx && x.tex0.cbp == y.tex0.cbp && x.tex0.cpsm == y.tex0.cpsm && x.tex0.csm == y.tex0.csm && x.tex0.csa == y.tex0.csa &&
           x.xyoffset.ofx == y.xyoffset.ofx && x.xyoffset.ofy == y.xyoffset.ofy &&
           x.zbuf.zbp == y.zbuf.zbp && x.zbuf.psm == y.zbuf.psm && x.zbuf.zmask == y.zbuf.zmask &&
           x.tex1 == y.tex1 && x.clamp == y.clamp && x.alpha == y.alpha && x.test == y.test && x.fba == y.fba &&
           a.prim.tme == b.prim.tme && a.prim.fge == b.prim.fge && a.prim.abe == b.prim.abe && a.prim.fst == b.prim.fst &&
           ((a.prim.type <= GS_PRIM_POINT) == (b.prim.type <= GS_PRIM_POINT)) &&
           ((a.prim.type == GS_PRIM_LINE || a.prim.type == GS_PRIM_LINESTRIP) == (b.prim.type == GS_PRIM_LINE || b.prim.type == GS_PRIM_LINESTRIP)) &&
           a.texa.ta0 == b.texa.ta0 && a.texa.ta1 == b.texa.ta1 && a.texa.aem == b.texa.aem &&
           a.pabe == b.pabe && a.fogR == b.fogR && a.fogG == b.fogG && a.fogB == b.fogB;
}

void GSGlesBackend::beginRun(const GSDrawState &state)
{
    m_lastState = state;
    m_needResolve = false;

    const GSContext &ctx = state.context;
    Command cmd;
    cmd.first = static_cast<uint32_t>(m_vertices.size());

    if (state.prim.type == GS_PRIM_POINT)
        cmd.mode = GL_POINTS;
    else if (state.prim.type == GS_PRIM_LINE || state.prim.type == GS_PRIM_LINESTRIP)
        cmd.mode = GL_LINES;
    else
        cmd.mode = GL_TRIANGLES;

    const int needHeight = static_cast<int>(ctx.scissor.y1) + 1;
    setTarget(cmd, getRenderTarget(ctx.frame.fbp, ctx.frame.fbw, ctx.frame.psm, needHeight));
    cmd.rt->drawn = true;

    // Depth
    const uint32_t test = static_cast<uint32_t>(ctx.test);
    const bool zte = (test >> 16) & 1u;
    const uint32_t ztst = (test >> 17) & 3u;
    const bool usesDepth = zte && (ztst != 1u || !ctx.zbuf.zmask);
    if (usesDepth)
    {
        cmd.depth = getDepthBuffer(ctx.zbuf.zbp, cmd.rt->width, cmd.rt->height);
        cmd.depthTest = true;
        static const unsigned funcs[4] = {GL_NEVER, GL_ALWAYS, GL_GEQUAL, GL_GREATER};
        cmd.depthFunc = funcs[ztst];
        cmd.depthWrite = !ctx.zbuf.zmask;
    }
    switch (ctx.zbuf.psm)
    {
    case GS_PSM_Z32:
        m_runZScale = 1.0f / 4294967296.0f;
        break;
    case GS_PSM_Z16:
    case GS_PSM_Z16S:
        m_runZScale = 1.0f / 65536.0f;
        break;
    default:
        m_runZScale = 1.0f / 16777216.0f;
        break;
    }

    // Color mask: FBMSK has a bit per channel bit. Only whole channels are honored here.
    const uint32_t mask = ctx.frame.fbmsk;
    cmd.colorMask = 0xF;
    for (int c = 0; c < 4; ++c)
    {
        const uint32_t channel = (mask >> (8 * c)) & 0xFFu;
        if (channel == 0xFFu)
            cmd.colorMask &= static_cast<uint8_t>(~(1u << c));
        else if (channel != 0u)
            ++m_stats.partialMask;
    }
    if (ctx.frame.psm == GS_PSM_CT24)
        cmd.colorMask &= 0x7;

    // Scissor
    cmd.scissor[0] = ctx.scissor.x0;
    cmd.scissor[1] = ctx.scissor.y0;
    cmd.scissor[2] = static_cast<int>(ctx.scissor.x1) - ctx.scissor.x0 + 1;
    cmd.scissor[3] = static_cast<int>(ctx.scissor.y1) - ctx.scissor.y0 + 1;

    uint32_t key = 0;

    // Texture
    if (state.prim.tme)
    {
        const GSTex0Reg &t = ctx.tex0;
        key |= kTex | (static_cast<uint32_t>(t.tfx & 3u) << kTfxShift);
        if (t.tcc)
            key |= kTcc;
        RenderTarget *source = nullptr;
        for (auto &rt : m_rts)
            if (rt->drawn && rt->fbp * 32u == t.tbp0 && !isIndexedPsm(t.psm))
                source = rt.get();
        float texW = static_cast<float>(1u << std::min<uint32_t>(t.tw, 10u));
        float texH = static_cast<float>(1u << std::min<uint32_t>(t.th, 10u));
        unsigned wrapS, wrapT;
        const uint32_t clampReg = static_cast<uint32_t>(ctx.clamp);
        const uint32_t wms = clampReg & 3u, wmt = (clampReg >> 2) & 3u;
        wrapS = wms == 1u ? GL_CLAMP_TO_EDGE : GL_REPEAT;
        wrapT = wmt == 1u ? GL_CLAMP_TO_EDGE : GL_REPEAT;
        if (wms >= 2u || wmt >= 2u)
        {
            key |= kRegion;
            const uint64_t cl = ctx.clamp;
            cmd.regionMode[0] = static_cast<int>(wms);
            cmd.regionMode[1] = static_cast<int>(wmt);
            cmd.region[0] = static_cast<float>((cl >> 4) & 0x3FFu);
            cmd.region[1] = static_cast<float>((cl >> 14) & 0x3FFu);
            cmd.region[2] = static_cast<float>((cl >> 24) & 0x3FFu);
            cmd.region[3] = static_cast<float>((cl >> 34) & 0x3FFu);
        }
        const unsigned filter = state.linearFilter ? GL_LINEAR : GL_NEAREST;
        if (source)
        {
            ++m_stats.rtTextures;
            if (source == cmd.rt)
            {
                // The target can't be sampled while drawn to: copy it first.
                ++m_stats.rtFeedback;
                if (!source->copyTex)
                {
                    source->copyTex = takeName(m_texNames, "texture");
                    pushOp([tex = source->copyTex, w = source->width, h = source->height]()
                           {
                               glActiveTexture(GL_TEXTURE0);
                               glBindTexture(GL_TEXTURE_2D, tex);
                               glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                               glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                               glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                               glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                               glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                           });
                }
                Command copy;
                copy.type = Command::Type::RtCopy;
                setTarget(copy, source);
                m_commands.push_back(copy);
                cmd.tex = source->copyTex;
            }
            else
                cmd.tex = source->tex;
            cmd.rtAlpha = 128.0f / 255.0f;
            // Sampled as a format with less alpha, the target's alpha goes through TEXA.
            cmd.ta[0] = state.texa.ta0 / 255.0f;
            cmd.ta[1] = state.texa.ta1 / 255.0f;
            cmd.ta[2] = state.texa.aem ? 1.0f : 0.0f;
            if (t.psm == GS_PSM_CT16 || t.psm == GS_PSM_CT16S)
                key |= kRt16;
            else if (t.psm == GS_PSM_CT24)
                key |= kRt24;
            cmd.texSize[0] = static_cast<float>(source->width);
            cmd.texSize[1] = static_cast<float>(source->height);
            cmd.uvScale[0] = texW / source->width;
            cmd.uvScale[1] = texH / source->height;
            cmd.sampler = getSampler(filter, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE);
        }
        else
        {
            TextureEntry *entry = resolveTexture(state);
            cmd.tex = entry->tex;
            cmd.texSize[0] = texW;
            cmd.texSize[1] = texH;
            if (entry->indexed)
            {
                key |= kPal;
                if (state.linearFilter)
                    key |= kLinear;
                cmd.clut = resolveClut(t, state.texa);
            }
            cmd.sampler = getSampler(entry->indexed ? GL_NEAREST : filter, wrapS, wrapT);
        }
    }

    if (state.prim.fge)
    {
        key |= kFog;
        cmd.fog[0] = state.fogR / 255.0f;
        cmd.fog[1] = state.fogG / 255.0f;
        cmd.fog[2] = state.fogB / 255.0f;
    }

    // Alpha test
    const bool ate = test & 1u;
    const uint32_t atst = ate ? ((test >> 1) & 7u) : 1u;
    const uint32_t afail = ate ? ((test >> 12) & 3u) : 0u;
    cmd.aref = static_cast<float>((test >> 4) & 0xFFu);
    key |= atst << kAtstShift;

    // Destination alpha test: needs the framebuffer's alpha in the shader.
    bool needFetch = false;
    if ((test >> 14) & 1u)
    {
        if (m_fbfetch)
        {
            key |= (((test >> 15) & 1u) ? 2u : 1u) << kDateShift;
            needFetch = true;
        }
        else
            ++m_stats.fbBlendMissing;
    }
    const bool fba = ctx.fba & 1u;

    // Blending
    const uint64_t alpha = ctx.alpha;
    const uint32_t a = alpha & 3u, b = (alpha >> 2) & 3u, c = (alpha >> 4) & 3u, d = (alpha >> 6) & 3u;
    cmd.fix = static_cast<float>((alpha >> 32) & 0xFFu);
    // The GS factor C runs to 2.0 (As or FIX of 0xFF), and this game relies on it: it lights surfaces
    // with Cd * As per channel. Two forms stay exact past 1.0 without reading the framebuffer.
    const bool factorInShader = c != 1u && !state.pabe && !fba;
    if (state.prim.abe && factorInShader && a == 0u && b == 2u && d != 0u)
    {
        // Cs * C + (Cd or 0): premultiply in the shader
        key |= kPremul | (c << (kBlendShift + 4));
        cmd.blend = d == 1u;
        cmd.srcRgb = GL_ONE;
        cmd.dstRgb = GL_ONE;
        cmd.equation = GL_FUNC_ADD;
    }
    else if (state.prim.abe && factorInShader && a == 1u && b == 2u && d == 2u)
    {
        // Cd * C = Cd * max(C - 1, 0) + Cd * min(C, 1)
        key |= kModDst | (c << (kBlendShift + 4));
        cmd.blend = true;
        cmd.srcRgb = GL_DST_COLOR;
        cmd.dstRgb = GL_SRC_ALPHA;
        cmd.equation = GL_FUNC_ADD;
    }
    else if (state.prim.abe)
    {
        unsigned src = GL_ONE, dst = GL_ZERO, equation = GL_FUNC_ADD;
        const bool fixedFunction = mapBlend(a, b, c, d, src, dst, equation) && !state.pabe && !fba;
        if (fixedFunction)
        {
            cmd.blend = !(src == GL_ONE && dst == GL_ZERO && equation == GL_FUNC_ADD);
            cmd.srcRgb = src;
            cmd.dstRgb = dst;
            cmd.equation = equation;
            cmd.blendConstant = std::min(cmd.fix / 128.0f, 1.0f);
        }
        else if (m_fbfetch)
        {
            key |= kFbBlend | (a << kBlendShift) | (b << (kBlendShift + 2)) | (c << (kBlendShift + 4)) | (d << (kBlendShift + 6));
            if (state.pabe)
                key |= kPabe;
            needFetch = true;
            ++m_stats.fbBlendRuns;
        }
        else
        {
            ++m_stats.fbBlendMissing;
            cmd.blend = true;
            cmd.srcRgb = GL_SRC_ALPHA;
            cmd.dstRgb = GL_ONE_MINUS_SRC_ALPHA;
            cmd.equation = GL_FUNC_ADD;
        }
    }
    if (fba)
        key |= kFba;

    cmd.program = getProgram(key);
    cmd.afail = static_cast<uint8_t>(afail);
    cmd.firstPassEnabled = atst != 0u;
    if (afail != 0u && atst != 1u)
    {
        uint32_t inverseKey = key | kAinv;
        cmd.inverseProgram = getProgram(inverseKey);
    }

    if (std::getenv("GSPLAY_STATES"))
    {
        char line[200];
        const GSTex0Reg &t0 = ctx.tex0;
        std::snprintf(line, sizeof(line),
                      "fpsm%02x zpsm%02x tme%d psm%02x tfx%d tcc%d fst%d fge%d abe%d abcd%u%u%u%u fix%02x ate%d atst%u aref%02x afail%u date%d zte%d ztst%u zm%d fbmsk%08x wm%u%u lin%d fba%d",
                      ctx.frame.psm, ctx.zbuf.psm, state.prim.tme, state.prim.tme ? t0.psm : 0, state.prim.tme ? t0.tfx : 0, state.prim.tme ? t0.tcc : 0, state.prim.fst, state.prim.fge, state.prim.abe,
                      a, b, c, d, (unsigned)((alpha >> 32) & 0xFFu), ate, atst, (test >> 4) & 0xFFu, afail, (test >> 14) & 1u, zte, ztst, ctx.zbuf.zmask,
                      ctx.frame.fbmsk, (unsigned)(ctx.clamp & 3u), (unsigned)((ctx.clamp >> 2) & 3u), state.linearFilter, fba);
        ++m_stateLog[line];
    }

    // executeCommands above may have cleared the list, so first is taken again
    cmd.first = static_cast<uint32_t>(m_vertices.size());
    m_commands.push_back(cmd);
    ++m_stats.runs;
    m_needResolve = false;
}

void GSGlesBackend::appendPrimitive(const GSVertex *verts, int count)
{
    const GSDrawState &state = m_lastState;
    const GSContext &ctx = state.context;
    const float ofx = ctx.xyoffset.ofx / 16.0f;
    const float ofy = ctx.xyoffset.ofy / 16.0f;
    const bool fst = state.prim.fst;
    const float texW = state.textureWidth ? state.textureWidth : 1.0f;
    const float texH = state.textureHeight ? state.textureHeight : 1.0f;
    const float zScale = m_runZScale;
    const GSVertex &last = verts[count ? count - 1 : 0];
    const bool iip = state.prim.iip;

    auto convert = [&](const GSVertex &v) -> HostVertex
    {
        HostVertex h;
        // GS pixels sample at integer positions, GL's at half: this makes coverage the same.
        h.x = v.x - ofx + 0.5f;
        h.y = v.y - ofy + 0.5f;
        // The GS doesn't clip on Z, and GL would: keep every vertex inside the depth range.
        h.z = std::min(static_cast<float>(v.z) * zScale, 1.0f);
        if (fst)
        {
            h.s = v.u / 16.0f / texW;
            h.t = v.v / 16.0f / texH;
            h.q = 1.0f;
        }
        else
        {
            h.s = v.s;
            h.t = v.t;
            h.q = v.q;
        }
        const GSVertex &colorSource = iip ? v : last;
        h.r = colorSource.r;
        h.g = colorSource.g;
        h.b = colorSource.b;
        h.a = colorSource.a;
        h.fog = colorSource.fog;
        h.pad[0] = h.pad[1] = h.pad[2] = 0;
        return h;
    };

    switch (state.prim.type)
    {
    case GS_PRIM_POINT:
        m_vertices.push_back(convert(verts[0]));
        break;
    case GS_PRIM_LINE:
    case GS_PRIM_LINESTRIP:
        m_vertices.push_back(convert(verts[0]));
        m_vertices.push_back(convert(verts[1]));
        break;
    case GS_PRIM_SPRITE:
    {
        HostVertex a = convert(verts[0]);
        HostVertex d = convert(verts[1]);
        // A sprite's color and z come from its second vertex, and STQ needs dividing per corner
        // because the corners mix the two vertices' coordinates.
        if (!fst)
        {
            const float qa = a.q != 0.0f ? a.q : 1.0f;
            const float qd = d.q != 0.0f ? d.q : 1.0f;
            a.s /= qa;
            a.t /= qa;
            a.q = 1.0f;
            d.s /= qd;
            d.t /= qd;
            d.q = 1.0f;
        }
        a.r = d.r;
        a.g = d.g;
        a.b = d.b;
        a.a = d.a;
        a.fog = d.fog;
        a.z = d.z;
        HostVertex b = a, c = a;
        b.x = d.x;
        b.s = d.s;
        c.y = d.y;
        c.t = d.t;
        m_vertices.push_back(a);
        m_vertices.push_back(b);
        m_vertices.push_back(c);
        m_vertices.push_back(b);
        m_vertices.push_back(d);
        m_vertices.push_back(c);
        break;
    }
    default:
        m_vertices.push_back(convert(verts[0]));
        m_vertices.push_back(convert(verts[1]));
        m_vertices.push_back(convert(verts[2]));
        break;
    }
}

void GSGlesBackend::Submit(const GSPrimitiveBatch &batch)
{
    if (m_needResolve || m_commands.empty() || m_commands.back().type != Command::Type::Draw || !sameState(batch.state, m_lastState))
        beginRun(batch.state);

    // GSPLAY_PROBE=x,y lists the primitives whose bounding box covers that render target pixel.
    static int probeX = -1, probeY = -1;
    static const bool probeParsed = []() {
        if (const char *env = std::getenv("GSPLAY_PROBE"))
            std::sscanf(env, "%d,%d", &probeX, &probeY);
        return true;
    }();
    (void)probeParsed;
    if (probeX >= 0)
    {
        const GSContext &ctx = batch.state.context;
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
        const int count = batch.state.prim.type == GS_PRIM_SPRITE ? 2 : batch.vertexCount;
        for (int i = 0; i < count; ++i)
        {
            const float x = batch.vertices[i].x - ctx.xyoffset.ofx / 16.0f;
            const float y = batch.vertices[i].y - ctx.xyoffset.ofy / 16.0f;
            x0 = std::min(x0, x);
            x1 = std::max(x1, x);
            y0 = std::min(y0, y);
            y1 = std::max(y1, y);
        }
        if (probeX >= x0 && probeX <= x1 && probeY >= y0 && probeY <= y1)
        {
            const GSVertex &v = batch.vertices[count - 1];
            const GSTex0Reg &t = ctx.tex0;
            std::printf("probe fbp%x fbm%08x type%d box %.0f,%.0f-%.0f,%.0f z%.0f rgba %02x%02x%02x%02x tme%d tbp%x psm%02x tw%u th%u tfx%u tcc%u cbp%x csa%u abe%d alpha%02x fix%02x test%05x iip%d\n",
                        ctx.frame.fbp, ctx.frame.fbmsk, batch.state.prim.type, x0, y0, x1, y1, v.z, v.r, v.g, v.b, v.a, batch.state.prim.tme,
                        t.tbp0, t.psm, t.tw, t.th, t.tfx, t.tcc, t.cbp, t.csa, batch.state.prim.abe,
                        (unsigned)(ctx.alpha & 0xFFu), (unsigned)((ctx.alpha >> 32) & 0xFFu), (unsigned)ctx.test, batch.state.prim.iip);
        }
    }
    const size_t before = m_vertices.size();
    appendPrimitive(batch.vertices.data(), batch.vertexCount);
    m_commands.back().count += static_cast<uint32_t>(m_vertices.size() - before);
}

bool GSGlesBackend::SubmitPackedStrip(const uint8_t *qwords, uint32_t count, const GSVertex *)
{
    const GSDrawState &state = m_lastState;
    // Flat strips recolor each triangle: the ordinary path handles those.
    if (m_needResolve || m_commands.empty() || m_commands.back().type != Command::Type::Draw || !state.prim.iip || state.prim.fst)
        return false;
    const GSContext &ctx = state.context;
    const float ox = 0.5f - ctx.xyoffset.ofx / 16.0f;
    const float oy = 0.5f - ctx.xyoffset.ofy / 16.0f;
    const float zScale = m_runZScale;

    const size_t before = m_vertices.size();
    m_vertices.resize(before + static_cast<size_t>(count) * 3u);
    HostVertex *out = m_vertices.data() + before;
    HostVertex a, b;
    for (uint32_t i = 0; i < count; ++i, qwords += 48)
    {
        uint32_t st[4], color[4];
        uint64_t xyz[2];
        std::memcpy(st, qwords, 16);
        std::memcpy(color, qwords + 16, 16);
        std::memcpy(xyz, qwords + 32, 16);
        HostVertex v;
        std::memcpy(&v.s, &st[0], 4);
        std::memcpy(&v.t, &st[1], 4);
        std::memcpy(&v.q, &st[2], 4);
        if (v.q == 0.0f)
            v.q = 1.0f;
        v.r = static_cast<uint8_t>(color[0]);
        v.g = static_cast<uint8_t>(color[1]);
        v.b = static_cast<uint8_t>(color[2]);
        v.a = static_cast<uint8_t>(color[3]);
        v.x = static_cast<float>(xyz[0] & 0xFFFFu) * 0.0625f + ox;
        v.y = static_cast<float>((xyz[0] >> 32) & 0xFFFFu) * 0.0625f + oy;
        v.z = std::min(static_cast<float>((xyz[1] >> 4) & 0xFFFFFFu) * zScale, 1.0f);
        v.fog = static_cast<uint8_t>(xyz[1] >> 36);
        v.pad[0] = v.pad[1] = v.pad[2] = 0;
        if (i >= 2u && !((xyz[1] >> 47) & 1u))
        {
            out[0] = a;
            out[1] = b;
            out[2] = v;
            out += 3;
        }
        a = b;
        b = v;
    }
    const size_t added = static_cast<size_t>(out - (m_vertices.data() + before));
    m_vertices.resize(before + added);
    m_commands.back().count += static_cast<uint32_t>(added);
    return true;
}

bool GSGlesBackend::SubmitSameState(const GSVertex *vertices, int count)
{
    if (m_needResolve || m_commands.empty() || m_commands.back().type != Command::Type::Draw)
        return false;
    const size_t before = m_vertices.size();
    appendPrimitive(vertices, count);
    m_commands.back().count += static_cast<uint32_t>(m_vertices.size() - before);
    return true;
}

// What the GS does on a TEX0 write with CLD set: same decisions as the software backend. A game loads
// palettes before nearly every draw, so a load seen before costs a copy, not 256 swizzled reads.
void GSGlesBackend::LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    if (!isIndexedPsm(tex0.psm))
        return;
    switch (tex0.cld)
    {
    case 1u:
        break;
    case 2u:
        m_clutCbp[0] = tex0.cbp;
        break;
    case 3u:
        m_clutCbp[1] = tex0.cbp;
        break;
    case 4u:
        if (m_clutCbp[0] == tex0.cbp)
            return;
        m_clutCbp[0] = tex0.cbp;
        break;
    case 5u:
        if (m_clutCbp[1] == tex0.cbp)
            return;
        m_clutCbp[1] = tex0.cbp;
        break;
    default:
        return;
    }

    const bool fourBit = isFourBitPsm(tex0.psm);
    const bool sixteenBit = tex0.cpsm == GS_PSM_CT16 || tex0.cpsm == GS_PSM_CT16S;
    const bool thirtyTwoBit = tex0.cpsm == GS_PSM_CT32 || tex0.cpsm == GS_PSM_CT24;
    if (!sixteenBit && !thirtyTwoBit)
        return;

    const uint64_t key = static_cast<uint64_t>(tex0.cbp) | (static_cast<uint64_t>(tex0.cpsm) << 14) | (static_cast<uint64_t>(tex0.csm) << 18) |
                         (static_cast<uint64_t>(tex0.csa) << 19) | (static_cast<uint64_t>(fourBit) << 24) |
                         (tex0.csm ? ((static_cast<uint64_t>(texclut.cbw) << 25) | (static_cast<uint64_t>(texclut.cou) << 33) | (static_cast<uint64_t>(texclut.cov) << 39)) : 0u);
    const uint32_t page = tex0.cbp / 32u;
    const uint64_t sig = (static_cast<uint64_t>(m_pageGen[page % kPageCount]) << 32) | m_pageGen[(page + 1u) % kPageCount];
    if (key == m_lastClutKey && sig == m_lastClutSig)
        return;
    m_lastClutKey = key;
    m_lastClutSig = sig;
    ClutLoad &load = m_clutLoads[key];
    if (load.first == 0xFFFFFFFFu || load.sig != sig)
    {
        const uint32_t entryCount = fourBit ? 16u : 256u;
        const uint32_t csaMask = sixteenBit ? 0x1Fu : 0x0Fu;
        const uint32_t destinationBase = (static_cast<uint32_t>(tex0.csa) & csaMask) << 4u;
        const bool suffix = tex0.csm == 0u && thirtyTwoBit && !fourBit;
        load.first = suffix ? destinationBase : 0u;
        load.count = entryCount;
        load.destination = suffix ? 0u : destinationBase;
        load.mask = sixteenBit ? 0x1FFu : 0xFFu;
        load.wide = thirtyTwoBit;
        load.sig = sig;
        load.values.resize(entryCount);
        for (uint32_t entry = load.first; entry < entryCount; ++entry)
        {
            uint32_t sourceX, sourceY, sourceWidth = 1u;
            if (tex0.csm == 0u)
            {
                // CSM1 swaps index bits 3 and 4
                const uint32_t index = (entry & 0xE7u) | ((entry & 0x08u) << 1u) | ((entry & 0x10u) >> 1u);
                sourceX = index & 0x0Fu;
                sourceY = index >> 4u;
            }
            else
            {
                sourceWidth = texclut.cbw ? texclut.cbw : 1u;
                sourceX = (static_cast<uint32_t>(texclut.cou) << 4u) + entry;
                sourceY = texclut.cov;
            }
            load.values[entry] = m_mem.ReadVram(tex0.cpsm, tex0.cbp, sourceWidth, sourceX, sourceY);
        }
        load.full = load.first == 0u && load.count == 256u && load.wide;
        if (load.full)
        {
            load.ram.resize(512u);
            for (uint32_t entry = 0; entry < 256u; ++entry)
            {
                load.ram[entry] = static_cast<uint16_t>(load.values[entry]);
                load.ram[entry + 256u] = static_cast<uint16_t>(load.values[entry] >> 16u);
            }
        }
    }
    if (load.full)
    {
        // The whole CLUT is replaced, so its identity is this load's, with no need to hash it.
        std::memcpy(m_clutRam, load.ram.data(), sizeof(m_clutRam));
        const uint64_t id = (key * 0x9E3779B97F4A7C15ull) ^ sig;
        if (id != m_clutRamHash)
        {
            m_clutRamHash = id;
            m_needResolve = true;
        }
        return;
    }
    for (uint32_t entry = load.first; entry < load.count; ++entry)
    {
        const uint32_t destination = (load.destination + entry) & load.mask;
        const uint32_t raw = load.values[entry];
        m_clutRam[destination] = static_cast<uint16_t>(raw);
        if (load.wide)
            m_clutRam[destination + 256u] = static_cast<uint16_t>(raw >> 16u);
    }
    const uint64_t hash = hashBytes(reinterpret_cast<const uint8_t *>(m_clutRam), sizeof(m_clutRam));
    if (hash != m_clutRamHash)
    {
        m_clutRamHash = hash;
        m_needResolve = true;
    }
}

void GSGlesBackend::BeginTransfer(const GSTransferCommand &command)
{
    m_mem.BeginTransfer(command);
    m_transfer = command;
    m_transferPending = true;
    m_needResolve = true;
    const uint8_t dpsm = command.bitbltbuf.dpsm;
    m_fastUpload = command.direction == 0u && (dpsm == GS_PSM_CT32 || dpsm == GS_PSM_CT16 || dpsm == GS_PSM_CT16S || dpsm == GS_PSM_T8 || dpsm == GS_PSM_T4);
    m_uploadCopied = 0u;
    m_uploadTotal = static_cast<uint32_t>(command.trxreg.rrw) * command.trxreg.rrh;
    if (command.direction == 2u)
    {
        bumpPages(command.bitbltbuf.dbp, command.bitbltbuf.dbw, command.bitbltbuf.dpsm, command.trxpos.dsay, command.trxreg.rrh);
        finishTransferIfDone();
    }
}

void GSGlesBackend::UploadImage(const uint8_t *data, uint32_t sizeBytes)
{
    const double uploadStart = nowMs();
    if (m_fastUpload)
        fastUpload(data, sizeBytes);
    else
        m_mem.UploadImage(data, sizeBytes);
    m_stats.vramMs += nowMs() - uploadStart;
    bumpPages(m_transfer.bitbltbuf.dbp, m_transfer.bitbltbuf.dbw, m_transfer.bitbltbuf.dpsm, m_transfer.trxpos.dsay, m_transfer.trxreg.rrh);
    m_needResolve = true;
    finishTransferIfDone();
}

// Host to local for the formats a game streams: the same swizzle as the software backend, without its
// lock and per-pixel indirect call.
void GSGlesBackend::fastUpload(const uint8_t *data, uint32_t sizeBytes)
{
    uint8_t *vram = m_vram;
    constexpr uint32_t kMask = 4u * 1024u * 1024u - 1u;
    const uint32_t dbp = m_transfer.bitbltbuf.dbp;
    const uint32_t dbw = std::max<uint32_t>(m_transfer.bitbltbuf.dbw, 1u);
    const uint32_t rrw = m_transfer.trxreg.rrw, dsax = m_transfer.trxpos.dsax, dsay = m_transfer.trxpos.dsay;
    if (rrw == 0u)
        return;
    uint32_t copied = m_uploadCopied;
    const uint32_t total = m_uploadTotal;
    uint32_t column = copied % rrw;
    uint32_t y = dsay + copied / rrw;
    uint32_t offset = 0u;
    auto advance = [&]()
    {
        ++copied;
        if (++column == rrw)
        {
            column = 0u;
            ++y;
        }
    };
    switch (m_transfer.bitbltbuf.dpsm)
    {
    case GS_PSM_CT32:
        while (offset + 4u <= sizeBytes && copied < total)
        {
            std::memcpy(vram + (GSPSMCT32::addrPSMCT32(dbp, dbw, dsax + column, y) & (kMask & ~3u)), data + offset, 4);
            offset += 4u;
            advance();
        }
        break;
    case GS_PSM_CT16:
    case GS_PSM_CT16S:
    {
        const bool signedLayout = m_transfer.bitbltbuf.dpsm == GS_PSM_CT16S;
        while (offset + 2u <= sizeBytes && copied < total)
        {
            const uint32_t x = dsax + column;
            const uint32_t a = signedLayout ? GSPSMCT16::addrPSMCT16S(dbp, dbw, x, y) : GSPSMCT16::addrPSMCT16(dbp, dbw, x, y);
            std::memcpy(vram + (a & (kMask & ~1u)), data + offset, 2);
            offset += 2u;
            advance();
        }
        break;
    }
    case GS_PSM_T8:
        while (offset < sizeBytes && copied < total)
        {
            vram[GSPSMT8::addrPSMT8(dbp, dbw, dsax + column, y) & kMask] = data[offset++];
            advance();
        }
        break;
    case GS_PSM_T4:
        while (offset < sizeBytes && copied < total)
        {
            const uint8_t packed = data[offset++];
            for (uint32_t n = 0; n < 2u && copied < total; ++n)
            {
                const uint32_t a = GSPSMT4::addrPSMT4(dbp, dbw, dsax + column, y);
                uint8_t &byte = vram[(a >> 1) & kMask];
                const uint8_t nibble = n ? (packed >> 4) : (packed & 0x0Fu);
                byte = (a & 1u) ? static_cast<uint8_t>((byte & 0x0Fu) | (nibble << 4)) : static_cast<uint8_t>((byte & 0xF0u) | nibble);
                advance();
            }
        }
        break;
    default:
        break;
    }
    m_uploadCopied = copied;
}

void GSGlesBackend::finishTransferIfDone()
{
    if (!m_transferPending)
        return;
    if (m_fastUpload)
    {
        if (m_uploadCopied < m_uploadTotal)
            return;
    }
    else
    {
        const GSTransferSnapshot snap = m_mem.GetTransferSnapshot();
        if (m_transfer.direction == 0u && snap.copiedPixels < snap.totalPixels)
            return;
    }
    m_transferPending = false;
    // Pixels written into a live render target have to reach its GL texture, in order.
    const uint32_t dbp = m_transfer.bitbltbuf.dbp;
    if (dbp & 31u)
        return;
    RenderTarget *rt = findRenderTarget(dbp / 32u, m_transfer.bitbltbuf.dbw);
    if (!rt)
        return;
    const int x = m_transfer.trxpos.dsax, y = m_transfer.trxpos.dsay;
    const int w = std::min<int>(m_transfer.trxreg.rrw, rt->width - x), h = std::min<int>(m_transfer.trxreg.rrh, rt->height - y);
    if (w <= 0 || h <= 0)
        return;
    Command cmd;
    cmd.type = Command::Type::RtUpload;
    setTarget(cmd, rt);
    cmd.rectX = x;
    cmd.rectY = y;
    cmd.rectW = w;
    cmd.rectH = h;
    cmd.uploadIndex = static_cast<uint32_t>(m_uploadPixels.size());
    m_uploadPixels.emplace_back();
    loadRtFromVram(*rt, x, y, w, h, m_uploadPixels.back());
    m_commands.push_back(cmd);
    ++m_stats.rtUploads;
}

void GSGlesBackend::Flush() {}

void GSGlesBackend::TextureFlush()
{
    m_mem.TextureFlush();
    m_needResolve = true;
}

void GSGlesBackend::Sync(GSSyncReason reason)
{
    m_mem.Sync(reason);
}

void GSGlesBackend::executeCommands()
{
    if (m_execCommands.empty())
    {
        m_execVertices.clear();
        return;
    }
    const double t0 = nowMs();
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<long>(m_execVertices.size() * sizeof(HostVertex)), m_execVertices.data(), GL_STREAM_DRAW);
    m_stats.vertices += m_execVertices.size();

    unsigned currentFbo = 0xFFFFFFFFu;
    Program *currentProgram = nullptr;
    unsigned currentTex = 0xFFFFFFFFu, currentClut = 0xFFFFFFFFu;
    unsigned currentSampler = 0xFFFFFFFFu;
    int blendState = -1;
    unsigned curSrc = 0, curDst = 0, curEq = 0;
    float curConstant = -1.0f;
    int depthState = -1, depthMaskState = -1;
    unsigned curDepthFunc = 0;
    int curMask = -1;
    int curScissor[4] = {-1, -1, -1, -1};

    glEnable(GL_SCISSOR_TEST);

    auto usePass = [&](const Command &cmd, Program *program, int colorMask, bool depthWrite)
    {
        if (program != currentProgram)
        {
            glUseProgram(program->id);
            currentProgram = program;
            ++m_stats.programSwitches;
        }
        if (program->scaleW != cmd.rtW || program->scaleH != cmd.rtH)
        {
            glUniform4f(program->uScale, 2.0f / cmd.rtW, 2.0f / cmd.rtH, -1.0f, -1.0f);
            program->scaleW = cmd.rtW;
            program->scaleH = cmd.rtH;
        }
        if (program->uTexSize >= 0 && (program->texSize[0] != cmd.texSize[0] || program->texSize[1] != cmd.texSize[1]))
        {
            glUniform2f(program->uTexSize, cmd.texSize[0], cmd.texSize[1]);
            program->texSize[0] = cmd.texSize[0];
            program->texSize[1] = cmd.texSize[1];
        }
        if (program->uUvScale >= 0 && (program->uvScale[0] != cmd.uvScale[0] || program->uvScale[1] != cmd.uvScale[1]))
        {
            glUniform2f(program->uUvScale, cmd.uvScale[0], cmd.uvScale[1]);
            program->uvScale[0] = cmd.uvScale[0];
            program->uvScale[1] = cmd.uvScale[1];
        }
        if (program->uFog >= 0 && std::memcmp(program->fog, cmd.fog, sizeof(cmd.fog)) != 0)
        {
            glUniform3f(program->uFog, cmd.fog[0], cmd.fog[1], cmd.fog[2]);
            std::memcpy(program->fog, cmd.fog, sizeof(cmd.fog));
        }
        if (program->uAref >= 0 && program->aref != cmd.aref)
        {
            glUniform1f(program->uAref, cmd.aref);
            program->aref = cmd.aref;
        }
        if (program->uFix >= 0 && program->fix != cmd.fix)
        {
            glUniform1f(program->uFix, cmd.fix);
            program->fix = cmd.fix;
        }
        if (program->uRtAlpha >= 0 && program->rtAlpha != cmd.rtAlpha)
        {
            glUniform1f(program->uRtAlpha, cmd.rtAlpha);
            program->rtAlpha = cmd.rtAlpha;
        }
        if (program->uTa >= 0 && std::memcmp(program->ta, cmd.ta, sizeof(cmd.ta)) != 0)
        {
            glUniform3f(program->uTa, cmd.ta[0], cmd.ta[1], cmd.ta[2]);
            std::memcpy(program->ta, cmd.ta, sizeof(cmd.ta));
        }
        if (program->uRegion >= 0)
        {
            glUniform4f(program->uRegion, cmd.region[0], cmd.region[1], cmd.region[2], cmd.region[3]);
            glUniform2i(program->uRegionMode, cmd.regionMode[0], cmd.regionMode[1]);
        }
        if (colorMask != curMask)
        {
            glColorMask(colorMask & 1, (colorMask >> 1) & 1, (colorMask >> 2) & 1, (colorMask >> 3) & 1);
            curMask = colorMask;
        }
        if (static_cast<int>(depthWrite) != depthMaskState)
        {
            glDepthMask(depthWrite);
            depthMaskState = depthWrite;
        }
    };

    for (const Command &cmd : m_execCommands)
    {
        if (cmd.type == Command::Type::GlOp)
        {
            if (curMask != 0xF)
            {
                glColorMask(1, 1, 1, 1);
                curMask = 0xF;
            }
            m_execOps[cmd.opIndex]();
            // Whatever it bound, nothing tracked holds any more.
            glActiveTexture(GL_TEXTURE0);
            glEnable(GL_SCISSOR_TEST);
            currentFbo = 0xFFFFFFFFu;
            currentProgram = nullptr;
            currentTex = currentClut = 0xFFFFFFFFu;
            curScissor[0] = curScissor[1] = curScissor[2] = curScissor[3] = -1;
            continue;
        }
        if (cmd.type == Command::Type::RtUpload)
        {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, cmd.rtTex);
            currentTex = cmd.rtTex;
            glTexSubImage2D(GL_TEXTURE_2D, 0, cmd.rectX, cmd.rectY, cmd.rectW, cmd.rectH, GL_RGBA, GL_UNSIGNED_BYTE,
                            m_execUploads[cmd.uploadIndex].data());
            continue;
        }
        if (cmd.type == Command::Type::RtCopy)
        {
            glDisable(GL_SCISSOR_TEST);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, m_copyFbo[0]);
            glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, cmd.rtTex, 0);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_copyFbo[1]);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, cmd.copyTex, 0);
            if (curMask != 0xF)
            {
                glColorMask(1, 1, 1, 1);
                curMask = 0xF;
            }
            glBlitFramebuffer(0, 0, cmd.rtW, cmd.rtH, 0, 0, cmd.rtW, cmd.rtH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            glEnable(GL_SCISSOR_TEST);
            currentFbo = 0xFFFFFFFFu;
            continue;
        }
        if (cmd.count == 0u)
            continue;

        const unsigned fbo = getFramebuffer(cmd.rtTex, cmd.depth, cmd.rtW, cmd.rtH);
        if (fbo != currentFbo)
        {
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glViewport(0, 0, cmd.rtW, cmd.rtH);
            currentFbo = fbo;
            ++m_stats.rtSwitches;
        }
        if (std::memcmp(curScissor, cmd.scissor, sizeof(curScissor)) != 0)
        {
            glScissor(cmd.scissor[0], cmd.scissor[1], cmd.scissor[2], cmd.scissor[3]);
            std::memcpy(curScissor, cmd.scissor, sizeof(curScissor));
        }
        if (cmd.tex != currentTex && cmd.tex != 0u)
        {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, cmd.tex);
            currentTex = cmd.tex;
        }
        if (cmd.sampler != currentSampler && cmd.tex != 0u)
        {
            glBindSampler(0, cmd.sampler);
            currentSampler = cmd.sampler;
        }
        if (cmd.clut != currentClut && cmd.clut != 0u)
        {
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, cmd.clut);
            glActiveTexture(GL_TEXTURE0);
            currentClut = cmd.clut;
        }
        if (static_cast<int>(cmd.blend) != blendState)
        {
            if (cmd.blend)
                glEnable(GL_BLEND);
            else
                glDisable(GL_BLEND);
            blendState = cmd.blend;
        }
        if (cmd.blend && (cmd.srcRgb != curSrc || cmd.dstRgb != curDst || cmd.equation != curEq))
        {
            // Alpha is never blended: the target keeps the source's alpha.
            glBlendFuncSeparate(cmd.srcRgb, cmd.dstRgb, GL_ONE, GL_ZERO);
            glBlendEquationSeparate(cmd.equation, GL_FUNC_ADD);
            curSrc = cmd.srcRgb;
            curDst = cmd.dstRgb;
            curEq = cmd.equation;
        }
        if (cmd.blend && cmd.blendConstant != curConstant)
        {
            glBlendColor(0.0f, 0.0f, 0.0f, cmd.blendConstant);
            curConstant = cmd.blendConstant;
        }
        if (static_cast<int>(cmd.depthTest) != depthState)
        {
            if (cmd.depthTest)
                glEnable(GL_DEPTH_TEST);
            else
                glDisable(GL_DEPTH_TEST);
            depthState = cmd.depthTest;
        }
        if (cmd.depthTest && cmd.depthFunc != curDepthFunc)
        {
            glDepthFunc(cmd.depthFunc);
            curDepthFunc = cmd.depthFunc;
        }

        if (cmd.firstPassEnabled)
        {
            usePass(cmd, cmd.program, cmd.colorMask, cmd.depthWrite);
            glDrawArrays(cmd.mode, static_cast<int>(cmd.first), static_cast<int>(cmd.count));
            ++m_stats.passes;
        }
        if (cmd.inverseProgram)
        {
            // AFAIL: what the alpha test rejected still writes some of the buffers.
            int mask = cmd.colorMask;
            bool depthWrite = false;
            switch (cmd.afail)
            {
            case 1: // FB_ONLY
                break;
            case 2: // ZB_ONLY
                mask = 0;
                depthWrite = cmd.depthWrite;
                break;
            case 3: // RGB_ONLY
                mask &= 0x7;
                break;
            default:
                break;
            }
            usePass(cmd, cmd.inverseProgram, mask, depthWrite);
            glDrawArrays(cmd.mode, static_cast<int>(cmd.first), static_cast<int>(cmd.count));
            ++m_stats.passes;
        }
    }

    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glColorMask(1, 1, 1, 1);
    glDepthMask(GL_TRUE);
    glBindSampler(0, 0);

    m_execCommands.clear();
    m_execVertices.clear();
    m_execUploads.clear();
    m_execOps.clear();
    m_stats.executeMs += nowMs() - t0;
}

void GSGlesBackend::drawPresentQuad(unsigned tex, int texW, int texH, uint32_t dbx, uint32_t dby, uint32_t w, uint32_t h, bool flip)
{
    const float u0 = static_cast<float>(dbx) / texW, u1 = static_cast<float>(dbx + w) / texW;
    const float v0 = static_cast<float>(dby) / texH, v1 = static_cast<float>(dby + h) / texH;
    const float top = flip ? 1.0f : -1.0f, bottom = flip ? -1.0f : 1.0f;
    HostVertex q[6];
    std::memset(q, 0, sizeof(q));
    auto set = [&](int i, float x, float y, float s, float t)
    {
        q[i].x = x;
        q[i].y = y;
        q[i].s = s;
        q[i].t = t;
        q[i].q = 1.0f;
    };
    set(0, -1, top, u0, v0);
    set(1, 1, top, u1, v0);
    set(2, -1, bottom, u0, v1);
    set(3, 1, top, u1, v0);
    set(4, 1, bottom, u1, v1);
    set(5, -1, bottom, u0, v1);
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(q), q, GL_STREAM_DRAW);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBindSampler(0, m_presentSampler);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindSampler(0, 0);
}

// With the frontend's state lock held: what the frame shows, and the frame itself, out of the game thread's hands.
void GSGlesBackend::LatchFrame(const GSPresentationRequest &request)
{
    refillNames();
    planPresent(request);
    evictTextures();
    evictCluts();
    {
        double targetBytes = 0.0, depthBytes = 0.0;
        for (const auto &rt : m_rts)
            targetBytes += static_cast<double>(rt->width) * rt->height * 4.0;
        for (const auto &d : m_depths)
            depthBytes += static_cast<double>(d.second->width) * d.second->height * 4.0;
        m_held[0] = static_cast<double>(m_cluts.size());
        m_held[1] = targetBytes;
        m_held[2] = static_cast<double>(m_depths.size());
        m_held[3] = depthBytes;
        m_held[4] = static_cast<double>(m_contentTextures.size());
        m_held[5] = static_cast<double>(m_textureBytes);
    }
    m_execVertices.swap(m_vertices);
    m_execCommands.swap(m_commands);
    m_execUploads.swap(m_uploadPixels);
    m_execOps.swap(m_glOps);
    m_vertices.clear();
    m_commands.clear();
    m_uploadPixels.clear();
    m_glOps.clear();
    m_needResolve = true;
    ++m_frameIndex;
}

void GSGlesBackend::planPresent(const GSPresentationRequest &request)
{
    m_plan = PresentPlan{};
    const uint64_t pmode = request.pmode;
    const bool en1 = pmode & 1u, en2 = pmode & 2u;
    const bool mmod = (pmode >> 5) & 1u, slbg = (pmode >> 7) & 1u;
    const uint32_t alp = static_cast<uint32_t>((pmode >> 8) & 0xFFu);

    struct Circuit
    {
        bool on = false;
        uint32_t fbp = 0, fbw = 0, dbx = 0, dby = 0, w = 0, h = 0;
        uint8_t psm = 0;
    };
    auto decode = [](uint64_t dispfb, uint64_t display, bool enabled)
    {
        Circuit c;
        c.fbp = static_cast<uint32_t>(dispfb & 0x1FFu);
        c.fbw = static_cast<uint32_t>((dispfb >> 9) & 0x3Fu);
        c.psm = static_cast<uint8_t>((dispfb >> 15) & 0x1Fu);
        c.dbx = static_cast<uint32_t>((dispfb >> 32) & 0x7FFu);
        c.dby = static_cast<uint32_t>((dispfb >> 43) & 0x7FFu);
        const uint32_t dw = static_cast<uint32_t>((display >> 32) & 0xFFFu);
        const uint32_t dh = static_cast<uint32_t>((display >> 44) & 0x7FFu);
        const uint32_t magh = static_cast<uint32_t>((display >> 23) & 0xFu);
        c.w = (dw + 1u) / (magh + 1u);
        c.h = dh + 1u;
        c.on = enabled && c.fbw != 0u && c.w >= 64u && c.h >= 64u;
        return c;
    };
    const Circuit c1 = decode(request.dispfb1, request.display1, en1);
    const Circuit c2 = decode(request.dispfb2, request.display2, en2);
    if (!c1.on && !c2.on)
        return;
    PresentPlan &p = m_plan;
    p.any = true;
    p.outW = std::max(c1.on ? c1.w : 0u, c2.on ? c2.w : 0u);
    p.outH = std::max(c1.on ? c1.h : 0u, c2.on ? c2.h : 0u);
    p.mmod = mmod;
    p.slbg = slbg;
    p.alp = alp;
    p.bg = request.bgcolor;
    p.displayFbp = c1.on ? c1.fbp : c2.fbp;
    if (c1.on)
    {
        RenderTarget *rt = getRenderTarget(c1.fbp, c1.fbw, c1.psm, 1);
        p.has1 = true;
        p.tex1 = rt->tex;
        p.w1 = rt->width;
        p.h1 = rt->height;
        p.dbx1 = c1.dbx;
        p.dby1 = c1.dby;
        p.cw1 = c1.w;
        p.ch1 = c1.h;
    }
    if (c2.on)
    {
        RenderTarget *rt = getRenderTarget(c2.fbp, c2.fbw, c2.psm, 1);
        p.has2 = true;
        p.tex2 = rt->tex;
        p.w2 = rt->width;
        p.h2 = rt->height;
        p.dbx2 = c2.dbx;
        p.dby2 = c2.dby;
        p.cw2 = c2.w;
        p.ch2 = c2.h;
    }
}

void GSGlesBackend::composePlan(PresentationFrame &result, bool capture)
{
    const PresentPlan &p = m_plan;
    const uint32_t outW = p.outW, outH = p.outH;
    int viewW = 0, viewH = 0;
    if (capture)
    {
        if (m_captureW != static_cast<int>(outW) || m_captureH != static_cast<int>(outH))
        {
            if (m_captureFbo)
            {
                glDeleteFramebuffers(1, &m_captureFbo);
                glDeleteTextures(1, &m_captureTex);
            }
            glGenFramebuffers(1, &m_captureFbo);
            glGenTextures(1, &m_captureTex);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, m_captureTex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, outW, outH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glBindFramebuffer(GL_FRAMEBUFFER, m_captureFbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_captureTex, 0);
            m_captureW = static_cast<int>(outW);
            m_captureH = static_cast<int>(outH);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, m_captureFbo);
        viewW = m_captureW;
        viewH = m_captureH;
    }
    else
    {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        viewW = gsplayScreenWidth();
        viewH = gsplayScreenHeight();
    }
    glViewport(0, 0, viewW, viewH);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glUseProgram(m_presentProgram.id);
    const uint64_t bg = p.bg;
    glClearColor((bg & 0xFF) / 255.0f, ((bg >> 8) & 0xFF) / 255.0f, ((bg >> 16) & 0xFF) / 255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    const bool flip = !capture;
    if (p.has1 && p.has2)
    {
        // The two circuits merge: read circuit 2 first, then circuit 1 over it.
        if (!p.slbg)
            drawPresentQuad(p.tex2, p.w2, p.h2, p.dbx2, p.dby2, p.cw2, p.ch2, flip);
        glEnable(GL_BLEND);
        glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
        if (p.mmod)
        {
            glBlendColor(0.0f, 0.0f, 0.0f, p.alp / 255.0f);
            glBlendFuncSeparate(GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA, GL_ONE, GL_ZERO);
        }
        else
            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
        drawPresentQuad(p.tex1, p.w1, p.h1, p.dbx1, p.dby1, p.cw1, p.ch1, flip);
        glDisable(GL_BLEND);
    }
    else if (p.has1)
        drawPresentQuad(p.tex1, p.w1, p.h1, p.dbx1, p.dby1, p.cw1, p.ch1, flip);
    else
        drawPresentQuad(p.tex2, p.w2, p.h2, p.dbx2, p.dby2, p.cw2, p.ch2, flip);

    if (capture)
    {
        result.width = outW;
        result.height = outH;
        // The frontend reads rows at its own fixed stride.
        constexpr uint32_t kFrontendStride = GSGLES_FRONTEND_STRIDE;
        result.pixels.resize(static_cast<size_t>(kFrontendStride) * outH * 4u);
        glPixelStorei(GL_PACK_ROW_LENGTH, kFrontendStride);
        glReadPixels(0, 0, outW, outH, GL_RGBA, GL_UNSIGNED_BYTE, result.pixels.data());
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        result.displayFbp = p.displayFbp;
        result.sourceFbp = result.displayFbp;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

PresentationFrame GSGlesBackend::Present(const GSPresentationRequest &request)
{
    (void)request; // LatchFrame planned it
    PresentationFrame result{};
    const double t0 = nowMs();
    executeCommands();
    if (m_plan.any)
    {
        if (m_capture)
            composePlan(result, true);
        composePlan(result, false);
    }
    m_stats.presentMs += nowMs() - t0;
    ++m_stats.frames;
    return result;
}

bool GSGlesBackend::ClearFramebuffer(const GSContext &context, uint32_t rgba)
{
    return m_mem.ClearFramebuffer(context, rgba);
}

uint32_t GSGlesBackend::ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    return m_mem.ConsumeLocalToHostBytes(dst, maxBytes);
}

uint32_t GSGlesBackend::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    return m_mem.ReadVram(psm, base, bw, x, y);
}

void GSGlesBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    m_mem.WriteVram(psm, base, bw, x, y, value);
}

void GSGlesBackend::SnapshotVram(std::vector<uint8_t> &out) const
{
    m_mem.SnapshotVram(out);
}

GSTransferSnapshot GSGlesBackend::GetTransferSnapshot() const
{
    return m_mem.GetTransferSnapshot();
}

void GSGlesBackend::countStats(double out[18]) const
{
    // what is held, as LatchFrame last saw it: the recording thread may be changing the containers now
    for (int k = 0; k < 6; ++k)
        out[12 + k] = m_held[k];
    out[0] = static_cast<double>(m_stats.runs);
    out[1] = static_cast<double>(m_stats.vertices);
    out[2] = static_cast<double>(m_stats.passes);
    out[3] = static_cast<double>(m_stats.rtSwitches);
    out[4] = static_cast<double>(m_stats.texUploads);
    out[5] = static_cast<double>(m_stats.texBytes);
    out[6] = static_cast<double>(m_stats.rtUploads);
    out[7] = static_cast<double>(m_stats.rtFeedback);
    out[8] = static_cast<double>(m_stats.clutUploads);
    out[9] = static_cast<double>(m_rts.size());
    out[10] = static_cast<double>(m_programs.size());
    out[11] = m_stats.executeMs;
}

void GSGlesBackend::printStats() const
{
    {
        std::vector<std::pair<uint64_t, std::string>> states;
        for (const auto &e : m_stateLog)
            states.emplace_back(e.second, e.first);
        std::sort(states.rbegin(), states.rend());
        for (size_t i = 0; i < states.size() && i < 40u; ++i)
            std::printf("state %5llu %s\n", (unsigned long long)states[i].first, states[i].second.c_str());
    }
    const Stats &s = m_stats;
    std::printf("gles: %llu presents, %llu runs, %llu passes, %llu vertices, %llu RT binds, %llu program binds, %zu programs\n",
                (unsigned long long)s.frames, (unsigned long long)s.runs, (unsigned long long)s.passes, (unsigned long long)s.vertices,
                (unsigned long long)s.rtSwitches, (unsigned long long)s.programSwitches, m_programs.size());
    std::printf("gles: textures %llu resolves, %llu page hits, %llu raw hits, %llu hash hits, %llu uploads (%.1f MB), %llu CLUTs, %llu from RT (%llu feedback), %zu RTs\n",
                (unsigned long long)s.texResolves, (unsigned long long)s.texPageHits, (unsigned long long)s.texRawHits, (unsigned long long)s.texHashHits,
                (unsigned long long)s.texUploads, s.texBytes / 1048576.0, (unsigned long long)s.clutUploads,
                (unsigned long long)s.rtTextures, (unsigned long long)s.rtFeedback, m_rts.size());
    std::printf("gles: fbfetch runs %llu, no-fetch fallbacks %llu, partial FBMSK %llu, RT uploads %llu\n",
                (unsigned long long)s.fbBlendRuns, (unsigned long long)s.fbBlendMissing, (unsigned long long)s.partialMask, (unsigned long long)s.rtUploads);
    std::printf("gles: ms decode %.2f, hash %.2f, upload %.2f, execute %.2f, present %.2f, vram %.2f\n", s.decodeMs, s.hashMs, s.uploadMs, s.executeMs, s.presentMs, s.vramMs);
}
