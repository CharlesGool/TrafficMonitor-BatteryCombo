// BatteryCombo: TrafficMonitor 插件，同时显示电池电量（数字画在电池图标内部）和电池实时功率。
#include <windows.h>
#include <setupapi.h>
#include <wchar.h>
#include <stdlib.h>
#include "PluginInterface.h"

namespace {

// ---------------------------------------------------------------------------
// 电池 IOCTL 定义（来自 batclass.h，这里自行声明以免依赖 DDK 头文件）
// ---------------------------------------------------------------------------
const GUID kGuidDeviceBattery = { 0x72631e54, 0x78a4, 0x11d0, { 0xbc, 0xf7, 0x00, 0xaa, 0x00, 0xb7, 0xb3, 0x2a } };

const DWORD kIoctlBatteryQueryTag = 0x294040;          // CTL_CODE(FILE_DEVICE_BATTERY, 0x10, METHOD_BUFFERED, FILE_READ_ACCESS)
const DWORD kIoctlBatteryQueryInformation = 0x294044;  // 0x11
const DWORD kIoctlBatteryQueryStatus = 0x29404C;       // 0x13

const ULONG kBatteryCapacityRelative = 0x40000000;
const LONG kBatteryUnknownRate = (LONG)0x80000000;
const ULONG kBatteryPowerOnLine = 0x1;
const ULONG kBatteryCharging = 0x4;
// 未公开的标志：对应 ACPI 6.4 _BST 的 "Battery Charge Limiting" 位。
// Windows 电池养护（任务栏电池图标上的爱心）生效时置位，实测 PowerState=0x11（已接通电源 + 充电受限）
const ULONG kBatteryChargeLimited = 0x10;

struct BatQueryInformation { ULONG BatteryTag; int InformationLevel; LONG AtRate; };
struct BatInformation
{
    ULONG Capabilities; UCHAR Technology; UCHAR Reserved[3]; UCHAR Chemistry[4];
    ULONG DesignedCapacity; ULONG FullChargedCapacity; ULONG DefaultAlert1; ULONG DefaultAlert2;
    ULONG CriticalBias; ULONG CycleCount;
};
struct BatWaitStatus { ULONG BatteryTag; ULONG Timeout; ULONG PowerState; ULONG LowCapacity; ULONG HighCapacity; };
struct BatStatus { ULONG PowerState; ULONG Capacity; ULONG Voltage; LONG Rate; };

// 读取所有电池的瞬时功率（mW，放电为负、充电为正）
class BatteryReader
{
public:
    ~BatteryReader() { Close(); }

    // power_state: 所有电池 PowerState 的按位或；返回 false 表示没有可用的功率数据
    bool Query(long& rate_mw, ULONG& power_state)
    {
        power_state = 0;
        DWORD now = GetTickCount();
        if (m_count == 0)
        {
            if (m_enumerated && now - m_last_enum < 10000)
                return false;
            Open();
            m_enumerated = true;
            m_last_enum = now;
            if (m_count == 0)
                return false;
        }

        long total = 0;
        bool any = false;
        bool failed = false;
        for (int i = 0; i < m_count; i++)
        {
            BatWaitStatus bws{};
            bws.BatteryTag = m_devs[i].tag;
            BatStatus bs{};
            DWORD out = 0;
            if (!DeviceIoControl(m_devs[i].handle, kIoctlBatteryQueryStatus, &bws, sizeof(bws), &bs, sizeof(bs), &out, nullptr))
            {
                failed = true;
                continue;
            }
            power_state |= bs.PowerState;
            if (m_devs[i].relative || bs.Rate == kBatteryUnknownRate)
                continue;
            total += bs.Rate;
            any = true;
        }
        if (failed)
        {
            // 电池被拔出或标签变化，下次重新枚举
            Close();
            m_enumerated = false;
        }
        rate_mw = total;
        return any;
    }

private:
    struct Device { HANDLE handle; ULONG tag; bool relative; };
    static const int kMaxBatteries = 4;

    void Open()
    {
        Close();
        HDEVINFO hdev = SetupDiGetClassDevsW(&kGuidDeviceBattery, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (hdev == INVALID_HANDLE_VALUE)
            return;
        for (DWORD i = 0; i < 16 && m_count < kMaxBatteries; i++)
        {
            SP_DEVICE_INTERFACE_DATA did{};
            did.cbSize = sizeof(did);
            if (!SetupDiEnumDeviceInterfaces(hdev, nullptr, &kGuidDeviceBattery, i, &did))
                break;
            DWORD need = 0;
            SetupDiGetDeviceInterfaceDetailW(hdev, &did, nullptr, 0, &need, nullptr);
            if (need == 0)
                continue;
            auto* detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)LocalAlloc(LPTR, need);
            if (detail == nullptr)
                continue;
            detail->cbSize = sizeof(*detail);
            if (SetupDiGetDeviceInterfaceDetailW(hdev, &did, detail, need, nullptr, nullptr))
            {
                HANDLE h = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h == INVALID_HANDLE_VALUE)
                    h = CreateFileW(detail->DevicePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h != INVALID_HANDLE_VALUE)
                {
                    Device dev{ h, 0, false };
                    if (QueryTag(dev))
                        m_devs[m_count++] = dev;
                    else
                        CloseHandle(h);
                }
            }
            LocalFree(detail);
        }
        SetupDiDestroyDeviceInfoList(hdev);
    }

    static bool QueryTag(Device& dev)
    {
        DWORD wait = 0;
        DWORD out = 0;
        ULONG tag = 0;
        if (!DeviceIoControl(dev.handle, kIoctlBatteryQueryTag, &wait, sizeof(wait), &tag, sizeof(tag), &out, nullptr) || tag == 0)
            return false;
        dev.tag = tag;
        BatQueryInformation bqi{};
        bqi.BatteryTag = tag;
        bqi.InformationLevel = 0;   // BatteryInformation
        BatInformation bi{};
        if (DeviceIoControl(dev.handle, kIoctlBatteryQueryInformation, &bqi, sizeof(bqi), &bi, sizeof(bi), &out, nullptr))
            dev.relative = (bi.Capabilities & kBatteryCapacityRelative) != 0;
        return true;
    }

    void Close()
    {
        for (int i = 0; i < m_count; i++)
            CloseHandle(m_devs[i].handle);
        m_count = 0;
    }

    Device m_devs[kMaxBatteries]{};
    int m_count{};
    bool m_enumerated{};
    DWORD m_last_enum{};
};

// ---------------------------------------------------------------------------
// Windows 电源模式（设置 → 系统 → 电源 → 电源模式）
// ---------------------------------------------------------------------------
enum class PowerMode { Efficiency, Balanced, Performance };

const GUID kSchemePowerSaver = { 0xa1841308, 0x3541, 0x4fab, { 0xbc, 0x81, 0xf7, 0x15, 0x56, 0xf2, 0x0b, 0x4a } };
const GUID kSchemeHighPerformance = { 0x8c5e7fda, 0xe8bf, 0x4a96, { 0x9a, 0x85, 0xa6, 0xe2, 0x3a, 0x8c, 0x63, 0x5c } };
const GUID kSchemeUltimatePerformance = { 0xe9a42b02, 0xd5df, 0x448d, { 0xaa, 0x00, 0x03, 0xf1, 0x47, 0x49, 0xeb, 0x61 } };
const GUID kOverlayBestEfficiency = { 0x961cc777, 0x2547, 0x4f9d, { 0x81, 0x74, 0x7d, 0x86, 0x18, 0x1b, 0x8a, 0x7a } };
const GUID kOverlayBestPerformance = { 0xded574b5, 0x45a0, 0x4f42, { 0x87, 0x37, 0x46, 0x34, 0x5c, 0x09, 0xc2, 0x38 } };

class PowerModeReader
{
public:
    PowerMode Query()
    {
        if (m_module == nullptr)
        {
            m_module = LoadLibraryW(L"powrprof.dll");
            if (m_module == nullptr)
                return PowerMode::Balanced;
            m_get_active = (GetActiveSchemeFn)(void*)GetProcAddress(m_module, "PowerGetActiveScheme");
            m_get_overlay = (GetOverlayFn)(void*)GetProcAddress(m_module, "PowerGetEffectiveOverlayScheme");
        }

        // 传统电源计划优先：节能 / 高性能 / 卓越性能
        GUID* active = nullptr;
        if (m_get_active != nullptr && m_get_active(nullptr, &active) == ERROR_SUCCESS && active != nullptr)
        {
            GUID scheme = *active;
            LocalFree(active);
            if (IsEqualGUID(scheme, kSchemePowerSaver))
                return PowerMode::Efficiency;
            if (IsEqualGUID(scheme, kSchemeHighPerformance) || IsEqualGUID(scheme, kSchemeUltimatePerformance))
                return PowerMode::Performance;
        }

        // 平衡计划下由电源模式滑块（覆盖方案）决定
        GUID overlay{};
        if (m_get_overlay != nullptr && m_get_overlay(&overlay) == ERROR_SUCCESS)
        {
            if (IsEqualGUID(overlay, kOverlayBestEfficiency))
                return PowerMode::Efficiency;
            if (IsEqualGUID(overlay, kOverlayBestPerformance))
                return PowerMode::Performance;
        }
        return PowerMode::Balanced;
    }

private:
    typedef DWORD (WINAPI* GetActiveSchemeFn)(HKEY, GUID**);
    typedef DWORD (WINAPI* GetOverlayFn)(GUID*);
    HMODULE m_module{};
    GetActiveSchemeFn m_get_active{};
    GetOverlayFn m_get_overlay{};
};

// ---------------------------------------------------------------------------
// 共享状态
// ---------------------------------------------------------------------------
struct BatteryState
{
    int percent = -1;           // -1: 未知 / 无电池
    bool has_rate = false;
    long rate_mw = 0;
    bool charging = false;
    bool ac_online = false;
    bool care = false;          // Windows 电池养护（充电受限）
    bool saver = false;         // 节能模式
    PowerMode mode = PowerMode::Balanced;
    DWORD life_time = (DWORD)-1;

    bool text_color_set = false;
    COLORREF text_color = 0;
    bool value_right_align = false;     // TrafficMonitor 任务栏选项「数值右对齐」

    // 部分绘制路径（Direct2D 任务栏）传进来的 DC 没有选入任务栏字体，用测量宽度时记下的字体代替
    bool font_cached = false;
    LOGFONTW font{};
};
BatteryState g_state;

const int kLowPercent = 20;

// 按电源模式着色：最佳能效黄色 / 平衡蓝色 / 最佳性能红色（Windows 11 深色 / 浅色配色）
COLORREF ModeColor(bool dark)
{
    switch (g_state.mode)
    {
    case PowerMode::Efficiency: return dark ? RGB(0xFC, 0xE1, 0x00) : RGB(0x9D, 0x5D, 0x00);
    case PowerMode::Performance: return dark ? RGB(0xFF, 0x99, 0xA4) : RGB(0xC4, 0x2B, 0x1C);
    default: return dark ? RGB(0x60, 0xCD, 0xFF) : RGB(0x00, 0x5F, 0xB8);
    }
}

COLORREF LowColor(bool dark)
{
    return dark ? RGB(0xFF, 0x99, 0xA4) : RGB(0xC4, 0x2B, 0x1C);
}

const wchar_t* ModeName()
{
    switch (g_state.mode)
    {
    case PowerMode::Efficiency: return L"最佳能效";
    case PowerMode::Performance: return L"最佳性能";
    default: return L"平衡";
    }
}

COLORREF ContrastColor(COLORREF c)
{
    int lum = (GetRValue(c) * 299 + GetGValue(c) * 587 + GetBValue(c) * 114) / 1000;
    return lum > 140 ? RGB(0, 0, 0) : RGB(255, 255, 255);
}

void FormatPower(wchar_t* buf, size_t size)
{
    if (!g_state.has_rate)
    {
        wcscpy_s(buf, size, L"--W");
        return;
    }
    // 充电为 +，放电为 -；功率为 0 时按是否接通电源决定（接通电源视为充电侧，显示 +0W）
    long watts = (labs(g_state.rate_mw) + 500) / 1000;
    const wchar_t* sign = g_state.rate_mw > 0 ? L"+" : (g_state.rate_mw < 0 ? L"-" : (g_state.ac_online ? L"+" : L""));
    swprintf_s(buf, size, L"%s%ldW", sign, watts);
}

void FormatPercent(wchar_t* buf, size_t size)
{
    if (g_state.percent < 0)
        wcscpy_s(buf, size, L"--");
    else
        swprintf_s(buf, size, L"%d", g_state.percent);
}

// ---------------------------------------------------------------------------
// 电池图标直接使用 Windows 任务栏同款字形（Segoe Fluent Icons / Segoe MDL2 Assets 的 MobBattery 系列）。
// 字形设计在 16×16 网格上（字号 E 像素，1 个网格单位 = E/16 像素，见 Grid）：
//   外壳 x 0~14、y 3~13，描边 1；正极 x 14~16；填充区 x 2~12、y 5~11（与描边间隔 1）；闪电伸到 y 0
// 为了在电池里放下电量数字，把字形在 x=8 处切开、中段横向拉长，两端（圆角、正极、闪电）保持系统渲染原样。
// ---------------------------------------------------------------------------
const wchar_t kGlyphBattery[] = L"\xEBA0";          // MobBattery0：空电池外壳
const wchar_t kGlyphBatteryFull[] = L"\xEBAA";      // MobBattery10：满电（外壳 + 填充）
const wchar_t kGlyphCharging[] = L"\xEBAB";         // MobBatteryCharging0：带闪电的外壳
const wchar_t kGlyphChargingFull[] = L"\xEBB5";     // MobBatteryCharging10：带闪电的满电
const wchar_t kGlyphHeart[] = L"\xEB52";            // HeartFill：电池养护角标
const wchar_t kGlyphCircle[] = L"\xF136";           // StatusCircleOuter：低电量角标底圆
const wchar_t kGlyphExclamation[] = L"\xF13B";      // 与底圆叠加的感叹号

// 网格单位换算：字号 E 像素时 n/2 个网格单位对应的像素（四舍五入）
struct Grid
{
    int em;
    int Half(int halves) const { return (halves * em + 16) / 32; }
    int operator()(int units) const { return Half(units * 2); }
};

// 数字 "0" 的实际墨迹：高度，以及墨迹顶部到基线的距离（不含行距，用于按墨迹而不是行高居中）
struct DigitInk { int height; int ascent; };

DigitInk MeasureDigitInk(HDC dc)
{
    GLYPHMETRICS gm{};
    MAT2 identity{ { 0, 1 }, { 0, 0 }, { 0, 0 }, { 0, 1 } };
    if (GetGlyphOutlineW(dc, L'0', GGO_METRICS, &gm, 0, nullptr, &identity) != GDI_ERROR && gm.gmBlackBoxY > 0)
        return { (int)gm.gmBlackBoxY, (int)gm.gmptGlyphOrigin.y };
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    int height = (tm.tmAscent - tm.tmInternalLeading) * 3 / 4;
    return { height, height };
}

// 以墨迹中心 (cx, cy) 为准绘制数字
void DrawDigits(HDC dc, const wchar_t* text, int cx, int cy, const DigitInk& ink, COLORREF color)
{
    int state = SaveDC(dc);
    SetTextColor(dc, color);
    SetTextAlign(dc, TA_BASELINE | TA_CENTER | TA_NOUPDATECP);
    TextOutW(dc, cx, cy + (2 * ink.ascent - ink.height) / 2, text, (int)wcslen(text));
    RestoreDC(dc, state);
}

// 字形墨迹框居中放进 box 后的实际范围（当前 DC 已选入图标字体）；用来让镂空紧贴图形而不是整个字号方框
RECT GlyphInkRect(HDC dc, const wchar_t* glyph, const RECT& box)
{
    GLYPHMETRICS gm{};
    MAT2 identity{ { 0, 1 }, { 0, 0 }, { 0, 0 }, { 0, 1 } };
    if (GetGlyphOutlineW(dc, glyph[0], GGO_METRICS, &gm, 0, nullptr, &identity) == GDI_ERROR)
        return box;
    RECT rc;
    rc.left = box.left + ((box.right - box.left) - (int)gm.gmBlackBoxX) / 2;
    rc.top = box.top + ((box.bottom - box.top) - (int)gm.gmBlackBoxY) / 2;
    rc.right = rc.left + (int)gm.gmBlackBoxX;
    rc.bottom = rc.top + (int)gm.gmBlackBoxY;
    return rc;
}

// 把图标字形的墨迹框居中放进 box（当前 DC 已选入图标字体）
void DrawGlyphInBox(HDC dc, const wchar_t* glyph, const RECT& box, COLORREF color)
{
    GLYPHMETRICS gm{};
    MAT2 identity{ { 0, 1 }, { 0, 0 }, { 0, 0 }, { 0, 1 } };
    if (GetGlyphOutlineW(dc, glyph[0], GGO_METRICS, &gm, 0, nullptr, &identity) == GDI_ERROR)
        return;
    int state = SaveDC(dc);
    SetTextColor(dc, color);
    SetTextAlign(dc, TA_BASELINE | TA_LEFT | TA_NOUPDATECP);
    int gx = box.left + ((box.right - box.left) - (int)gm.gmBlackBoxX) / 2 - gm.gmptGlyphOrigin.x;
    int gy = box.top + ((box.bottom - box.top) - (int)gm.gmBlackBoxY) / 2 + gm.gmptGlyphOrigin.y;
    TextOutW(dc, gx, gy, glyph, 1);
    RestoreDC(dc, state);
}

struct Layout
{
    Grid g;         // 电池字形字号与网格换算
    int extra;      // 电池中段拉长的像素
    int ink;        // 数字墨迹高度
    int power_w, gap, heart, low, knock;
    int LowOut() const { return 0; }                // 低电量角标不探出外壳左侧，电池左边缘和其他行的标签对齐
    int BatteryW() const { return g(16) + extra; }
    int Width() const { return power_w + gap + LowOut() + BatteryW(); }
};

// max_em > 0 时限制字号：整个 16 网格的字形框（含上方闪电、下方爱心）都要放进项目高度
Layout Measure(HDC dc, int max_em = 0)
{
    SIZE d100{}, d88{}, power{};
    GetTextExtentPoint32W(dc, L"100", 3, &d100);
    GetTextExtentPoint32W(dc, L"88", 2, &d88);
    GetTextExtentPoint32W(dc, L"+99W", 4, &power);

    Layout l{};
    l.ink = MeasureDigitInk(dc).height;
    // 字号取数字墨迹高的 3 倍、向下取 8 的倍数：图标和文字行高相当（字形框 16 单位里外壳只占 10 单位）。
    // 数字绘制时再缩到 4 单位高（填充区 6 单位，上下各留 1 单位）
    int em = l.ink * 3;
    em -= em % 8;
    if (max_em > 0 && em > max_em)
    {
        // 优先取 16 的倍数（每条边都落在整像素上，和系统图标一样清晰），否则取 8 的倍数（边落在半像素上）
        em = max_em - max_em % 8;
        if (max_em % 16 != 0 && max_em - max_em % 16 >= max_em * 9 / 10)
            em = max_em - max_em % 16;
    }
    if (em < 16)
        em = 16;
    l.g.em = em;
    const Grid& g = l.g;
    l.power_w = power.cx;
    l.gap = g.Half(3);
    l.heart = g(7);                 // 与 Windows 电池养护图标一致，盖住外壳右下角
    l.low = g(6);
    l.knock = g(1) < 1 ? 1 : g(1);
    // 填充区（10 单位 + extra）要能放下 "100"，或放下两位数再让出左侧闪电（3 单位）和右下爱心（约 4.5 单位）。
    // 数字绘制时会缩到 4 单位高，宽度按同样比例缩放后再算，电池只拉长到刚好够用
    int w100 = d100.cx, w88 = d88.cx;
    if (l.ink > g(4))
    {
        w100 = w100 * g(4) / l.ink;
        w88 = w88 * g(4) / l.ink;
    }
    int need = w100 + g(1);
    if (w88 + g.Half(17) > need)
        need = w88 + g.Half(17);
    l.extra = need > g(10) ? need - g(10) : 0;
    return l;
}

// 选入可用字体，返回需要在绘制后删除的临时字体
HFONT EnsureFont(HDC dc)
{
    if (GetCurrentObject(dc, OBJ_FONT) == GetStockObject(SYSTEM_FONT) && g_state.font_cached)
    {
        HFONT font = CreateFontIndirectW(&g_state.font);
        if (font != nullptr)
            SelectObject(dc, font);
        return font;
    }
    return nullptr;
}

// 创建图标字体；系统中没有时返回 nullptr
HFONT CreateIconFont(HDC dc, int px)
{
    const wchar_t* faces[] = { L"Segoe Fluent Icons", L"Segoe MDL2 Assets" };
    for (const wchar_t* face : faces)
    {
        HFONT font = CreateFontW(-px, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, face);
        if (font == nullptr)
            continue;
        HGDIOBJ old = SelectObject(dc, font);
        wchar_t actual[LF_FACESIZE]{};
        GetTextFaceW(dc, LF_FACESIZE, actual);
        SelectObject(dc, old);
        if (_wcsicmp(actual, face) == 0)
            return font;
        DeleteObject(font);
    }
    return nullptr;
}

void DrawTextIn(HDC dc, const wchar_t* text, RECT rc, COLORREF color, UINT align)
{
    SetTextColor(dc, color);
    DrawTextW(dc, text, -1, &rc, align | DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP);
}

// 把字形在 cut 列切开，右半部分右移 extra 像素，中间逐列复制 cut 列补齐；extra 为负时两半重叠裁剪。
// (gx, gy) 为 TA_BASELINE 下的绘制原点
void DrawGlyphStretched(HDC dc, const wchar_t* glyph, int gx, int gy, int cut, int extra, COLORREF color)
{
    int state = SaveDC(dc);
    SetTextColor(dc, color);
    SetTextAlign(dc, TA_BASELINE | TA_LEFT | TA_NOUPDATECP);
    auto draw = [&](int dx, int left, int right) {
        int s = SaveDC(dc);
        IntersectClipRect(dc, left, gy - 4096, right, gy + 4096);
        TextOutW(dc, gx + dx, gy, glyph, 1);
        RestoreDC(dc, s);
    };
    draw(0, gx - 4096, cut + (extra < 0 ? extra : 0));
    draw(extra, cut + extra, gx + 4096);
    for (int d = 0; d < extra; d++)
        draw(d, cut + d, cut + d + 1);
    RestoreDC(dc, state);
}

// 把设备坐标下的区域与当前裁剪区求交（DC 原本没有裁剪区时直接设为该区域）
void IntersectClip(HDC dc, HRGN dev_rgn)
{
    HRGN clip = CreateRectRgn(0, 0, 0, 0);
    CombineRgn(clip, dev_rgn, nullptr, RGN_COPY);
    HRGN existing = CreateRectRgn(0, 0, 0, 0);
    if (GetClipRgn(dc, existing) == 1)
        CombineRgn(clip, clip, existing, RGN_AND);
    SelectClipRgn(dc, clip);
    DeleteObject(existing);
    DeleteObject(clip);
}

// 把角标周围的椭圆区域从裁剪区中挖掉，使外壳和填充在角标处留出缝隙（与背景无关的镂空效果）
void KnockOut(HDC dc, const RECT* boxes, int count, int knock, const RECT& item)
{
    RECT dev_item = item;
    LPtoDP(dc, (POINT*)&dev_item, 2);
    HRGN clip = CreateRectRgnIndirect(&dev_item);
    for (int i = 0; i < count; i++)
    {
        RECT rc = boxes[i];
        InflateRect(&rc, knock, knock);
        LPtoDP(dc, (POINT*)&rc, 2);
        HRGN hole = CreateEllipticRgn(rc.left, rc.top, rc.right + 1, rc.bottom + 1);
        CombineRgn(clip, clip, hole, RGN_DIFF);
        DeleteObject(hole);
    }
    IntersectClip(dc, clip);
    DeleteObject(clip);
}

// ---------------------------------------------------------------------------
// 显示项目：[功率] [电池图标(电量) + 角标]
// ---------------------------------------------------------------------------
class BatteryItem : public IPluginItem
{
public:
    const wchar_t* GetItemName() const override { return L"电池电量与功耗"; }
    const wchar_t* GetItemId() const override { return L"BatteryComboQ7mZ"; }
    const wchar_t* GetItemLableText() const override { return L""; }
    const wchar_t* GetItemValueText() const override { return m_value_text; }
    const wchar_t* GetItemValueSampleText() const override { return L"+99W 100%"; }
    bool IsCustomDraw() const override { return true; }
    int GetItemWidth() const override { return 90; }

    int GetItemWidthEx(void* hDC) const override
    {
        HDC dc = (HDC)hDC;
        HGDIOBJ font = GetCurrentObject(dc, OBJ_FONT);
        if (font != nullptr && font != GetStockObject(SYSTEM_FONT) && GetObjectW(font, sizeof(LOGFONTW), &g_state.font) == sizeof(LOGFONTW))
            g_state.font_cached = true;
        int saved = SaveDC(dc);
        HFONT tmp = EnsureFont(dc);
        // 宽度只取决于字体，保持稳定：宿主按首次取到的宽度确定窗口大小，之后不会缩小窗口
        int width = Measure(dc).Width();
        RestoreDC(dc, saved);
        if (tmp != nullptr)
            DeleteObject(tmp);
        return width;
    }

    void DrawItem(void* hDC, int x, int y, int w, int h, bool dark_mode) override
    {
        HDC dc = (HDC)hDC;
        int saved = SaveDC(dc);
        HFONT tmp_font = EnsureFont(dc);
        HFONT digits_font = nullptr;
        HFONT icon_font = nullptr;
        SetBkMode(dc, TRANSPARENT);

        COLORREF fg = g_state.text_color_set ? g_state.text_color : (dark_mode ? RGB(255, 255, 255) : RGB(0, 0, 0));
        Layout l = Measure(dc, h);
        const Grid& g = l.g;

        // 和 TrafficMonitor 的「标签 + 数值」布局一致：电池像标签一样固定在左侧，功率像数值一样跟在后面，
        // 开启「数值右对齐」时功率贴右边缘
        int right = x + (w > l.Width() ? w : l.Width());
        int L = x + l.LowOut();                     // 外壳左边（左侧留出低电量角标探出的位置）
        int top = y + (h - g.em) / 2;               // 16 网格字形框的上边（闪电顶端）
        int T = top + g(3);                         // 外壳上边
        int body_r = L + g(14) + l.extra;           // 外壳右边（不含正极）

        wchar_t power_text[16];
        FormatPower(power_text, 16);
        DrawTextIn(dc, power_text, RECT{ L + l.BatteryW() + l.gap, y, right, y + h }, fg,
                   (g_state.value_right_align ? DT_RIGHT : DT_LEFT) | DT_VCENTER);

        HFONT text_font = (HFONT)GetCurrentObject(dc, OBJ_FONT);
        icon_font = CreateIconFont(dc, g.em);
        if (icon_font != nullptr)
        {
            // 字形定位：让 MobBattery0 的墨迹框左上角落在 (L, T)
            SelectObject(dc, icon_font);
            GLYPHMETRICS gm{};
            MAT2 identity{ { 0, 1 }, { 0, 0 }, { 0, 0 }, { 0, 1 } };
            GetGlyphOutlineW(dc, kGlyphBattery[0], GGO_METRICS, &gm, 0, nullptr, &identity);
            int gx = L - gm.gmptGlyphOrigin.x;
            int gy = T + gm.gmptGlyphOrigin.y;
            int cut = L + g(8);

            // 角标：接通电源时用带闪电的字形（与 Windows 一致，充电受限时也显示）；低电量 → 左下；电池养护 → 右下爱心
            bool plugged = g_state.charging || g_state.ac_online;
            bool show_low = !plugged && g_state.percent >= 0 && g_state.percent <= kLowPercent;
            bool show_heart = g_state.care;
            int bottom = top + g.em;                    // 16 网格字形框的下边
            RECT heart_box{ body_r + g.Half(3) - l.heart, bottom - l.heart, body_r + g.Half(3), bottom };
            RECT low_box{ L - l.LowOut(), bottom - g(1) - l.low, L - l.LowOut() + l.low, bottom - g(1) };

            // 角标字体：低电量圆按墨迹缩放到正好填满 low_box；镂空和避让都按字形墨迹的实际范围计算
            HFONT heart_font = show_heart ? CreateIconFont(dc, l.heart) : nullptr;
            HFONT low_font = nullptr;
            RECT heart_ink = heart_box;
            RECT low_ink = low_box;
            if (heart_font != nullptr)
            {
                SelectObject(dc, heart_font);
                heart_ink = GlyphInkRect(dc, kGlyphHeart, heart_box);
            }
            if (show_low)
            {
                low_font = CreateIconFont(dc, l.low);
                if (low_font != nullptr)
                {
                    SelectObject(dc, low_font);
                    RECT ink_rc = GlyphInkRect(dc, kGlyphCircle, low_box);
                    int ink_w = ink_rc.right - ink_rc.left;
                    if (ink_w > 0 && ink_w < l.low)
                    {
                        SelectObject(dc, text_font);
                        DeleteObject(low_font);
                        low_font = CreateIconFont(dc, l.low * l.low / ink_w);
                        if (low_font != nullptr)
                        {
                            SelectObject(dc, low_font);
                            ink_rc = GlyphInkRect(dc, kGlyphCircle, low_box);
                        }
                    }
                    low_ink = ink_rc;
                }
            }
            SelectObject(dc, icon_font);

            RECT holes[2];
            int hole_count = 0;
            if (show_low)
                holes[hole_count++] = low_ink;
            if (show_heart)
                holes[hole_count++] = heart_ink;
            int knock_state = SaveDC(dc);
            if (hole_count > 0)
                KnockOut(dc, holes, hole_count, l.knock, RECT{ x, y, right, y + h });

            // 填充：用满电字形，中段按电量拉长 / 缩短，右端圆角落在电量位置；裁剪到外壳内侧
            int percent = g_state.percent < 0 ? 0 : (g_state.percent > 100 ? 100 : g_state.percent);
            int fill_len = ((g(10) + l.extra) * percent + 50) / 100;
            COLORREF fill_color = ModeColor(dark_mode);
            RECT fill{ L + g(2), T + g(2), L + g(2) + fill_len, T + g(8) };
            if (fill_len > 0)
            {
                int fill_state = SaveDC(dc);
                // 右边界正好卡在填充末端：字形里还带着外壳描边，网格单位非整数时描边的抗锯齿边缘会漏进来
                IntersectClipRect(dc, L + g.Half(3), T + g.Half(3), fill.right, T + g.Half(17));
                DrawGlyphStretched(dc, plugged ? kGlyphChargingFull : kGlyphBatteryFull, gx, gy, cut, fill_len - g(10), fill_color);
                RestoreDC(dc, fill_state);
            }

            // 电量数字：墨迹高度不超过 4 单位（填充区 6 单位，上下各留 1 单位），按墨迹居中并避开角标；落在填充上的部分用反差色
            SelectObject(dc, text_font);
            int max_ink = g(4);
            if (l.ink > max_ink)
            {
                LOGFONTW lf{};
                GetObjectW(text_font, sizeof(lf), &lf);
                lf.lfHeight = lf.lfHeight * max_ink / l.ink;
                lf.lfWidth = 0;
                digits_font = CreateFontIndirectW(&lf);
                if (digits_font != nullptr)
                    SelectObject(dc, digits_font);
            }
            DigitInk ink = MeasureDigitInk(dc);
            wchar_t percent_text[8];
            FormatPercent(percent_text, 8);
            int digits_left = L + g(2);
            int digits_right = L + g(12) + l.extra;
            if (plugged)
                digits_left = L + g(5);
            if (show_low && low_ink.right + l.knock > digits_left)
                digits_left = low_ink.right + l.knock;
            if (show_heart && heart_ink.left - l.knock < digits_right)
                digits_right = heart_ink.left - l.knock;
            int digits_cx = (digits_left + digits_right) / 2;
            int digits_cy = T + g(5);
            int clip_state = SaveDC(dc);
            ExcludeClipRect(dc, fill.left, fill.top, fill.right, fill.bottom);
            DrawDigits(dc, percent_text, digits_cx, digits_cy, ink, fg);
            RestoreDC(dc, clip_state);
            if (fill_len > 0)
            {
                clip_state = SaveDC(dc);
                IntersectClipRect(dc, fill.left, fill.top, fill.right, fill.bottom);
                DrawDigits(dc, percent_text, digits_cx, digits_cy, ink, ContrastColor(fill_color));
                RestoreDC(dc, clip_state);
            }

            // 外壳（接通电源时带闪电）
            SelectObject(dc, icon_font);
            DrawGlyphStretched(dc, plugged ? kGlyphCharging : kGlyphBattery, gx, gy, cut, l.extra, fg);
            RestoreDC(dc, knock_state);

            // 低电量：图标字体的实心圆 + 感叹号（与系统状态角标同款，带抗锯齿）
            if (low_font != nullptr)
            {
                SelectObject(dc, low_font);
                COLORREF low_color = LowColor(dark_mode);
                DrawGlyphInBox(dc, kGlyphCircle, low_box, low_color);
                DrawGlyphInBox(dc, kGlyphExclamation, low_box, ContrastColor(low_color));
            }
            if (heart_font != nullptr)
            {
                SelectObject(dc, heart_font);
                DrawGlyphInBox(dc, kGlyphHeart, heart_box, fg);
            }
            SelectObject(dc, text_font);
            if (low_font != nullptr)
                DeleteObject(low_font);
            if (heart_font != nullptr)
                DeleteObject(heart_font);
        }

        RestoreDC(dc, saved);
        if (icon_font != nullptr)
            DeleteObject(icon_font);
        if (digits_font != nullptr)
            DeleteObject(digits_font);
        if (tmp_font != nullptr)
            DeleteObject(tmp_font);
    }

    void Update()
    {
        wchar_t percent_text[8];
        wchar_t power_text[16];
        FormatPercent(percent_text, 8);
        FormatPower(power_text, 16);
        swprintf_s(m_value_text, L"%s %s%%", power_text, percent_text);
    }

private:
    wchar_t m_value_text[32]{ L"--W --%" };
};

// ---------------------------------------------------------------------------
// 插件
// ---------------------------------------------------------------------------
class BatteryComboPlugin : public ITMPlugin
{
public:
    IPluginItem* GetItem(int index) override { return index == 0 ? &m_item : nullptr; }

    void DataRequired() override
    {
        SYSTEM_POWER_STATUS sps{};
        bool sps_ok = GetSystemPowerStatus(&sps) != FALSE;
        bool no_battery = sps_ok && (sps.BatteryFlag & 128);
        g_state.percent = (sps_ok && !no_battery && sps.BatteryLifePercent <= 100) ? sps.BatteryLifePercent : -1;
        g_state.life_time = sps_ok ? sps.BatteryLifeTime : (DWORD)-1;
        g_state.saver = sps_ok && sps.SystemStatusFlag == 1;

        long rate = 0;
        ULONG power_state = 0;
        g_state.has_rate = m_reader.Query(rate, power_state);
        g_state.rate_mw = g_state.has_rate ? rate : 0;
        g_state.charging = (power_state & kBatteryCharging) || (g_state.has_rate && rate > 0) || (sps_ok && (sps.BatteryFlag & 8));
        g_state.ac_online = sps_ok ? sps.ACLineStatus == 1 : (power_state & kBatteryPowerOnLine) != 0;
        // 拔掉电源后充电受限标志可能仍保持置位；Windows 只在接通电源时显示电池养护爱心
        g_state.care = (power_state & kBatteryChargeLimited) != 0 && g_state.ac_online;
        g_state.mode = m_mode_reader.Query();

        ApplyTestOverride();
        m_item.Update();
        UpdateTooltip();
    }

    const wchar_t* GetInfo(PluginInfoIndex index) override
    {
        switch (index)
        {
        case TMI_NAME: return L"电池电量与功耗";
        case TMI_DESCRIPTION: return L"在一个项目中同时显示电池电量（数字显示在 Windows 风格的电池图标内）和电池实时功率。"
                                     L"电量按 Windows 电源模式着色，并用角标标示接通电源、低电量和电池养护。";
        case TMI_AUTHOR: return L"CharlesGool";
        case TMI_COPYRIGHT: return L"Copyright (C) 2026 CharlesGool";
        case TMI_VERSION: return L"1.0";
        case TMI_URL: return L"https://github.com/CharlesGool/TrafficMonitor-BatteryCombo";
        default: return L"";
        }
    }

    const wchar_t* GetTooltipInfo() override { return m_tooltip; }

    void OnExtenedInfo(ExtendedInfoIndex index, const wchar_t* data) override
    {
        if (data == nullptr || data[0] == L'\0')
            return;
        if (index == EI_VALUE_TEXT_COLOR)
        {
            g_state.text_color = (COLORREF)wcstoul(data, nullptr, 10);
            g_state.text_color_set = true;
        }
        else if (index == EI_TASKBAR_WND_VALUE_RIGHT_ALIGN)
        {
            g_state.value_right_align = wcstoul(data, nullptr, 10) != 0;
        }
    }

private:
    void UpdateTooltip()
    {
        wchar_t percent_text[8];
        wchar_t power_text[16];
        FormatPercent(percent_text, 8);
        FormatPower(power_text, 16);
        const wchar_t* status = g_state.charging ? L"充电中"
                              : g_state.care ? L"已接通电源，电池养护中（充电受限）"
                              : g_state.ac_online ? L"已接通电源，未充电"
                              : L"使用电池";
        int len = swprintf_s(m_tooltip, L"电池电量: %s%%\r\n状态: %s\r\n功率: %s\r\n电源模式: %s",
                             percent_text, status, power_text, ModeName());
        if (len > 0 && g_state.saver)
            len += swprintf_s(m_tooltip + len, kTooltipSize - len, L"\r\n节能模式: 已开启");
        if (len > 0 && !g_state.ac_online && g_state.life_time != (DWORD)-1)
            swprintf_s(m_tooltip + len, kTooltipSize - len, L"\r\n剩余时间: %lu 小时 %lu 分钟",
                       g_state.life_time / 3600, g_state.life_time % 3600 / 60);
    }

#ifdef BATTERY_COMBO_TEST
public:
    bool m_test_override = false;
    BatteryState m_test_state;
private:
    void ApplyTestOverride()
    {
        if (!m_test_override)
            return;
        g_state.percent = m_test_state.percent;
        g_state.has_rate = m_test_state.has_rate;
        g_state.rate_mw = m_test_state.rate_mw;
        g_state.charging = m_test_state.charging;
        g_state.ac_online = m_test_state.ac_online;
        g_state.care = m_test_state.care;
        g_state.mode = m_test_state.mode;
    }
#else
    void ApplyTestOverride() {}
#endif

    static const int kTooltipSize = 192;
    BatteryItem m_item;
    BatteryReader m_reader;
    PowerModeReader m_mode_reader;
    wchar_t m_tooltip[kTooltipSize]{};
};

BatteryComboPlugin g_plugin;

} // namespace

ITMPlugin* TMPluginGetInstance()
{
    return &g_plugin;
}

#ifdef BATTERY_COMBO_TEST
// 仅测试构建导出：模拟各种电池状态以便渲染截图。mode: 0 最佳能效 / 1 平衡 / 2 最佳性能
extern "C" __declspec(dllexport) void TestSetState(int percent, long rate_mw, int charging, int ac, int care, int mode)
{
    g_plugin.m_test_override = true;
    g_plugin.m_test_state.percent = percent;
    g_plugin.m_test_state.has_rate = true;
    g_plugin.m_test_state.rate_mw = rate_mw;
    g_plugin.m_test_state.charging = charging != 0;
    g_plugin.m_test_state.ac_online = ac != 0;
    g_plugin.m_test_state.care = care != 0;
    g_plugin.m_test_state.mode = (PowerMode)mode;
    g_plugin.DataRequired();
}
#endif
