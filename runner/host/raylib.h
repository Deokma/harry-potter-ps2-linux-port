// Stands in for raylib on a headless build: the same names the runtime uses, no window, no input, no audio.
// The frame still goes through a "texture" that is just memory, so PS2PORT_FRAME_DIR keeps working (PPM, not PNG).
// EndDrawing paces to 60 Hz, as raylib's SetTargetFPS(60) does. A real display arrives with the GLES backend.
#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

struct Color
{
    unsigned char r, g, b, a;
};
struct Rectangle
{
    float x, y, width, height;
};
struct Vector2
{
    float x, y;
};
struct Image
{
    void *data;
    int width, height, mipmaps, format;
};
struct Texture2D
{
    unsigned int id;
    int width, height, mipmaps, format;
    void *pixels;
};

constexpr Color WHITE{255, 255, 255, 255}, BLACK{0, 0, 0, 255}, BLANK{0, 0, 0, 0}, MAGENTA{255, 0, 255, 255};
enum
{
    FLAG_WINDOW_RESIZABLE = 4,
    FLAG_WINDOW_HIDDEN = 128,
    LOG_NONE = 7,
    PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 = 7,
};
enum KeyboardKey
{
    KEY_NULL = 0,
    KEY_SPACE = 32,
    KEY_ONE = 49,
    KEY_THREE = 51,
    KEY_A = 65,
    KEY_C = 67,
    KEY_D = 68,
    KEY_E = 69,
    KEY_Q = 81,
    KEY_S = 83,
    KEY_V = 86,
    KEY_W = 87,
    KEY_X = 88,
    KEY_Z = 90,
    KEY_ESCAPE = 256,
    KEY_ENTER = 257,
    KEY_TAB = 258,
    KEY_RIGHT = 262,
    KEY_LEFT = 263,
    KEY_DOWN = 264,
    KEY_UP = 265,
    KEY_KP_0 = 320,
    KEY_KP_1 = 321,
    KEY_LEFT_SHIFT = 340,
    KEY_LEFT_CONTROL = 341,
    KEY_RIGHT_SHIFT = 344,
    KEY_RIGHT_CONTROL = 345,
};
enum GamepadButton
{
    GAMEPAD_BUTTON_UNKNOWN = 0,
    GAMEPAD_BUTTON_LEFT_FACE_UP,
    GAMEPAD_BUTTON_LEFT_FACE_RIGHT,
    GAMEPAD_BUTTON_LEFT_FACE_DOWN,
    GAMEPAD_BUTTON_LEFT_FACE_LEFT,
    GAMEPAD_BUTTON_RIGHT_FACE_UP,
    GAMEPAD_BUTTON_RIGHT_FACE_RIGHT,
    GAMEPAD_BUTTON_RIGHT_FACE_DOWN,
    GAMEPAD_BUTTON_RIGHT_FACE_LEFT,
    GAMEPAD_BUTTON_LEFT_TRIGGER_1,
    GAMEPAD_BUTTON_LEFT_TRIGGER_2,
    GAMEPAD_BUTTON_RIGHT_TRIGGER_1,
    GAMEPAD_BUTTON_RIGHT_TRIGGER_2,
    GAMEPAD_BUTTON_MIDDLE_LEFT,
    GAMEPAD_BUTTON_MIDDLE,
    GAMEPAD_BUTTON_MIDDLE_RIGHT,
    GAMEPAD_BUTTON_LEFT_THUMB,
    GAMEPAD_BUTTON_RIGHT_THUMB,
};
enum GamepadAxis
{
    GAMEPAD_AXIS_LEFT_X = 0,
    GAMEPAD_AXIS_LEFT_Y = 1,
    GAMEPAD_AXIS_RIGHT_X = 2,
    GAMEPAD_AXIS_RIGHT_Y = 3,
};

inline void SetConfigFlags(unsigned) {}
inline void SetTraceLogLevel(int) {}
inline void InitWindow(int, int, const char *) {}
inline void InitAudioDevice() {}
inline bool IsAudioDeviceReady() { return false; }
inline void CloseAudioDevice() {}
struct Wave
{
    unsigned int frameCount, sampleRate, sampleSize, channels;
    void *data;
};
struct Sound
{
    unsigned int frameCount;
};
inline Wave LoadWaveFromMemory(const char *, const unsigned char *, int) { return Wave{0u, 0u, 0u, 0u, nullptr}; }
inline Sound LoadSoundFromWave(Wave) { return Sound{0u}; }
inline void UnloadWave(Wave) {}
inline void UnloadSound(Sound) {}
inline void PlaySound(Sound) {}
inline void StopSound(Sound) {}
inline bool IsSoundPlaying(Sound) { return false; }
inline void SetSoundVolume(Sound, float) {}
inline void SetSoundPitch(Sound, float) {}
inline void SetTargetFPS(int) {}
inline bool IsWindowReady() { return true; }
inline bool WindowShouldClose() { return false; }
inline void CloseWindow() {}
inline int GetScreenWidth() { return 640; }
inline int GetScreenHeight() { return 448; }
inline bool IsKeyDown(int) { return false; }
inline bool IsKeyPressed(int) { return false; }
inline bool IsGamepadAvailable(int) { return false; }
inline bool IsGamepadButtonDown(int, int) { return false; }
inline float GetGamepadAxisMovement(int, int) { return 0.0f; }

inline Image GenImageColor(int width, int height, Color color)
{
    Image image{std::malloc(static_cast<size_t>(width) * height * 4), width, height, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
    for (int i = 0; i < width * height; ++i)
        std::memcpy(static_cast<uint8_t *>(image.data) + i * 4, &color, 4);
    return image;
}
inline void UnloadImage(Image image) { std::free(image.data); }
inline Texture2D LoadTextureFromImage(Image image)
{
    Texture2D texture{1u, image.width, image.height, 1, image.format, std::malloc(static_cast<size_t>(image.width) * image.height * 4)};
    std::memcpy(texture.pixels, image.data, static_cast<size_t>(image.width) * image.height * 4);
    return texture;
}
inline void UnloadTexture(Texture2D texture) { std::free(texture.pixels); }
inline void UpdateTexture(Texture2D texture, const void *pixels) { std::memcpy(texture.pixels, pixels, static_cast<size_t>(texture.width) * texture.height * 4); }
inline Image LoadImageFromTexture(Texture2D texture)
{
    Image image{std::malloc(static_cast<size_t>(texture.width) * texture.height * 4), texture.width, texture.height, 1, texture.format};
    std::memcpy(image.data, texture.pixels, static_cast<size_t>(texture.width) * texture.height * 4);
    return image;
}
inline void ImageCrop(Image *image, Rectangle r)
{
    const int w = static_cast<int>(r.width), h = static_cast<int>(r.height), x0 = static_cast<int>(r.x), y0 = static_cast<int>(r.y);
    void *out = std::malloc(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y)
        std::memcpy(static_cast<uint8_t *>(out) + static_cast<size_t>(y) * w * 4, static_cast<uint8_t *>(image->data) + (static_cast<size_t>(y + y0) * image->width + x0) * 4, static_cast<size_t>(w) * 4);
    std::free(image->data);
    image->data = out;
    image->width = w;
    image->height = h;
}
// A PPM, whatever the name says
inline bool ExportImage(Image image, const char *path)
{
    FILE *f = std::fopen(path, "wb");
    if (!f)
        return false;
    std::fprintf(f, "P6\n%d %d\n255\n", image.width, image.height);
    const uint8_t *p = static_cast<const uint8_t *>(image.data);
    for (int i = 0; i < image.width * image.height; ++i, p += 4)
        std::fwrite(p, 1, 3, f);
    std::fclose(f);
    return true;
}

inline void BeginDrawing() {}
inline void ClearBackground(Color) {}
inline void DrawTexturePro(Texture2D, Rectangle, Rectangle, Vector2, float, Color) {}
inline void EndDrawing()
{
    // With a real display, its own swap paces the loop.
    static const bool paced = std::getenv("PS2PORT_DISPLAY") != nullptr;
    if (paced)
        return;
    static auto next = std::chrono::steady_clock::now();
    next += std::chrono::microseconds(16667);
    const auto now = std::chrono::steady_clock::now();
    if (next > now)
        std::this_thread::sleep_until(next);
    else if (now - next > std::chrono::milliseconds(100))
        next = now;
}
