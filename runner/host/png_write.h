// A PNG writer with no dependencies: 8-bit RGB, stored (uncompressed) deflate blocks. A 640x448 frame is about 860 KB and takes a
// few milliseconds; meant for screenshots, written off the threads that matter.
#pragma once
#include <cstdint>
#include <cstdio>
#include <vector>

namespace ps2portpng
{
inline uint32_t crc32(const uint8_t *data, size_t size, uint32_t crc = 0u)
{
    static uint32_t table[256];
    static bool ready = false;
    if (!ready)
    {
        for (uint32_t i = 0; i < 256u; ++i)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < size; ++i)
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

inline void put32(std::vector<uint8_t> &out, uint32_t v)
{
    out.push_back(static_cast<uint8_t>(v >> 24));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

inline void chunk(std::vector<uint8_t> &out, const char *tag, const std::vector<uint8_t> &body)
{
    put32(out, static_cast<uint32_t>(body.size()));
    const size_t start = out.size();
    out.insert(out.end(), tag, tag + 4);
    out.insert(out.end(), body.begin(), body.end());
    put32(out, crc32(out.data() + start, out.size() - start));
}

// rgba: width x height pixels, 4 bytes each (alpha ignored), rows `stride` bytes apart. False if the file cannot be written.
inline bool write(const char *path, const uint8_t *rgba, uint32_t width, uint32_t height, size_t stride)
{
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(height) * (width * 3u + 1u));
    for (uint32_t y = 0; y < height; ++y)
    {
        raw.push_back(0u); // filter: none
        const uint8_t *in = rgba + stride * y;
        for (uint32_t x = 0; x < width; ++x)
        {
            raw.push_back(in[x * 4u]);
            raw.push_back(in[x * 4u + 1u]);
            raw.push_back(in[x * 4u + 2u]);
        }
    }
    std::vector<uint8_t> z;
    z.reserve(raw.size() + raw.size() / 65535u * 5u + 16u);
    z.push_back(0x78u);
    z.push_back(0x01u);
    uint32_t a = 1u, b = 0u;
    for (size_t at = 0; at < raw.size();)
    {
        const size_t n = raw.size() - at < 65535u ? raw.size() - at : 65535u;
        z.push_back(at + n == raw.size() ? 1u : 0u);
        z.push_back(static_cast<uint8_t>(n));
        z.push_back(static_cast<uint8_t>(n >> 8));
        z.push_back(static_cast<uint8_t>(~n));
        z.push_back(static_cast<uint8_t>(~n >> 8));
        for (size_t i = 0; i < n; ++i)
        {
            a += raw[at + i];
            if (a >= 65521u)
                a -= 65521u;
            b += a;
            if (b >= 65521u)
                b -= 65521u;
        }
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(at), raw.begin() + static_cast<std::ptrdiff_t>(at + n));
        at += n;
    }
    put32(z, (b << 16) | a);

    std::vector<uint8_t> out = {0x89u, 'P', 'N', 'G', '\r', '\n', 0x1Au, '\n'};
    std::vector<uint8_t> header;
    put32(header, width);
    put32(header, height);
    header.insert(header.end(), {8u, 2u, 0u, 0u, 0u});
    chunk(out, "IHDR", header);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", {});
    FILE *f = std::fopen(path, "wb");
    if (!f)
        return false;
    const bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}
}
