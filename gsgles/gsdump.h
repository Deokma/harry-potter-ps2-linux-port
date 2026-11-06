// Reader for an uncompressed PCSX2 2.x GS dump (scripts/gsdump_unpack.py makes one).
// Format, from PCSX2 v2.6.3 GSDump.cpp / GSState::Freeze: u32 0xFFFFFFFF, u32 header size,
// GSDumpHeader (9 x u32), serial, screenshot, GS state, 8 KiB privileged registers, then
// packets: 0 transfer (u8 path, u32 size, data), 1 vsync (u8 field), 2 FIFO read (u32 size),
// 3 privileged registers (8 KiB).
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

struct GsDump
{
    static constexpr uint32_t kVramSize = 4u * 1024u * 1024u;
    static constexpr uint32_t kPrivSize = 0x2000u;
    // GS state: version, 15 environment registers, 2 x 12 context registers, RGBAQ, ST, UV, FOG,
    // XYZ, one obsolete register, transfer state, 4 MiB of local memory, 4 x GIF path state, q.
    // The transfer state's size varies by version, so the memory is found from the end.
    static constexpr uint32_t kEnvOffset = 4u;
    static constexpr uint32_t kCtxOffset = kEnvOffset + 15u * 8u;
    static constexpr uint32_t kVertexOffset = kCtxOffset + 2u * 96u;
    static constexpr uint32_t kTailSize = 4u * 20u + 4u;

    enum class PacketType : uint8_t
    {
        Transfer = 0,
        Vsync = 1,
        FifoRead = 2,
        Registers = 3,
    };

    struct Packet
    {
        PacketType type = PacketType::Vsync;
        uint8_t path = 0;
        uint32_t size = 0;
        const uint8_t *data = nullptr;
    };

    std::vector<uint8_t> raw;
    const uint8_t *state = nullptr;
    const uint8_t *priv = nullptr;
    std::vector<Packet> packets;
    uint32_t stateVersion = 0;
    uint32_t stateSize = 0;
    std::string error;

    uint64_t env(uint32_t i) const { return read64(state + kEnvOffset + i * 8u); }
    uint64_t ctx(uint32_t c, uint32_t i) const { return read64(state + kCtxOffset + c * 96u + i * 8u); }
    uint64_t vertexReg(uint32_t i) const { return read64(state + kVertexOffset + i * 8u); }
    const uint8_t *vram() const { return state + stateSize - kTailSize - kVramSize; }
    const uint8_t *paths() const { return state + stateSize - kTailSize; }

    static uint64_t read64(const uint8_t *p)
    {
        uint64_t v;
        std::memcpy(&v, p, 8);
        return v;
    }
    static uint32_t read32(const uint8_t *p)
    {
        uint32_t v;
        std::memcpy(&v, p, 4);
        return v;
    }

    bool load(const std::string &path)
    {
        FILE *f = std::fopen(path.c_str(), "rb");
        if (!f)
        {
            error = "cannot open " + path;
            return false;
        }
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        raw.resize(static_cast<size_t>(size));
        const size_t got = std::fread(raw.data(), 1, raw.size(), f);
        std::fclose(f);
        if (got != raw.size() || raw.size() < 64u)
        {
            error = "short read";
            return false;
        }
        if (read32(raw.data()) != 0xFFFFFFFFu)
        {
            error = "not an uncompressed GS dump of the new format";
            return false;
        }
        const uint32_t headerSize = read32(raw.data() + 4);
        stateVersion = read32(raw.data() + 8);
        stateSize = read32(raw.data() + 12);
        size_t pos = 8u + headerSize;
        if (stateSize < kVertexOffset + kVramSize + kTailSize)
        {
            error = "unexpected GS state size " + std::to_string(stateSize);
            return false;
        }
        state = raw.data() + pos;
        pos += stateSize;
        priv = raw.data() + pos;
        pos += kPrivSize;
        while (pos < raw.size())
        {
            Packet p;
            p.type = static_cast<PacketType>(raw[pos++]);
            switch (p.type)
            {
            case PacketType::Transfer:
                p.path = raw[pos];
                p.size = read32(raw.data() + pos + 1);
                pos += 5u;
                p.data = raw.data() + pos;
                pos += p.size;
                break;
            case PacketType::Vsync:
                p.path = raw[pos++];
                break;
            case PacketType::FifoRead:
                p.size = read32(raw.data() + pos);
                pos += 4u;
                break;
            case PacketType::Registers:
                p.size = kPrivSize;
                p.data = raw.data() + pos;
                pos += kPrivSize;
                break;
            default:
                error = "unknown packet type at " + std::to_string(pos);
                return false;
            }
            packets.push_back(p);
        }
        return true;
    }
};
