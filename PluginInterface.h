// ABI-compatible declaration of the TrafficMonitor plugin interface
// (https://github.com/zhongyang219/TrafficMonitor/blob/master/include/PluginInterface.h).
// Virtual functions MUST stay in exactly this order: the host calls them by vtable slot.
#pragma once

class ITrafficMonitor;
class IPluginDrawer;

class IPluginItem
{
public:
    virtual const wchar_t* GetItemName() const = 0;
    virtual const wchar_t* GetItemId() const = 0;
    virtual const wchar_t* GetItemLableText() const = 0;
    virtual const wchar_t* GetItemValueText() const = 0;
    virtual const wchar_t* GetItemValueSampleText() const = 0;
    virtual bool IsCustomDraw() const { return false; }
    virtual int GetItemWidth() const { return 0; }
    virtual void DrawItem(void* hDC, int x, int y, int w, int h, bool dark_mode) {}
    virtual int GetItemWidthEx(void* hDC) const { return 0; }                         // API v3

    enum MouseEventType { MT_LCLICKED, MT_RCLICKED, MT_DBCLICKED, MT_WHEEL_UP, MT_WHEEL_DOWN };
    enum MouseEventFlag { MF_TASKBAR_WND = 1 << 0 };
    virtual int OnMouseEvent(MouseEventType type, int x, int y, void* hWnd, int flag) { return 0; }   // API v3

    enum KeyboardEventFlag { KF_TASKBAR_WND = 1 << 0 };
    virtual int OnKeboardEvent(int key, bool ctrl, bool shift, bool alt, void* hWnd, int flag) { return 0; }  // API v4

    enum ItemInfoType {};
    virtual void* OnItemInfo(ItemInfoType type, void* para1, void* para2) { return 0; }  // API v4

    virtual int IsDrawResourceUsageGraph() const { return 0; }                        // API v6
    virtual float GetResourceUsageGraphValue() const { return 0.0f; }                 // API v6

    virtual bool DrawItemEx(IPluginDrawer* pDrawer, int x, int y, int w, int h, bool dark_mode) { return false; }  // API v8
    virtual int IsDoubleLineExclusive() const { return 0; }                           // API v8
};

class ITMPlugin
{
public:
    // 7: the host will not call the API v8 functions (DrawItemEx), so only DrawItem draws.
    virtual int GetAPIVersion() const { return 7; }
    virtual IPluginItem* GetItem(int index) = 0;
    virtual void DataRequired() = 0;

    enum OptionReturn { OR_OPTION_CHANGED, OR_OPTION_UNCHANGED, OR_OPTION_NOT_PROVIDED };
    virtual OptionReturn ShowOptionsDialog(void* hParent) { return OR_OPTION_NOT_PROVIDED; }

    enum PluginInfoIndex { TMI_NAME, TMI_DESCRIPTION, TMI_AUTHOR, TMI_COPYRIGHT, TMI_VERSION, TMI_URL, TMI_MAX };
    virtual const wchar_t* GetInfo(PluginInfoIndex index) = 0;

    struct MonitorInfo
    {
        unsigned long long up_speed{};
        unsigned long long down_speed{};
        int cpu_usage{};
        int memory_usage{};
        int gpu_usage{};
        int hdd_usage{};
        int cpu_temperature{};
        int gpu_temperature{};
        int hdd_temperature{};
        int main_board_temperature{};
        int cpu_freq{};
    };
    virtual void OnMonitorInfo(const MonitorInfo& monitor_info) {}

    virtual const wchar_t* GetTooltipInfo() { return L""; }                           // API v2

    enum ExtendedInfoIndex
    {
        EI_LABEL_TEXT_COLOR,
        EI_VALUE_TEXT_COLOR,
        EI_DRAW_TASKBAR_WND,
        EI_NAIN_WND_NET_SPEED_SHORT_MODE,
        EI_MAIN_WND_SPERATE_WITH_SPACE,
        EI_MAIN_WND_UNIT_BYTE,
        EI_MAIN_WND_UNIT_SELECT,
        EI_MAIN_WND_NOT_SHOW_UNIT,
        EI_MAIN_WND_NOT_SHOW_PERCENT,
        EI_TASKBAR_WND_NET_SPEED_SHORT_MODE,
        EI_TASKBAR_WND_SPERATE_WITH_SPACE,
        EI_TASKBAR_WND_VALUE_RIGHT_ALIGN,
        EI_TASKBAR_WND_NET_SPEED_WIDTH,
        EI_TASKBAR_WND_UNIT_BYTE,
        EI_TASKBAR_WND_UNIT_SELECT,
        EI_TASKBAR_WND_NOT_SHOW_UNIT,
        EI_TASKBAR_WND_NOT_SHOW_PERCENT,
        EI_CONFIG_DIR,
    };
    virtual void OnExtenedInfo(ExtendedInfoIndex index, const wchar_t* data) {}

    virtual void* GetPluginIcon() { return nullptr; }
    virtual int GetCommandCount() { return 0; }                                       // API v5
    virtual const wchar_t* GetCommandName(int command_index) { return nullptr; }      // API v5
    virtual void* GetCommandIcon(int command_index) { return nullptr; }               // API v5
    virtual void OnPluginCommand(int command_index, void* hWnd, void* para) {}        // API v5
    virtual int IsCommandChecked(int command_index) { return false; }                 // API v5
    virtual void OnInitialize(ITrafficMonitor* pApp) {}                               // API v7
};

extern "C" __declspec(dllexport) ITMPlugin* TMPluginGetInstance();
