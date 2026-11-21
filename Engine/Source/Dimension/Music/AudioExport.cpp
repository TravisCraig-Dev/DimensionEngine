#include "Dimension/Music/AudioExport.h"
#include "Dimension/Core/Log.h"
#include <opus.h>
#include <ogg/ogg.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <random>

namespace Dimension::Music
{
    namespace
    {
        void PutLE16(std::vector<unsigned char>& out, uint16_t v)
        {
            out.push_back((unsigned char)(v & 0xFF));
            out.push_back((unsigned char)(v >> 8));
        }
        void PutLE32(std::vector<unsigned char>& out, uint32_t v)
        {
            for (int i = 0; i < 4; ++i) out.push_back((unsigned char)((v >> (8 * i)) & 0xFF));
        }
        void WritePages(ogg_stream_state& os, std::ofstream& file, bool flush)
        {
            ogg_page page;
            while (flush ? ogg_stream_flush(&os, &page) : ogg_stream_pageout(&os, &page))
            {
                file.write((const char*)page.header, page.header_len);
                file.write((const char*)page.body, page.body_len);
            }
        }
    }

    bool WriteOpus(const fs::path& path, const std::vector<float>& stereo, uint32_t sampleRate, int kbps)
    {
        if (sampleRate != 48000)
        {
            Log::Error("Opus files are written at 48 kHz (this audio is {} Hz)", sampleRate);
            return false;
        }
        std::ofstream file(path, std::ios::binary);
        if (!file) return false;
        int error = 0;
        OpusEncoder* encoder = opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &error);
        if (!encoder || error != OPUS_OK) return false;
        opus_encoder_ctl(encoder, OPUS_SET_BITRATE(std::clamp(kbps, 6, 510) * 1000));
        opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(10));
        opus_int32 preSkip = 0;
        opus_encoder_ctl(encoder, OPUS_GET_LOOKAHEAD(&preSkip));

        ogg_stream_state os;
        ogg_stream_init(&os, (int)(std::random_device{}() & 0x7FFFFFFF));
        ogg_int64_t packetNo = 0;
        auto packet = [&](unsigned char* data, long bytes, bool first, bool last, ogg_int64_t granule) {
            ogg_packet op{};
            op.packet = data;
            op.bytes = bytes;
            op.b_o_s = first;
            op.e_o_s = last;
            op.granulepos = granule;
            op.packetno = packetNo++;
            ogg_stream_packetin(&os, &op);
        };
        // The identification header (RFC 7845): version 1, two channels, the encoder's delay, 48 kHz, no gain, mapping 0.
        std::vector<unsigned char> head = { 'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 2 };
        PutLE16(head, (uint16_t)preSkip);
        PutLE32(head, 48000);
        PutLE16(head, 0);
        head.push_back(0);
        packet(head.data(), (long)head.size(), true, false, 0);
        WritePages(os, file, true);
        // The comment header: the vendor, no comments.
        const char* vendor = opus_get_version_string();
        std::vector<unsigned char> tags = { 'O', 'p', 'u', 's', 'T', 'a', 'g', 's' };
        PutLE32(tags, (uint32_t)std::strlen(vendor));
        tags.insert(tags.end(), vendor, vendor + std::strlen(vendor));
        PutLE32(tags, 0);
        packet(tags.data(), (long)tags.size(), false, false, 0);
        WritePages(os, file, true);

        // The audio: 20 ms frames; the end padded with silence past the encoder's delay. The last granule position
        // (pre-skip + the sound's length) tells players where it really ends.
        const int frameSize = 960;
        const int64_t frames = (int64_t)(stereo.size() / 2);
        const int64_t total = frames + preSkip;
        std::vector<float> pcm(frameSize * 2);
        std::vector<unsigned char> out(4000);
        bool ok = true;
        for (int64_t at = 0; at < total; at += frameSize)
        {
            for (int i = 0; i < frameSize; ++i)
            {
                int64_t f = at + i;
                pcm[i * 2] = f < frames ? std::clamp(stereo[f * 2], -1.0f, 1.0f) : 0.0f;
                pcm[i * 2 + 1] = f < frames ? std::clamp(stereo[f * 2 + 1], -1.0f, 1.0f) : 0.0f;
            }
            opus_int32 bytes = opus_encode_float(encoder, pcm.data(), frameSize, out.data(), (opus_int32)out.size());
            if (bytes < 0)
            {
                ok = false;
                break;
            }
            bool last = at + frameSize >= total;
            packet(out.data(), bytes, false, last, std::min(at + frameSize, total));
            WritePages(os, file, false);
        }
        WritePages(os, file, true);
        ogg_stream_clear(&os);
        opus_encoder_destroy(encoder);
        return ok && (bool)file;
    }

    float PeakOf(const std::vector<float>& samples)
    {
        float peak = 0.0f;
        for (float s : samples) peak = std::max(peak, std::fabs(s));
        return peak;
    }

    void Normalize(std::vector<float>& samples, float peakDb)
    {
        float peak = PeakOf(samples);
        if (peak < 1e-6f) return;
        float gain = std::pow(10.0f, peakDb / 20.0f) / peak;
        for (float& s : samples) s *= gain;
    }
}
