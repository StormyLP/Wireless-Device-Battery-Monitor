// Finalmouse UltralightX on its 2.4 GHz dongle (361D:0100, "UltralightX dongle").
// Protocol from Finalmouse's web configurator XPANEL (xpanel.finalmouse.com, bundle
// _app/immutable/chunks/BhIB3vuQ.js: Ir() builds requests, Ht()/z() pick the report ids, the
// CMD_ID_VBAT / CMD_ID_BATTERY_CHARGING parsers read the replies), checked against a WebHID capture
// from an owner's dongle (XPANEL showed 88 %, VBAT replied 4276 mV).
// Vendor collection FF00:0001, 64-byte input and output reports:
//   request: output report 04 [02][80 | cmd][00]      (payload length 0 = read, nothing is changed)
//   reply:   input report 05  [2 + n][cmd][n][payload]
//   cmd 05 (VBAT):             2 bytes, battery voltage in mV, little endian; 0 / FFFF = no reading
//   cmd 25 (BATTERY_CHARGING): 1 byte, non-zero while charging
// The dongle also pushes unrelated replies (e.g. RSSI, cmd 0D) on the same collection; they are skipped.
// Percentages use XPANEL's own voltage curve (YNk7IF1T.js), so the tray matches the configurator.
// On its cable the mouse also shows up as 361D:0102 ("UltralightX mouse", FF00:0001). XPANEL treats that id as
// firmware-update only (ULX_MOUSE_USB, isFirmwareOnlyDevice) and keeps reading the battery through the dongle,
// which answers while the cable charges (checked on hardware: "100%, charging"). So 0102 is never opened here.
// Not handled yet: the Starlight X (report ids 01 / 02).
#include "drivers.h"

#include <chrono>

namespace {

const USHORT kVid = 0x361D;
const USHORT kDonglePid = 0x0100;
const BYTE kOutReport = 0x04;
const BYTE kInReport = 0x05;
const BYTE kCmdVbat = 0x05;
const BYTE kCmdCharging = 0x25;

// XPANEL's curve, highest voltage first; percentages in tenths (its lowest point is 0.2 %).
const int kMv[] = { 4380, 4170, 3880, 3740, 3660, 3620, 3000 };
const int kPct10[] = { 1000, 750, 500, 250, 100, 50, 2 };
const int kPoints = 7;

// Linear between the curve points, rounded like XPANEL's Math.round().
int PercentFromMv(int mv) {
    if (mv >= kMv[0]) return 100;
    for (int i = 1; i < kPoints; ++i) {
        if (mv < kMv[i]) continue;
        const int span = kMv[i - 1] - kMv[i];
        return (kPct10[i] * span + (kPct10[i - 1] - kPct10[i]) * (mv - kMv[i]) + 5 * span) / (10 * span);
    }
    return 0;
}

// Sends a read request for `cmd` and waits at most `timeoutMs` for its reply.
// Returns the reply payload, or an empty vector when the dongle did not answer.
std::vector<BYTE> Request(HidDev& d, BYTE cmd, int timeoutMs) {
    std::vector<BYTE> req(d.coll().outLen(), 0);
    req[0] = kOutReport;
    req[1] = 0x02;
    req[2] = 0x80 | cmd;
    req[3] = 0x00;
    if (!d.Write(req)) return {};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        const auto left =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) return {};
        std::vector<BYTE> r;
        if (d.Read(r, (int)left) < 0) return {};
        if (r.size() < 4 || r[0] != kInReport || r[2] != cmd) continue;  // pushed report for something else
        const size_t n = r[3];
        if (4 + n > r.size()) return {};
        return std::vector<BYTE>(r.begin() + 4, r.begin() + 4 + n);
    }
}

// Battery voltage in mV, 0 when the mouse gave no reading, -1 when the dongle did not answer.
int ReadMv(HidDev& d) {
    std::vector<BYTE> p = Request(d, kCmdVbat, 300);
    if (p.size() < 2) return -1;
    int mv = p[0] | p[1] << 8;
    return mv == 0xFFFF ? 0 : mv;
}

}  // namespace

void PollFinalmouse(PollContext& ctx) {
    if (ctx.IsClaimed(kVid, kDonglePid)) return;
    for (const HidColl* c : ctx.Find(kVid, kDonglePid, [](const HidColl& c) {
             return c.page() == 0xFF00 && c.usage() == 0x0001 && c.outLen() >= 4 && c.inLen() >= 8;
         })) {
        HidDev d;
        if (!d.Open(*c)) continue;
        d.Flush();
        int mv = ReadMv(d);
        if (mv == 0) {  // right after waking the dongle may answer with a placeholder 0 first
            Sleep(100);  // worst case for the whole poll: 300 + 100 + 300 + 250 ms, under the 1 s budget
            mv = ReadMv(d);
        }
        if (mv < 0) continue;
        DeviceStatus s;
        s.key = L"finalmouse:" + Hex4(kVid) + L":" + Hex4(kDonglePid);
        s.name = L"Finalmouse UltralightX";
        s.kind = DeviceKind::Mouse;
        s.via = L"2.4 GHz receiver";
        s.graceSec = 1800;  // a sleeping mouse leaves the dongle without a reading
        if (mv == 0) {
            s.asleep = true;  // shown with the last known value
        } else {
            s.percent = PercentFromMv(mv);
            std::vector<BYTE> chg = Request(d, kCmdCharging, 250);
            s.charging = !chg.empty() && chg[0] != 0;
        }
        ctx.Add(s, kVid, kDonglePid);
        break;
    }
}
