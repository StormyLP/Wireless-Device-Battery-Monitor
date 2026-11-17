// Exercise the production driver with simulated replies; no hardware access.
#include "../src/drivers/drivers.h"

#include <cassert>
#include <cstdio>

namespace {
std::vector<std::vector<BYTE>> replies;
std::vector<BYTE> sentCommands;
int opens = 0;

HidColl Collection() {
    HidColl c;
    c.vid = 0x361D;
    c.pid = 0x0100;
    c.caps.UsagePage = 0xFF00;
    c.caps.Usage = 1;
    c.caps.InputReportByteLength = 64;
    c.caps.OutputReportByteLength = 64;
    return c;
}

// Input report 05: [2 + n][cmd][n][payload].
std::vector<BYTE> Reply(BYTE cmd, std::vector<BYTE> payload) {
    std::vector<BYTE> r(64, 0);
    r[0] = 0x05;
    r[1] = (BYTE)(2 + payload.size());
    r[2] = cmd;
    r[3] = (BYTE)payload.size();
    for (size_t i = 0; i < payload.size(); ++i) r[4 + i] = payload[i];
    return r;
}
std::vector<BYTE> Vbat(int mv) { return Reply(0x05, { (BYTE)(mv & 0xFF), (BYTE)(mv >> 8) }); }
std::vector<BYTE> Charging(bool on) { return Reply(0x25, { (BYTE)on }); }
std::vector<BYTE> Rssi() { return Reply(0x0D, { 0xD0 }); }  // pushed by the dongle, must be skipped

PollContext Poll(std::vector<std::vector<BYTE>> data) {
    replies = std::move(data);
    sentCommands.clear();
    PollContext ctx;
    ctx.colls = { Collection() };
    PollFinalmouse(ctx);
    return ctx;
}
}  // namespace

bool HidDev::Open(const HidColl& c) {
    c_ = c;
    h_ = reinterpret_cast<HANDLE>(1);
    ++opens;
    return true;
}
void HidDev::Close() { h_ = INVALID_HANDLE_VALUE; }
void HidDev::Flush() {}
bool HidDev::Write(const std::vector<BYTE>& data, int) {
    // Only payload-less read requests on output report 04 may be sent.
    assert(data.size() == 64 && data[0] == 0x04 && data[1] == 0x02 && data[3] == 0x00);
    assert(data[2] == (0x80 | 0x05) || data[2] == (0x80 | 0x25));
    for (size_t i = 4; i < data.size(); ++i) assert(data[i] == 0);
    sentCommands.push_back(data[2] & 0x7F);
    return true;
}
int HidDev::Read(std::vector<BYTE>& buf, int) {
    if (replies.empty()) return -1;  // timeout
    buf = replies.front();
    replies.erase(replies.begin());
    return (int)buf.size();
}
std::wstring Hex4(USHORT value) {
    wchar_t s[8];
    swprintf(s, 8, L"%04X", value);
    return s;
}

int main() {
    // The owner's capture: 4276 mV, not charging; XPANEL showed 88 %.
    auto ctx = Poll({ Rssi(), Vbat(4276), Rssi(), Charging(false) });
    assert(ctx.out.size() == 1 && ctx.IsClaimed(0x361D, 0x0100));
    const auto& s = ctx.out[0];
    assert(s.percent == 88 && !s.charging && !s.asleep);
    assert(s.name == L"Finalmouse UltralightX" && s.key == L"finalmouse:361D:0100");
    assert(s.kind == DeviceKind::Mouse && s.via == L"2.4 GHz receiver");
    assert((sentCommands == std::vector<BYTE>{ 0x05, 0x25 }));
    int before = opens;
    PollFinalmouse(ctx);  // already claimed
    assert(opens == before && ctx.out.size() == 1);

    // XPANEL's curve and its rounding.
    const int mv[] = { 4500, 4380, 4170, 4100, 3880, 3740, 3660, 3620, 3300, 3000, 2900 };
    const int pct[] = { 100, 100, 75, 69, 50, 25, 10, 5, 3, 0, 0 };  // 3300 mV: 0.2 + 4.8 * 300 / 620 = 2.52
    for (int i = 0; i < 11; ++i) {
        ctx = Poll({ Vbat(mv[i]), Charging(false) });
        assert(ctx.out.size() == 1 && ctx.out[0].percent == pct[i]);
    }

    ctx = Poll({ Vbat(4000), Charging(true) });
    assert(ctx.out.size() == 1 && ctx.out[0].charging);

    // A placeholder 0 first, then the real reading.
    ctx = Poll({ Vbat(0), Vbat(4276), Charging(false) });
    assert(ctx.out.size() == 1 && ctx.out[0].percent == 88);
    assert((sentCommands == std::vector<BYTE>{ 0x05, 0x05, 0x25 }));

    // No reading twice: the mouse is asleep; charging is not asked.
    ctx = Poll({ Vbat(0), Vbat(0xFFFF) });
    assert(ctx.out.size() == 1 && ctx.out[0].asleep);
    assert((sentCommands == std::vector<BYTE>{ 0x05, 0x05 }));

    // Silent dongle or only unrelated reports: nothing listed, nothing claimed.
    ctx = Poll({});
    assert(ctx.out.empty() && ctx.claimed.empty());
    ctx = Poll({ Rssi(), Rssi() });
    assert(ctx.out.empty() && ctx.claimed.empty());

    // Other devices and collections are never opened.
    ctx = {};
    auto otherVendor = Collection(), otherProduct = Collection(), otherPage = Collection();
    auto otherUsage = Collection(), shortOut = Collection();
    otherVendor.vid = 0x1234;
    otherProduct.pid = 0x0101;
    otherPage.caps.UsagePage = 1;
    otherUsage.caps.Usage = 2;
    shortOut.caps.OutputReportByteLength = 0;
    ctx.colls = { otherVendor, otherProduct, otherPage, otherUsage, shortOut };
    before = opens;
    PollFinalmouse(ctx);
    assert(opens == before && ctx.out.empty());
    puts("Finalmouse driver tests passed");
}
