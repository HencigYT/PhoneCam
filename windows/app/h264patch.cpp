#include "h264patch.h"
#include <stdexcept>

namespace {

class BitReader
{
public:
    explicit BitReader(const std::vector<uint8_t>& b) : _b(b) {}
    uint32_t U(int n)
    {
        uint32_t v = 0;
        for (int i = 0; i < n; i++)
        {
            if (_pos >= _b.size() * 8)
                throw std::out_of_range("sps");
            v = v << 1 | ((_b[_pos >> 3] >> (7 - (_pos & 7))) & 1);
            _pos++;
        }
        return v;
    }
    uint32_t Ue()
    {
        int zeros = 0;
        while (U(1) == 0)
            if (++zeros > 31)
                throw std::out_of_range("sps");
        return (1u << zeros) - 1 + U(zeros);
    }

private:
    const std::vector<uint8_t>& _b;
    size_t _pos = 0;
};

class BitWriter
{
public:
    void U(uint32_t v, int n)
    {
        for (int i = n - 1; i >= 0; i--)
            Bit((v >> i) & 1);
    }
    void Ue(uint32_t v)
    {
        uint32_t x = v + 1;
        int len = 0;
        while ((x >> len) > 1)
            len++;
        U(0, len);
        U(x, len + 1);
    }
    std::vector<uint8_t> Finish()
    {
        Bit(1); // rbsp_stop_one_bit
        while (_bits)
            Bit(0);
        return _out;
    }

private:
    void Bit(uint32_t b)
    {
        _cur = (uint8_t)(_cur << 1 | b);
        if (++_bits == 8)
        {
            _out.push_back(_cur);
            _cur = 0;
            _bits = 0;
        }
    }
    std::vector<uint8_t> _out;
    uint8_t _cur = 0;
    int _bits = 0;
};

// Reads from the original SPS while writing an identical copy.
struct Copier
{
    BitReader& r;
    BitWriter& w;
    uint32_t U(int n)
    {
        uint32_t v = r.U(n);
        w.U(v, n);
        return v;
    }
    uint32_t Ue()
    {
        uint32_t v = r.Ue();
        w.Ue(v);
        return v;
    }
    void Hrd()
    {
        uint32_t count = Ue();
        U(4);
        U(4);
        for (uint32_t i = 0; i <= count; i++)
        {
            Ue();
            Ue();
            U(1);
        }
        U(5);
        U(5);
        U(5);
        U(5);
    }
};

std::vector<uint8_t> Unescape(const uint8_t* p, size_t n)
{
    std::vector<uint8_t> o;
    o.reserve(n);
    int zeros = 0;
    for (size_t i = 0; i < n; i++)
    {
        if (zeros >= 2 && p[i] == 3)
        {
            zeros = 0;
            continue;
        }
        o.push_back(p[i]);
        zeros = p[i] == 0 ? zeros + 1 : 0;
    }
    return o;
}

void AppendEscaped(std::vector<uint8_t>& out, const std::vector<uint8_t>& rbsp)
{
    int zeros = 0;
    for (uint8_t c : rbsp)
    {
        if (zeros >= 2 && c <= 3)
        {
            out.push_back(3);
            zeros = 0;
        }
        out.push_back(c);
        zeros = c == 0 ? zeros + 1 : 0;
    }
}

std::vector<uint8_t> RewriteSps(const uint8_t* nal, size_t size)
{
    std::vector<uint8_t> rbsp = Unescape(nal + 1, size - 1);
    BitReader r(rbsp);
    BitWriter w;
    Copier c{ r, w };

    uint32_t profile = c.U(8);
    c.U(8);
    c.U(8);
    c.Ue();
    switch (profile)
    {
    case 100: case 110: case 122: case 244: case 44: case 83: case 86: case 118: case 128: case 138: case 139:
    case 134: case 135:
    {
        uint32_t chroma = c.Ue();
        if (chroma == 3)
            c.U(1);
        c.Ue();
        c.Ue();
        c.U(1);
        if (c.U(1)) // seq_scaling_matrix_present_flag
        {
            for (int i = 0; i < (chroma != 3 ? 8 : 12); i++)
            {
                if (!c.U(1))
                    continue;
                int last = 8, next = 8;
                for (int j = 0; j < (i < 6 ? 16 : 64); j++)
                {
                    if (next != 0)
                    {
                        uint32_t k = c.Ue(); // delta_scale as se(v)
                        int delta = (k & 1) ? (int)((k + 1) / 2) : -(int)(k / 2);
                        next = (last + delta + 256) % 256;
                    }
                    last = next == 0 ? last : next;
                }
            }
        }
        break;
    }
    }
    c.Ue(); // log2_max_frame_num_minus4
    uint32_t pocType = c.Ue();
    if (pocType == 0)
        c.Ue();
    else if (pocType == 1)
    {
        c.U(1);
        c.Ue();
        c.Ue();
        uint32_t n = c.Ue();
        for (uint32_t i = 0; i < n; i++)
            c.Ue();
    }
    uint32_t refFrames = c.Ue();
    c.U(1);
    c.Ue();
    c.Ue();
    if (!c.U(1)) // frame_mbs_only_flag
        c.U(1);
    c.U(1);
    if (c.U(1)) // frame_cropping_flag
    {
        c.Ue();
        c.Ue();
        c.Ue();
        c.Ue();
    }

    // VUI: keep everything (colour info matters), replace bitstream_restriction.
    uint32_t vui = r.U(1);
    w.U(1, 1);
    uint32_t mvOver = 1, bytesDenom = 2, bitsDenom = 1, mvH = 16, mvV = 16;
    if (vui)
    {
        if (c.U(1) && c.U(8) == 255)
        {
            c.U(16);
            c.U(16);
        }
        if (c.U(1))
            c.U(1);
        if (c.U(1))
        {
            c.U(3);
            c.U(1);
            if (c.U(1))
            {
                c.U(8);
                c.U(8);
                c.U(8);
            }
        }
        if (c.U(1))
        {
            c.Ue();
            c.Ue();
        }
        if (c.U(1))
        {
            c.U(32);
            c.U(32);
            c.U(1);
        }
        uint32_t nalHrd = c.U(1);
        if (nalHrd)
            c.Hrd();
        uint32_t vclHrd = c.U(1);
        if (vclHrd)
            c.Hrd();
        if (nalHrd || vclHrd)
            c.U(1);
        c.U(1); // pic_struct_present_flag
        if (r.U(1))
        {
            mvOver = r.U(1);
            bytesDenom = r.Ue();
            bitsDenom = r.Ue();
            mvH = r.Ue();
            mvV = r.Ue();
            r.Ue();
            r.Ue();
        }
    }
    else
    {
        w.U(0, 8); // no aspect/overscan/signal/chroma loc/timing/nal hrd/vcl hrd/pic_struct
    }
    w.U(1, 1); // bitstream_restriction_flag
    w.U(mvOver, 1);
    w.Ue(bytesDenom);
    w.Ue(bitsDenom);
    w.Ue(mvH);
    w.Ue(mvV);
    w.Ue(0);                                // max_num_reorder_frames
    w.Ue(refFrames > 0 ? refFrames : 1);    // max_dec_frame_buffering

    std::vector<uint8_t> out{ nal[0] };
    AppendEscaped(out, w.Finish());
    return out;
}

} // namespace

bool PatchH264NoReorder(const uint8_t* data, size_t size, std::vector<uint8_t>& out)
{
    // Find NAL units (Annex-B start codes).
    std::vector<size_t> starts, payloads;
    for (size_t i = 0; i + 3 <= size;)
    {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
        {
            starts.push_back(i > 0 && data[i - 1] == 0 ? i - 1 : i);
            payloads.push_back(i + 3);
            i += 3;
        }
        else
            i++;
    }
    bool hasSps = false;
    for (size_t p : payloads)
        if (p < size && (data[p] & 0x1F) == 7)
            hasSps = true;
    if (!hasSps)
        return false;

    out.clear();
    out.reserve(size + 16);
    out.insert(out.end(), data, data + (starts.empty() ? size : starts[0]));
    bool changed = false;
    for (size_t k = 0; k < payloads.size(); k++)
    {
        size_t p = payloads[k];
        size_t end = k + 1 < starts.size() ? starts[k + 1] : size;
        out.insert(out.end(), data + starts[k], data + p);
        if (p < end && (data[p] & 0x1F) == 7)
        {
            try
            {
                std::vector<uint8_t> sps = RewriteSps(data + p, end - p);
                out.insert(out.end(), sps.begin(), sps.end());
                changed = true;
                continue;
            }
            catch (const std::exception&)
            {
            }
        }
        out.insert(out.end(), data + p, data + end);
    }
    return changed;
}
