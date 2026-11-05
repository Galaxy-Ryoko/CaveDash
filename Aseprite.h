#pragma once
// Minimal .aseprite loader (RGBA only, raw/compressed/linked cels) based on the
// chunk layout used by Samwhamtymam/Aseprite-Parser and the official ase-file-specs.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <filesystem>
#include <iterator>
#include <string>
#include <vector>
#include <zlib.h>

class AsepriteImage
{
public:
    struct Frame
    {
        std::vector<uint8_t> rgba; // width * height * 4, straight alpha
        int durationMs = 100;
    };

    int width = 0;
    int height = 0;
    std::vector<Frame> frames;

    // Composites the visible layers of every frame, except layers named in skipLayers.
    bool Load(const std::wstring& path, const std::vector<std::string>& skipLayers = {})
    {
        std::ifstream f(std::filesystem::path(path), std::ios::binary);
        if (!f) return false;
        std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (buf.size() < 128) return false;

        Reader r{ buf, 0 };
        r.Skip(4);
        if (r.U16() != 0xA5E0) return false;
        int frameCount = r.U16();
        width = r.U16();
        height = r.U16();
        if (r.U16() != 32 || frameCount <= 0) return false; // RGBA only
        r.pos = 128;
        frames.assign(frameCount, Frame{});

        struct LayerInfo { bool visible; uint8_t opacity; };
        std::vector<LayerInfo> layers;
        struct CelInfo { int x, y, w, h; uint8_t opacity; std::vector<uint8_t> px; };
        // Keyed by layer index so linked cels resolve to the right layer and draw bottom-up.
        std::vector<std::map<int, CelInfo>> celsByFrame(frameCount);

        for (int fr = 0; fr < frameCount; ++fr)
        {
            size_t frameStart = r.pos;
            uint32_t frameSize = r.U32();
            r.Skip(2);
            uint32_t chunks = r.U16();
            frames[fr].durationMs = r.U16();
            r.Skip(2);
            uint32_t newCount = r.U32();
            if (newCount) chunks = newCount;

            for (uint32_t c = 0; c < chunks; ++c)
            {
                size_t chunkStart = r.pos;
                uint32_t chunkSize = r.U32();
                uint16_t type = r.U16();
                size_t chunkEnd = chunkStart + chunkSize;

                if (type == 0x2004) // layer
                {
                    uint16_t flags = r.U16();
                    uint16_t layerType = r.U16();
                    r.Skip(2 + 4 + 2); // child level, default size, blend mode
                    uint8_t opacity = r.U8();
                    r.Skip(3);
                    uint16_t nameLen = r.U16();
                    std::string name;
                    for (uint16_t i = 0; i < nameLen; ++i) name += static_cast<char>(r.U8());
                    bool skipped = std::find(skipLayers.begin(), skipLayers.end(), name) != skipLayers.end();
                    layers.push_back({ (flags & 1) != 0 && layerType == 0 && !skipped, opacity });
                }
                else if (type == 0x2005) // cel
                {
                    uint16_t layer = r.U16();
                    CelInfo cel{};
                    cel.x = static_cast<int16_t>(r.U16());
                    cel.y = static_cast<int16_t>(r.U16());
                    cel.opacity = r.U8();
                    uint16_t celType = r.U16();
                    r.Skip(7);
                    if (layer < layers.size() && layers[layer].visible)
                    {
                        cel.opacity = static_cast<uint8_t>(cel.opacity * layers[layer].opacity / 255);
                        if (celType == 0 || celType == 2)
                        {
                            cel.w = r.U16();
                            cel.h = r.U16();
                            size_t bytes = static_cast<size_t>(cel.w) * cel.h * 4;
                            if (celType == 0)
                            {
                                cel.px.assign(buf.begin() + r.pos, buf.begin() + r.pos + bytes);
                            }
                            else
                            {
                                cel.px.resize(bytes);
                                uLongf outLen = static_cast<uLongf>(bytes);
                                if (uncompress(cel.px.data(), &outLen, buf.data() + r.pos,
                                    static_cast<uLong>(chunkEnd - r.pos)) != Z_OK)
                                    cel.px.clear();
                            }
                            celsByFrame[fr][layer] = std::move(cel);
                        }
                        else if (celType == 1) // linked cel: same layer in an earlier frame
                        {
                            uint16_t linked = r.U16();
                            if (linked < celsByFrame.size())
                            {
                                auto it = celsByFrame[linked].find(layer);
                                if (it != celsByFrame[linked].end())
                                    celsByFrame[fr][layer] = it->second;
                            }
                        }
                    }
                }
                r.pos = chunkEnd;
            }
            r.pos = frameStart + frameSize;
        }

        for (int fr = 0; fr < frameCount; ++fr)
        {
            std::vector<uint8_t>& rgba = frames[fr].rgba;
            rgba.assign(static_cast<size_t>(width) * height * 4, 0);
            for (const auto& entry : celsByFrame[fr])
                Composite(rgba, entry.second.x, entry.second.y, entry.second.w, entry.second.h,
                    entry.second.opacity, entry.second.px);
        }
        return true;
    }

private:
    void Composite(std::vector<uint8_t>& rgba, int cx, int cy, int cw, int ch, uint8_t opacity,
        const std::vector<uint8_t>& px) const
    {
        for (int y = 0; y < ch; ++y)
            for (int x = 0; x < cw; ++x)
            {
                int dx = cx + x, dy = cy + y;
                if (dx < 0 || dy < 0 || dx >= width || dy >= height) continue;
                if (px.size() < static_cast<size_t>((y * cw + x) * 4 + 4)) continue;
                const uint8_t* s = &px[(y * cw + x) * 4];
                uint8_t* d = &rgba[(dy * width + dx) * 4];
                float sa = s[3] / 255.0f * opacity / 255.0f;
                float da = d[3] / 255.0f;
                float oa = sa + da * (1 - sa);
                if (oa <= 0) continue;
                for (int k = 0; k < 3; ++k)
                    d[k] = static_cast<uint8_t>((s[k] * sa + d[k] * da * (1 - sa)) / oa);
                d[3] = static_cast<uint8_t>(oa * 255);
            }
    }

    struct Reader
    {
        const std::vector<uint8_t>& b;
        size_t pos;
        uint8_t U8() { return pos < b.size() ? b[pos++] : 0; }
        uint16_t U16() { uint16_t lo = U8(); return static_cast<uint16_t>(lo | (U8() << 8)); }
        uint32_t U32() { uint32_t lo = U16(); return lo | (static_cast<uint32_t>(U16()) << 16); }
        void Skip(size_t n) { pos += n; }
    };
};
