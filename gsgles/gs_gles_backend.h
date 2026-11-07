// GS rendering through GLES 3.0, behind the runtime's GSRasterBackend interface.
//
// Local memory stays on the CPU: a contained GSCpuBackend does the swizzled transfers into the 4 MB
// shadow, and draws go to GL render targets instead of that shadow. Rules from docs/PHASE_2.md B5:
// one vertex upload per frame, draws recorded and run in order at present (1); no framebuffer reads
// except in capture mode (6); indexed textures as R8 plus a CLUT texture looked up in the shader (4),
// cached by page generation and then by content hash; PS2 blends GLES can't express go through
// GL_ARM_shader_framebuffer_fetch, everything else through fixed-function blending (3).
//
// Render target alpha is stored as a / 128, clamped, so GL's SRC_ALPHA and DST_ALPHA factors mean what
// the GS's As and Ad mean. The cost is that alpha above 0x80 is lost once written.
//
// Two threads: everything from the frontend records and makes no GL call, so the game thread never waits on the
// driver or the display. What used to touch GL while recording (texture and CLUT uploads, program compiles, render
// targets) is an op in the command stream, with GL names taken from a pool made ahead of time. The thread that owns
// the context calls LatchFrame, with the frontend's state lock held, to take the frame, then Present to run it.
#pragma once

#include "runtime/gs/gs_backend.h"
#include "runtime/gs/gs_cpu_backend.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class GSGlesBackend final : public GSRasterBackend
{
public:
    GSGlesBackend();
    ~GSGlesBackend() override;

    // The next Present reads its picture back, for comparison with a reference.
    void setCaptureNextPresent(bool capture) { m_capture = capture; }
    void printStats() const;
    // Runs, vertices, draw passes, target switches, texture uploads, texture bytes, target uploads, target feedback copies, CLUT uploads, render targets, GL programs, ms executing,
    // then what is held now: palettes, target bytes, depth buffers, depth bytes, content textures, content bytes
    void countStats(double out[18]) const;

    void Initialize(uint8_t *vram, uint32_t vramSize) override;
    void Reset() override;
    void Submit(const GSPrimitiveBatch &batch) override;
    bool SubmitSameState(const GSVertex *vertices, int count) override;
    bool SubmitPackedStrip(const uint8_t *qwords, uint32_t count, const GSVertex *queue) override;
    void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) override;
    void BeginTransfer(const GSTransferCommand &command) override;
    void UploadImage(const uint8_t *data, uint32_t sizeBytes) override;
    void Flush() override;
    void TextureFlush() override;
    void Sync(GSSyncReason reason) override;
    void LatchFrame(const GSPresentationRequest &request) override;
    PresentationFrame Present(const GSPresentationRequest &request) override;
    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override;
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override;
    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override;
    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override;
    void SnapshotVram(std::vector<uint8_t> &out) const override;
    GSTransferSnapshot GetTransferSnapshot() const override;

private:
    struct HostVertex
    {
        float x, y, z;
        float s, t, q;
        uint8_t r, g, b, a;
        uint8_t fog, pad[3];
    };

    struct RenderTarget
    {
        uint32_t fbp = 0, fbw = 0;
        uint8_t psm = 0;
        int width = 0, height = 0;
        unsigned tex = 0;
        unsigned copyTex = 0;
        bool drawn = false;
    };

    struct DepthBuffer
    {
        unsigned rb = 0;
        int width = 0, height = 0;
    };

    struct Program
    {
        unsigned id = 0;
        int uScale = -1, uTex = -1, uClut = -1, uTexSize = -1, uUvScale = -1, uFog = -1, uAref = -1, uFix = -1,
            uRtAlpha = -1, uTa = -1, uRegion = -1, uRegionMode = -1;
        // Last values sent, to skip redundant updates. The -1s match nothing a draw sets.
        float texSize[2] = {-1, -1}, uvScale[2] = {-1, -1}, fog[3] = {-1, -1, -1}, ta[3] = {-1, -1, -1};
        float aref = -1, fix = -1, rtAlpha = -1;
        int scaleW = 0, scaleH = 0;
    };

    struct TextureEntry
    {
        unsigned tex = 0;
        int w = 0, h = 0;
        bool indexed = false;
        size_t bytes = 0;
        uint32_t lastUsed = 0;
    };

    // What a TEX0 address and format holds right now, as of pageSig.
    struct TextureSlot
    {
        uint64_t pageSig = 0;
        TextureEntry *current = nullptr;
    };

    // One run of primitives with unchanged state, or an RT update, in frame order.
    struct Command
    {
        enum class Type : uint8_t
        {
            Draw,
            RtUpload,
            RtCopy,
            GlOp,
        };
        Type type = Type::Draw;
        unsigned mode = 0;
        uint32_t first = 0, count = 0;
        RenderTarget *rt = nullptr;
        // the target as it is at this point in the stream: it may grow later, before this runs
        unsigned rtTex = 0, copyTex = 0;
        int rtW = 0, rtH = 0;
        uint32_t opIndex = 0;
        DepthBuffer *depth = nullptr;
        Program *program = nullptr;
        Program *inverseProgram = nullptr; // second pass for alpha-test AFAIL modes
        uint8_t afail = 0;
        bool firstPassEnabled = true;
        unsigned tex = 0, clut = 0, sampler = 0;
        float texSize[2] = {1, 1}, uvScale[2] = {1, 1};
        float fog[3] = {0, 0, 0};
        float aref = 0, fix = 0, rtAlpha = 1;
        float ta[3] = {0, 0, 0}; // TEXA for a render target sampled as a format with less alpha
        float region[4] = {0, 0, 0, 0};
        int regionMode[2] = {0, 0};
        bool blend = false;
        unsigned srcRgb = 0, dstRgb = 0, equation = 0;
        float blendConstant = 0;
        uint8_t colorMask = 0xF; // bit 0 R, 1 G, 2 B, 3 A
        bool depthTest = false, depthWrite = false;
        unsigned depthFunc = 0;
        int scissor[4] = {0, 0, 0, 0};
        // RtUpload: rectangle and the index of its pixels in m_uploadPixels
        int rectX = 0, rectY = 0, rectW = 0, rectH = 0;
        uint32_t uploadIndex = 0;
    };

    void ensureGlObjects();
    void destroyGlObjects();
    void beginRun(const GSDrawState &state);
    void appendPrimitive(const GSVertex *verts, int count);
    void fastUpload(const uint8_t *data, uint32_t sizeBytes);
    bool sameState(const GSDrawState &a, const GSDrawState &b) const;
    RenderTarget *findRenderTarget(uint32_t fbp, uint32_t fbw);
    RenderTarget *getRenderTarget(uint32_t fbp, uint32_t fbw, uint8_t psm, int needHeight);
    DepthBuffer *getDepthBuffer(uint32_t zbp, int width, int height);
    unsigned getFramebuffer(unsigned tex, DepthBuffer *depth, int w, int h);
    void pushOp(std::function<void()> op);
    void refillNames();
    static unsigned takeName(std::vector<unsigned> &pool, const char *what);
    void setTarget(Command &cmd, RenderTarget *rt);
    void planPresent(const GSPresentationRequest &request);
    void composePlan(PresentationFrame &result, bool capture);
    void loadRtFromVram(RenderTarget &rt, int x, int y, int w, int h, std::vector<uint8_t> &out);
    Program *getProgram(uint32_t key);
    TextureEntry *resolveTexture(const GSDrawState &state);
    unsigned resolveClut(const GSTex0Reg &tex0, const GSTexaReg &texa);
    void evictTextures();
    unsigned getSampler(unsigned filter, unsigned wrapS, unsigned wrapT);
    uint64_t pageSignature(uint32_t tbp0, uint32_t tbw, uint8_t psm, uint32_t w, uint32_t h) const;
    void texturePages(uint32_t tbp0, uint32_t tbw, uint8_t psm, uint32_t w, uint32_t h, uint32_t &first, uint32_t &count) const;
    void decodeTexture(const GSTex0Reg &t, const GSTexaReg &texa, uint32_t w, uint32_t h);
    void bumpPages(uint32_t dbp, uint32_t dbw, uint8_t psm, uint32_t y, uint32_t h);
    void finishTransferIfDone();
    void executeCommands();
    void drawPresentQuad(unsigned tex, int texW, int texH, uint32_t dbx, uint32_t dby, uint32_t w, uint32_t h, bool flip);

    GSCpuBackend m_mem;
    uint8_t *m_vram = nullptr;
    bool m_capture = false;
    bool m_glReady = false;
    bool m_fbfetch = false;
    bool m_fbfetchArm = false;

    unsigned m_vao = 0, m_vbo = 0;
    unsigned m_captureFbo = 0, m_captureTex = 0, m_captureRb = 0;
    int m_captureW = 0, m_captureH = 0;
    Program m_presentProgram;
    unsigned m_copyFbo[2] = {0, 0};

    std::vector<std::unique_ptr<RenderTarget>> m_rts;
    std::map<uint64_t, std::unique_ptr<DepthBuffer>> m_depths;
    std::map<uint64_t, unsigned> m_fbos;
    std::unordered_map<uint32_t, std::unique_ptr<Program>> m_programs;
    std::unordered_map<uint64_t, TextureSlot> m_textureSlots;
    std::unordered_map<uint64_t, TextureEntry> m_contentTextures;
    std::unordered_map<uint64_t, unsigned> m_samplers;
    size_t m_textureBytes = 0;
    // Palettes by content. CLUT memory's hash changes with every load, including parts a draw doesn't read, so m_clutKeys only remembers which
    // palette a hash and a way of reading it came to; left alone, one texture per hash ran the device out of memory in three minutes.
    struct ClutEntry
    {
        unsigned tex = 0;
        uint32_t lastUsed = 0;
    };
    std::unordered_map<uint64_t, ClutEntry> m_cluts;
    std::unordered_map<uint64_t, ClutEntry *> m_clutKeys;
    void evictCluts();

    std::vector<HostVertex> m_vertices;
    std::vector<Command> m_commands;
    std::vector<std::vector<uint8_t>> m_uploadPixels;
    std::vector<std::function<void()>> m_glOps;
    // The frame taken by LatchFrame: only the context's thread touches these.
    std::vector<HostVertex> m_execVertices;
    std::vector<Command> m_execCommands;
    std::vector<std::vector<uint8_t>> m_execUploads;
    std::vector<std::function<void()>> m_execOps;
    std::vector<unsigned> m_texNames, m_samplerNames, m_rbNames;
    unsigned m_presentSampler = 0;
    struct PresentPlan
    {
        bool any = false, has1 = false, has2 = false, mmod = false, slbg = false;
        unsigned tex1 = 0, tex2 = 0;
        int w1 = 0, h1 = 0, w2 = 0, h2 = 0;
        uint32_t dbx1 = 0, dby1 = 0, cw1 = 0, ch1 = 0, dbx2 = 0, dby2 = 0, cw2 = 0, ch2 = 0;
        uint32_t outW = 0, outH = 0, alp = 0, displayFbp = 0;
        uint64_t bg = 0;
    };
    PresentPlan m_plan;
    GSDrawState m_lastState{};
    bool m_needResolve = true;
    GSTransferCommand m_transfer{};
    bool m_transferPending = false;
    uint32_t m_pageGen[512] = {};
    uint16_t m_clutRam[512] = {};
    uint64_t m_clutRamHash = 0;
    uint32_t m_clutCbp[2] = {0, 0};
    struct ClutLoad
    {
        uint32_t first = 0xFFFFFFFFu, count = 0, destination = 0, mask = 0;
        bool wide = false;
        uint64_t sig = 0;
        std::vector<uint32_t> values;
        bool full = false;
        std::vector<uint16_t> ram;
    };
    uint64_t m_lastClutKey = ~0ull, m_lastClutSig = 0;
    std::unordered_map<uint64_t, ClutLoad> m_clutLoads;
    bool m_fastUpload = false;
    uint32_t m_uploadCopied = 0, m_uploadTotal = 0;
    uint32_t m_frameIndex = 0;
    std::vector<uint8_t> m_decode;
    std::map<std::string, uint64_t> m_stateLog;
    float m_runZScale = 0.0f;

    struct Stats
    {
        uint64_t frames = 0, runs = 0, vertices = 0, passes = 0, rtSwitches = 0, programSwitches = 0;
        uint64_t texResolves = 0, texPageHits = 0, texRawHits = 0, texHashHits = 0, texUploads = 0, texBytes = 0;
        uint64_t clutUploads = 0, rtTextures = 0, rtFeedback = 0, fbBlendRuns = 0, fbBlendMissing = 0;
        uint64_t rtUploads = 0, partialMask = 0;
        double vramMs = 0, submitMs = 0, decodeMs = 0, hashMs = 0, uploadMs = 0, executeMs = 0, presentMs = 0;
    };
    Stats m_stats;
};
