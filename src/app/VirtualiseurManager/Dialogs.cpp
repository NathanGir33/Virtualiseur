#include "Dialogs.h"

#include "Ui.h"

#include "DiskSnapshots.h"

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <atomic>
#include <thread>
#include <array>
#include <cmath>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <utility>

namespace manager {
namespace {

using ui::Scale;
namespace color = ui::color;

enum ControlId : int {
    IdCategories = 100,
    IdSnapshotList,
    IdSnapshotDetail,
    IdSnapshotName,
    IdSnapshotRestore,
    IdSnapshotDelete,
    IdSnapshotClone,
    IdCloneName,
    IdCloneKind,
    IdCloneCancel,
    IdPageTitle,
    IdMessage,
    IdName,
    IdOs,
    IdFolderPath,
    IdOpenFolder,
    IdMemory,
    IdCpus,
    IdBoot,
    IdRtc,
    IdGpu,
    IdResolution,
    Id3d,
    Id3dStatus,
    IdDisk,
    IdBrowseDisk,
    IdDiskSize,
    IdDiskInfo,
    IdDiskBus,
    IdIso,
    IdBrowseIso,
    IdEject,
    IdIsoInfo,
    IdAudio,
    IdMicrophone,
    IdNetwork,
    IdNetMode,
    IdInstaller,
    IdPreseed,
    IdBrowsePreseed,
    IdGuestTools,
    IdShareTag,
    IdShareTagLast = IdShareTag + kMaxShares - 1,
    IdSharePath,
    IdSharePathLast = IdSharePath + kMaxShares - 1,
    IdShareBrowse,
    IdShareBrowseLast = IdShareBrowse + kMaxShares - 1,
    IdShareReadOnly,
    IdShareReadOnlyLast = IdShareReadOnly + kMaxShares - 1,
    IdStartNow,
    IdNetNameLabel,
    IdBridge,
    IdIntnet,
    IdMac,
    IdMacGenerate,
    IdNetInfo,
    IdForwardTitle,
    IdForwards,
    IdForwardRemove,
    IdForwardProto,
    IdForwardHost,
    IdForwardArrow,
    IdForwardGuest,
    IdForwardAdd,
    IdFullscreen,
    IdSnapshotExport,
};
constexpr int kTrackOffset = 1000;
constexpr int kSpinOffset = 2000;
constexpr std::uint64_t kGiB = 1024ull * 1024 * 1024;

std::wstring ReadText(HWND control)
{
    const int length = GetWindowTextLengthW(control);
    std::wstring value(static_cast<std::size_t>(length) + 1, L'\0');
    GetWindowTextW(control, value.data(), length + 1);
    value.resize(static_cast<std::size_t>(length));
    return value;
}

std::wstring Trim(std::wstring text)
{
    const auto first = text.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return {};
    const auto last = text.find_last_not_of(L" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::wstring BrowseFile(HWND owner, const wchar_t* filter, const wchar_t* title, bool save,
                        const wchar_t* extension, const std::wstring& initial)
{
    std::array<wchar_t, 32768> path{};
    if (!initial.empty() && initial.size() < path.size()) wcscpy_s(path.data(), path.size(), initial.c_str());
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = path.data();
    dialog.nMaxFile = static_cast<DWORD>(path.size());
    dialog.lpstrTitle = title;
    dialog.lpstrDefExt = extension;
    dialog.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER |
                   (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    const BOOL selected = save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog);
    return selected ? std::wstring(path.data()) : std::wstring();
}

std::wstring BrowseFolder(HWND owner, const wchar_t* title, const std::wstring& initial)
{
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    std::wstring result;
    IFileOpenDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dialog->SetTitle(title);
        IShellItem* folder = nullptr;
        if (!initial.empty() &&
            SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
            dialog->SetFolder(folder);
            folder->Release();
        }
        IShellItem* chosen = nullptr;
        if (SUCCEEDED(dialog->Show(owner)) && SUCCEEDED(dialog->GetResult(&chosen))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(chosen->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                result = path;
                CoTaskMemFree(path);
            }
            chosen->Release();
        }
        dialog->Release();
    }
    if (SUCCEEDED(apartment)) CoUninitialize();
    return result;
}

// Mount tag of a shared folder: what the guest types after "mount -t 9p".
bool ValidTag(const std::wstring& tag, std::wstring& error)
{
    const bool ok = !tag.empty() && tag.size() <= 31 && ((tag[0] >= L'a' && tag[0] <= L'z') || (tag[0] >= L'A' && tag[0] <= L'Z') || (tag[0] >= L'0' && tag[0] <= L'9')) && std::all_of(tag.begin(), tag.end(), [](wchar_t c) {
        return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'_' ||
               c == L'-' || c == L'.';
    });
    if (!ok) error = L"Nom de partage : 1 à 31 caractères parmi lettres sans accent, chiffres, . _ et -, en commençant par une lettre ou un chiffre.";
    return ok;
}

// A tag from the folder name ("Mes Documents" -> "mes-documents").
std::wstring SuggestTag(const std::wstring& folder, int index)
{
    std::wstring tag;
    for (const wchar_t c : fs::path(folder).filename().wstring()) {
        if (tag.size() == 31) break;
        if ((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'_' || c == L'.') tag.push_back(c);
        else if (c >= L'A' && c <= L'Z') tag.push_back(static_cast<wchar_t>(c - L'A' + L'a'));
        else if (!tag.empty() && tag.back() != L'-') tag.push_back(L'-');
    }
    while (!tag.empty() && (tag.back() == L'-' || tag.back() == L'.')) tag.pop_back();
    if (tag.empty()) tag = index == 0 ? L"partage" : L"partage" + std::to_wstring(index + 1);
    return tag;
}

bool ValidName(const std::wstring& name, std::wstring& error)
{
    if (name.empty()) {
        error = L"Donne un nom à la machine virtuelle.";
        return false;
    }
    if (name.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos || name.back() == L'.' || name.size() > 80) {
        error = L"Nom invalide : évite les caractères \\ / : * ? \" < > | et le point final.";
        return false;
    }
    return true;
}

// --- Slider: trackbar + numeric field + colored ranges ---------------------------------------

struct SliderSpec {
    unsigned minimum = 1;
    unsigned maximum = 100;
    bool logarithmic = false;
    unsigned snap = 1;   // Linear sliders: values picked with the mouse are multiples of this.
    unsigned spinStep = 1;
    std::wstring unit;
    std::function<std::wstring(unsigned)> rangeLabel;
    // Colored ranges: green up to `green`, orange up to `orange`, red beyond. 0 = none.
    unsigned green = 0;
    unsigned orange = 0;
    std::function<std::wstring(unsigned)> hint; // Text under the slider (zone meaning).
};

class Slider final {
public:
    static constexpr int kLogSteps = 1000;

    int id = 0;
    int page = 0;
    SliderSpec spec;
    HWND track = nullptr;
    HWND edit = nullptr;
    HWND spin = nullptr;
    HWND unit = nullptr;
    HWND minLabel = nullptr;
    HWND maxLabel = nullptr;
    HWND hint = nullptr;
    bool updating = false;

    int ToPos(unsigned value) const
    {
        value = std::clamp(value, spec.minimum, spec.maximum);
        if (!spec.logarithmic) return static_cast<int>(value);
        if (spec.maximum <= spec.minimum) return 0;
        const double ratio = std::log(static_cast<double>(value) / spec.minimum) /
                             std::log(static_cast<double>(spec.maximum) / spec.minimum);
        return static_cast<int>(std::lround(ratio * kLogSteps));
    }

    unsigned FromPos(int position) const
    {
        if (!spec.logarithmic) {
            auto value = static_cast<unsigned>((std::max)(position, 0));
            if (spec.snap > 1 && value != spec.maximum) value = (value + spec.snap / 2) / spec.snap * spec.snap;
            return std::clamp(value, spec.minimum, spec.maximum);
        }
        const double ratio = static_cast<double>(position) / kLogSteps;
        const double raw = spec.minimum * std::pow(static_cast<double>(spec.maximum) / spec.minimum, ratio);
        // Round to "nice" sizes: 1 under 16, 2 under 64, 8 under 256, 16 under 1024, 64 beyond.
        const unsigned step = raw < 16 ? 1 : raw < 64 ? 2 : raw < 256 ? 8 : raw < 1024 ? 16 : 64;
        auto value = static_cast<unsigned>(std::lround(raw / step)) * step;
        if (position <= 0) value = spec.minimum;
        if (position >= kLogSteps) value = spec.maximum;
        return std::clamp(value, spec.minimum, spec.maximum);
    }

    bool Parse(unsigned& value) const
    {
        const auto text = ReadText(edit);
        if (text.empty()) return false;
        wchar_t* end = nullptr;
        const unsigned long parsed = wcstoul(text.c_str(), &end, 10);
        if (*end != L'\0' || parsed < spec.minimum || parsed > spec.maximum) return false;
        value = static_cast<unsigned>(parsed);
        return true;
    }

    unsigned Value() const
    {
        unsigned value = 0;
        if (Parse(value)) return value;
        return FromPos(static_cast<int>(SendMessageW(track, TBM_GETPOS, 0, 0)));
    }

    COLORREF ZoneColor(unsigned value) const
    {
        if (spec.green == 0) return color::kSecondary;
        if (value <= spec.green) return color::kGreen;
        if (value <= spec.orange) return color::kOrange;
        return color::kRed;
    }

    void Set(unsigned value)
    {
        value = std::clamp(value, spec.minimum, spec.maximum);
        updating = true;
        SendMessageW(track, TBM_SETPOS, TRUE, ToPos(value));
        SetWindowTextW(edit, std::to_wstring(value).c_str());
        SendMessageW(spin, UDM_SETPOS32, 0, static_cast<LPARAM>(value));
        updating = false;
        UpdateHint();
    }

    void SetRange(unsigned minimum, unsigned maximum)
    {
        const auto value = Value();
        spec.minimum = minimum;
        spec.maximum = (std::max)(minimum, maximum);
        SendMessageW(track, TBM_SETRANGEMIN, FALSE, ToPos(spec.minimum));
        SendMessageW(track, TBM_SETRANGEMAX, TRUE, ToPos(spec.maximum));
        SendMessageW(spin, UDM_SETRANGE32, spec.minimum, spec.maximum);
        if (spec.rangeLabel) {
            SetWindowTextW(minLabel, spec.rangeLabel(spec.minimum).c_str());
            SetWindowTextW(maxLabel, spec.rangeLabel(spec.maximum).c_str());
        }
        Set(value);
    }

    void OnTrack()
    {
        if (updating) return;
        const auto value = FromPos(static_cast<int>(SendMessageW(track, TBM_GETPOS, 0, 0)));
        updating = true;
        SetWindowTextW(edit, std::to_wstring(value).c_str());
        SendMessageW(spin, UDM_SETPOS32, 0, static_cast<LPARAM>(value));
        updating = false;
        UpdateHint();
    }

    void OnEdit()
    {
        if (updating) return;
        unsigned value = 0;
        if (Parse(value)) {
            updating = true;
            SendMessageW(track, TBM_SETPOS, TRUE, ToPos(value));
            updating = false;
        }
        UpdateHint();
    }

    void UpdateHint();
    void Enable(bool enabled) const
    {
        for (HWND control : {track, edit, spin}) EnableWindow(control, enabled);
    }

    std::map<HWND, COLORREF>* colors = nullptr;
};

void Slider::UpdateHint()
{
    unsigned value = 0;
    std::wstring text;
    COLORREF hintColor = color::kSecondary;
    if (!Parse(value)) {
        text = L"Valeur entre " + FormatNumber(spec.minimum) + L" et " + FormatNumber(spec.maximum) + L".";
        hintColor = color::kRed;
    } else {
        if (spec.hint) text = spec.hint(value);
        hintColor = ZoneColor(value);
    }
    if (colors != nullptr) (*colors)[hint] = hintColor;
    SetWindowTextW(hint, text.c_str());
    InvalidateRect(hint, nullptr, TRUE);
}

SliderSpec MemorySpec()
{
    const auto host = Host().memoryMiB;
    SliderSpec spec;
    spec.minimum = 256;
    spec.maximum = (std::max)(512u, host * 9 / 10 / 256 * 256);
    spec.snap = 128;
    spec.spinStep = 64;
    spec.unit = L"Mio";
    spec.rangeLabel = [](unsigned value) { return FormatNumber(value) + L" Mio"; };
    spec.green = host / 2;
    spec.orange = host * 3 / 4;
    spec.hint = [host](unsigned value) -> std::wstring {
        if (value <= host / 2) return L"";
        if (value <= host * 3 / 4) return L"Plus de la moitié de la RAM de l'hôte";
        return L"Plus de 75 % de la RAM de l'hôte : Windows risque de ralentir";
    };
    return spec;
}

SliderSpec CpuSpec()
{
    const auto& host = Host();
    SliderSpec spec;
    spec.minimum = 1;
    spec.maximum = (std::min)(64u, host.logicalCpus);
    spec.unit = L"vCPU";
    spec.rangeLabel = [](unsigned value) { return std::to_wstring(value) + (value > 1 ? L" processeurs" : L" processeur"); };
    spec.green = host.physicalCores;
    spec.orange = host.physicalCores;
    spec.hint = [cores = host.physicalCores](unsigned value) -> std::wstring {
        if (value <= cores) return L"";
        return L"Au-delà des " + std::to_wstring(cores) + L" cœurs physiques : gain limité";
    };
    return spec;
}

SliderSpec DiskSpec(unsigned minimumGiB, std::uint64_t roomGiB)
{
    SliderSpec spec;
    spec.minimum = (std::max)(2u, minimumGiB);
    spec.maximum = (std::max)(spec.minimum, 4096u);
    spec.logarithmic = true;
    spec.unit = L"Gio";
    spec.rangeLabel = [](unsigned value) { return FormatNumber(value) + L" Gio"; };
    if (roomGiB != 0) {
        spec.green = static_cast<unsigned>((std::min<std::uint64_t>)(roomGiB, spec.maximum));
        spec.orange = spec.maximum;
        spec.hint = [room = roomGiB](unsigned value) -> std::wstring {
            if (value <= room) return L"";
            return L"Plus grand que l'espace libre (" + FormatNumber(room) + L" Gio) : le fichier grossit à l'usage";
        };
    }
    return spec;
}

// --- Form: modal window with pages of controls --------------------------------------------

class Form {
public:
    virtual ~Form() = default;

    bool Run(HWND owner, const std::wstring& title, int width, int height)
    {
        static const ATOM atom = [] {
            WNDCLASSEXW windowClass{sizeof(windowClass)};
            windowClass.style = CS_DBLCLKS;
            windowClass.lpfnWndProc = &Form::Proc;
            windowClass.hInstance = GetModuleHandleW(nullptr);
            windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            windowClass.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
            windowClass.lpszClassName = L"VirtualiseurForm";
            return RegisterClassExW(&windowClass);
        }();
        (void)atom;
        width_ = width;
        height_ = height;
        constexpr DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
        constexpr DWORD exStyle = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
        RECT bounds{0, 0, Scale(width), Scale(height)};
        AdjustWindowRectExForDpi(&bounds, style, FALSE, exStyle, ui::Dpi());
        RECT ownerRect{};
        GetWindowRect(owner, &ownerRect);
        const int windowWidth = bounds.right - bounds.left;
        const int windowHeight = bounds.bottom - bounds.top;
        int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - windowWidth) / 2;
        int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - windowHeight) / 2;
        MONITORINFO monitor{sizeof(monitor)};
        if (GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor)) {
            const int left = static_cast<int>(monitor.rcWork.left);
            const int top = static_cast<int>(monitor.rcWork.top);
            x = std::clamp(x, left, (std::max)(left, static_cast<int>(monitor.rcWork.right) - windowWidth));
            y = std::clamp(y, top, (std::max)(top, static_cast<int>(monitor.rcWork.bottom) - windowHeight));
        }
        CreateWindowExW(exStyle, L"VirtualiseurForm", title.c_str(), style, x, y, windowWidth, windowHeight,
                        owner, nullptr, GetModuleHandleW(nullptr), this);
        if (window_ == nullptr) return false;
        ui::ThemeWindow(window_); // Dark or light title bar.
        Build();
        ShowPage(page_);
        OnPageShown(page_);
        EnableWindow(owner, FALSE);
        ShowWindow(window_, SW_SHOW);
        Focus();

        MSG message{};
        bool quit = false;
        while (!done_) {
            const BOOL result = GetMessageW(&message, nullptr, 0, 0);
            if (result <= 0) {
                quit = result == 0;
                break;
            }
            if (!IsDialogMessageW(window_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        EnableWindow(owner, TRUE);
        DestroyWindow(window_);
        SetForegroundWindow(owner);
        if (quit) PostQuitMessage(static_cast<int>(message.wParam));
        return accepted_;
    }

protected:
    virtual void Build() = 0;
    // OK pressed: validates and stores the result; false keeps the window open.
    virtual bool Commit() = 0;
    virtual void Command(int, int) {}
    virtual void SliderChanged(Slider&) {}
    virtual void PaintBackground(HDC, const RECT&) {}
    virtual bool DrawItem(const DRAWITEMSTRUCT&) { return false; }
    virtual void Focus() {}
    // Annuler, Escape or the close box: false keeps the window open.
    virtual bool CanClose() { return true; }
    virtual void Timer(UINT_PTR) {}

    // --- Building (DIP coordinates) ---
    HWND Add(int page, int id, const wchar_t* className, const std::wstring& text, DWORD style,
             int x, int y, int width, int height, DWORD exStyle = 0, HFONT font = nullptr)
    {
        HWND control = CreateWindowExW(exStyle, className, text.c_str(), WS_CHILD | style, Scale(x), Scale(y),
                                       Scale(width), Scale(height), window_,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font != nullptr ? font : ui::Font().body), TRUE);
        if (id > 0) items_[id] = control;
        ui::ThemeControl(control);
        paged_.emplace_back(page, control);
        return control;
    }

    HWND AddStatic(int page, const std::wstring& text, int x, int y, int width, int height = 20,
                   HFONT font = nullptr, COLORREF textColor = CLR_INVALID, DWORD style = SS_LEFT, int id = -1)
    {
        HWND control = Add(page, id, L"STATIC", text, style | SS_NOPREFIX, x, y, width, height, 0, font);
        if (textColor != CLR_INVALID) colors_[control] = textColor;
        return control;
    }

    void AddLabel(int page, const std::wstring& text, int x, int y, int width = 160)
    {
        AddStatic(page, text, x, y + 5, width, 20);
    }

    HWND AddEdit(int page, int id, const std::wstring& text, int x, int y, int width, DWORD style = ES_AUTOHSCROLL)
    {
        return Add(page, id, L"EDIT", text, WS_TABSTOP | style, x, y, width, 26, WS_EX_CLIENTEDGE);
    }

    HWND AddCombo(int page, int id, const std::vector<std::wstring>& items, int x, int y, int width, bool editable = false)
    {
        HWND combo = Add(page, id, WC_COMBOBOXW, L"",
                         WS_TABSTOP | WS_VSCROLL | (editable ? CBS_DROPDOWN | CBS_AUTOHSCROLL : CBS_DROPDOWNLIST), x, y,
                         width, 300);
        for (const auto& item : items) SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.c_str()));
        if (!editable) SendMessageW(combo, CB_SETCURSEL, 0, 0);
        return combo;
    }

    HWND AddList(int page, int id, int x, int y, int width, int height)
    {
        return Add(page, id, WC_LISTBOXW, L"", WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, x, y, width,
                   height, WS_EX_CLIENTEDGE);
    }

    // Shows or hides one control of the current page (mode-dependent fields).
    void Show(int id, bool visible) const { ShowWindow(Item(id), visible ? SW_SHOWNA : SW_HIDE); }

    HWND AddCheck(int page, int id, const std::wstring& text, int x, int y, int width)
    {
        return Add(page, id, L"BUTTON", text, WS_TABSTOP | BS_AUTOCHECKBOX, x, y, width, 24);
    }

    HWND AddButton(int page, int id, const std::wstring& text, int x, int y, int width, int height = 30, bool primary = false)
    {
        return Add(page, id, L"BUTTON", text, WS_TABSTOP | (primary ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON), x, y, width, height);
    }

    // Section title followed by a thin line.
    void AddHeading(int page, const std::wstring& text, int x, int y, int width)
    {
        // The label is only as wide as its text, so the line beside it stays visible.
        HDC dc = GetDC(window_);
        const int textWidth = ui::TextWidth(dc, text, ui::Font().bold);
        ReleaseDC(window_, dc);
        const int labelWidth = MulDiv(textWidth, 96, static_cast<int>(ui::Dpi())) + 4;
        AddStatic(page, text, x, y, labelWidth, 22, ui::Font().bold, color::kText);
        headings_.push_back({page, RECT{Scale(x) + textWidth + Scale(12), Scale(y + 11), Scale(x + width), Scale(y + 12)}});
    }

    Slider* AddSlider(int page, int id, int x, int y, int width, SliderSpec spec, unsigned value)
    {
        auto slider = std::make_unique<Slider>();
        slider->id = id;
        slider->page = page;
        slider->spec = std::move(spec);
        slider->colors = &colors_;
        const int fieldWidth = 92;
        const int trackWidth = width - fieldWidth - 46;
        slider->track = Add(page, id + kTrackOffset, TRACKBAR_CLASSW, L"",
                            WS_TABSTOP | TBS_HORZ | TBS_NOTICKS | TBS_DOWNISLEFT, x - 6, y, trackWidth + 12, 30);
        SendMessageW(slider->track, TBM_SETRANGEMIN, FALSE, slider->ToPos(slider->spec.minimum));
        SendMessageW(slider->track, TBM_SETRANGEMAX, FALSE, slider->ToPos(slider->spec.maximum));
        const int page10 = slider->spec.logarithmic ? Slider::kLogSteps / 10
                                                    : (std::max)(1, static_cast<int>(slider->spec.maximum - slider->spec.minimum) / 10);
        SendMessageW(slider->track, TBM_SETPAGESIZE, 0, page10);
        SendMessageW(slider->track, TBM_SETLINESIZE, 0,
                     slider->spec.logarithmic ? 10 : static_cast<LPARAM>((std::max)(1u, slider->spec.snap)));
        slider->edit = AddEdit(page, id, L"", x + trackWidth + 14, y + 2, fieldWidth, ES_NUMBER | ES_RIGHT);
        slider->spin = Add(page, id + kSpinOffset, UPDOWN_CLASSW, L"",
                           UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_NOTHOUSANDS | UDS_HOTTRACK, 0, 0, 0, 0);
        SendMessageW(slider->spin, UDM_SETBUDDY, reinterpret_cast<WPARAM>(slider->edit), 0);
        SendMessageW(slider->spin, UDM_SETRANGE32, slider->spec.minimum, slider->spec.maximum);
        UDACCEL accel{0, slider->spec.spinStep};
        SendMessageW(slider->spin, UDM_SETACCEL, 1, reinterpret_cast<LPARAM>(&accel));
        slider->unit = AddStatic(page, slider->spec.unit, x + trackWidth + 14 + fieldWidth + 6, y + 6, 40, 20);
        const auto rangeText = [&](unsigned bound) {
            return slider->spec.rangeLabel ? slider->spec.rangeLabel(bound) : std::to_wstring(bound);
        };
        slider->minLabel = AddStatic(page, rangeText(slider->spec.minimum), x, y + 38, 110, 18,
                                     ui::Font().caption, color::kSecondary);
        slider->maxLabel = AddStatic(page, rangeText(slider->spec.maximum), x + trackWidth - 110, y + 38, 110, 18,
                                     ui::Font().caption, color::kSecondary, SS_RIGHT);
        slider->hint = AddStatic(page, L"", x, y + 56, width, 18, ui::Font().caption, color::kSecondary);
        auto* result = slider.get();
        sliders_.push_back(std::move(slider));
        result->Set(value);
        return result;
    }

    void ShowPage(int page)
    {
        page_ = page;
        for (const auto& [owner, control] : paged_) ShowWindow(control, owner < 0 || owner == page ? SW_SHOWNA : SW_HIDE);
        InvalidateRect(window_, nullptr, TRUE);
    }

    void SetMessage(const std::wstring& text, bool error = true)
    {
        HWND label = Item(IdMessage);
        colors_[label] = error ? color::kRed : color::kSecondary;
        SetWindowTextW(label, text.c_str());
        InvalidateRect(label, nullptr, TRUE);
    }

    void Fail(int page, int focusId, const std::wstring& text)
    {
        if (page >= 0 && page != page_) {
            ShowPage(page);
            OnPageShown(page);
        }
        if (Item(focusId) != nullptr && !IsWindowVisible(Item(focusId))) focusId = -1;
        SetMessage(text);
        if (HWND control = Item(focusId)) {
            SetFocus(control);
            SendMessageW(control, EM_SETSEL, 0, -1);
        }
        MessageBeep(MB_ICONWARNING);
    }
    virtual void OnPageShown(int) {}

    HWND Item(int id) const
    {
        const auto found = items_.find(id);
        return found == items_.end() ? nullptr : found->second;
    }
    std::wstring TextOf(int id) const { return Trim(ReadText(Item(id))); }
    void SetText(int id, const std::wstring& text) const { SetWindowTextW(Item(id), text.c_str()); }
    int ComboIndex(int id) const { return static_cast<int>(SendMessageW(Item(id), CB_GETCURSEL, 0, 0)); }
    void SetComboIndex(int id, int index) const { SendMessageW(Item(id), CB_SETCURSEL, static_cast<WPARAM>(index), 0); }
    bool Checked(int id) const { return SendMessageW(Item(id), BM_GETCHECK, 0, 0) == BST_CHECKED; }
    void SetChecked(int id, bool checked) const { SendMessageW(Item(id), BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0); }
    void SetColor(int id, COLORREF textColor)
    {
        colors_[Item(id)] = textColor;
        InvalidateRect(Item(id), nullptr, TRUE);
    }
    Slider* SliderById(int id) const
    {
        for (const auto& slider : sliders_) {
            if (slider->id == id) return slider.get();
        }
        return nullptr;
    }

    // The window was asked to close (Escape, close box) during a long operation.
    bool Closing() const noexcept { return done_; }
    // Ends the modal loop (Run returns `accepted`).
    void Close(bool accepted) noexcept
    {
        accepted_ = accepted;
        done_ = true;
    }

    HWND window_ = nullptr;
    int page_ = 0;
    int width_ = 0;
    int height_ = 0;

private:
    static LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        Form* self = nullptr;
        if (message == WM_NCCREATE) {
            self = static_cast<Form*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<Form*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        }
        if (self == nullptr) return DefWindowProcW(window, message, wParam, lParam);
        return self->Handle(message, wParam, lParam);
    }

    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window_, &paint);
            RECT client{};
            GetClientRect(window_, &client);
            {
                ui::BufferedPaint buffer(dc, client);
                const HDC target = buffer.Dc();
                ui::Fill(target, client, color::kPanel);
                PaintBackground(target, client);
                for (const auto& heading : headings_) {
                    if (heading.page == page_ || heading.page < 0) ui::Fill(target, heading.line, color::kBorder);
                }
                for (const auto& slider : sliders_) {
                    if (slider->page == page_ || slider->page < 0) PaintBands(target, *slider);
                }
            }
            EndPaint(window_, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            const auto dc = reinterpret_cast<HDC>(wParam);
            const auto control = reinterpret_cast<HWND>(lParam);
            const auto found = colors_.find(control);
            SetTextColor(dc, found != colors_.end() ? found->second : color::kText);
            SetBkColor(dc, color::kPanel);
            SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(ui::Brush(color::kPanel));
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            // The category list sits on the side pane; other lists, edits and
            // combo box drop-downs are on the panel color.
            const auto dc = reinterpret_cast<HDC>(wParam);
            const COLORREF background = reinterpret_cast<HWND>(lParam) == Item(IdCategories) ? color::kWindow : color::kPanel;
            SetTextColor(dc, color::kText);
            SetBkColor(dc, background);
            return reinterpret_cast<LRESULT>(ui::Brush(background));
        }
        case WM_NOTIFY: {
            const auto* header = reinterpret_cast<const NMHDR*>(lParam);
            if (header->code == NM_CUSTOMDRAW &&
                (GetWindowLongW(header->hwndFrom, GWL_STYLE) & BS_TYPEMASK) == BS_AUTOCHECKBOX) {
                wchar_t kind[16] = {};
                GetClassNameW(header->hwndFrom, kind, 16);
                if (_wcsicmp(kind, L"Button") == 0) {
                    return ui::DrawCheckBox(*reinterpret_cast<const NMCUSTOMDRAW*>(lParam), color::kPanel);
                }
            }
            break;
        }
        case WM_MEASUREITEM: {
            auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
            if (measure->CtlType == ODT_LISTBOX) {
                measure->itemHeight = static_cast<UINT>(Scale(measure->CtlID == IdSnapshotList ? 30 : 38));
                return TRUE;
            }
            break;
        }
        case WM_DRAWITEM:
            if (DrawItem(*reinterpret_cast<DRAWITEMSTRUCT*>(lParam))) return TRUE;
            break;
        case WM_HSCROLL:
            for (const auto& slider : sliders_) {
                if (reinterpret_cast<HWND>(lParam) == slider->track) {
                    slider->OnTrack();
                    SliderChanged(*slider);
                    return 0;
                }
            }
            break;
        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            const int code = HIWORD(wParam);
            if (id == IDOK && code == BN_CLICKED) {
                if (Commit()) {
                    accepted_ = true;
                    done_ = true;
                }
                return 0;
            }
            if (id == IDCANCEL) {
                if (CanClose()) done_ = true;
                return 0;
            }
            if (code == EN_CHANGE) {
                if (Slider* slider = SliderById(id)) {
                    slider->OnEdit();
                    SliderChanged(*slider);
                    return 0;
                }
            }
            Command(id, code);
            return 0;
        }
        case DM_GETDEFID:
            return MAKELRESULT(IDOK, DC_HASDEFID);
        case WM_CLOSE:
            if (CanClose()) done_ = true;
            return 0;
        case WM_TIMER:
            Timer(wParam);
            return 0;
        default:
            break;
        }
        return DefWindowProcW(window_, message, wParam, lParam);
    }

    void PaintBands(HDC dc, const Slider& slider) const
    {
        if (slider.spec.green == 0) return;
        RECT channel{};
        SendMessageW(slider.track, TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM>(&channel));
        RECT thumb{};
        SendMessageW(slider.track, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
        RECT trackRect{};
        GetWindowRect(slider.track, &trackRect);
        MapWindowPoints(nullptr, window_, reinterpret_cast<POINT*>(&trackRect), 2);
        const int half = (thumb.right - thumb.left) / 2;
        const int left = trackRect.left + channel.left + half;
        const int right = trackRect.left + channel.right - half;
        const int top = trackRect.bottom + Scale(1);
        const int bottom = top + Scale(4);
        const int low = slider.ToPos(slider.spec.minimum);
        const int high = slider.ToPos(slider.spec.maximum);
        if (high <= low || right <= left) return;
        const auto toX = [&](unsigned value) {
            return left + static_cast<int>(static_cast<long long>(slider.ToPos(value) - low) * (right - left) / (high - low));
        };
        const int greenEnd = toX((std::min)(slider.spec.green, slider.spec.maximum));
        const int orangeEnd = toX((std::min)((std::max)(slider.spec.orange, slider.spec.green), slider.spec.maximum));
        ui::Fill(dc, RECT{left, top, greenEnd, bottom}, ui::Mix(color::kPanel, color::kGreen, 60));
        if (orangeEnd > greenEnd) ui::Fill(dc, RECT{greenEnd, top, orangeEnd, bottom}, ui::Mix(color::kPanel, color::kOrange, 60));
        if (right > orangeEnd) ui::Fill(dc, RECT{orangeEnd, top, right, bottom}, ui::Mix(color::kPanel, color::kRed, 60));
    }

    struct Heading {
        int page;
        RECT line;
    };
    std::map<int, HWND> items_;
    std::vector<std::pair<int, HWND>> paged_;
    std::vector<Heading> headings_;
    std::vector<std::unique_ptr<Slider>> sliders_;
    bool done_ = false;
    bool accepted_ = false;

protected:
    std::map<HWND, COLORREF> colors_;
};

// --- Settings ----------------------------------------------------------------------------

struct Category {
    wchar_t glyph;
    const wchar_t* title;
};
constexpr std::array<Category, kPageCount> kCategories{{
    {ui::glyph::kInfo, L"Général"},
    {ui::glyph::kSystem, L"Système"},
    {ui::glyph::kDisplay, L"Affichage"},
    {ui::glyph::kDisk, L"Stockage"},
    {ui::glyph::kAudio, L"Audio"},
    {ui::glyph::kNetwork, L"Réseau"},
    {ui::glyph::kFolder, L"Dossiers partagés"},
    {ui::glyph::kInstall, L"Installation"},
}};

class SettingsDialog final : public Form {
public:
    SettingsDialog(const VmEntry& vm, const std::vector<VmEntry>& library, VmConfig& result, int startPage)
        : vm_(vm), library_(library), result_(result)
    {
        page_ = std::clamp(startPage, 0, kPageCount - 1);
    }

private:
    static constexpr int kWidth = 840;
    static constexpr int kHeight = 590;
    static constexpr int kLeft = 232;  // Content column.
    static constexpr int kField = 400; // Field column.
    static constexpr int kRight = kWidth - 28;
    static constexpr int kFieldWidth = kRight - kField;

    void Build() override
    {
        const auto& c = vm_.config;
        HWND list = Add(-1, IdCategories, WC_LISTBOXW, L"", WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_OWNERDRAWFIXED |
                        LBS_NOINTEGRALHEIGHT, 8, 12, 200, kHeight - 84);
        for (std::size_t index = 0; index < kCategories.size(); ++index) SendMessageW(list, LB_ADDSTRING, 0, 0);
        SendMessageW(list, LB_SETCURSEL, static_cast<WPARAM>(page_), 0);
        AddStatic(-1, kCategories[static_cast<std::size_t>(page_)].title, kLeft, 16, kRight - kLeft, 36,
                  ui::Font().title, color::kText, SS_LEFT, IdPageTitle);

        // Général.
        AddLabel(kPageGeneral, L"Nom :", kLeft, 70);
        AddEdit(kPageGeneral, IdName, c.name, kField, 70, kFieldWidth);
        AddLabel(kPageGeneral, L"Système invité :", kLeft, 110);
        AddEdit(kPageGeneral, IdOs, c.os, kField, 110, kFieldWidth);
        SendMessageW(Item(IdOs), EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"ex. Debian 13 (détecté depuis l'ISO)"));
        AddLabel(kPageGeneral, L"Dossier :", kLeft, 150);
        AddStatic(kPageGeneral, vm_.directory.wstring(), kField, 155, kFieldWidth - 108, 20, nullptr, color::kSecondary,
                  SS_LEFT | SS_PATHELLIPSIS, IdFolderPath);
        AddButton(kPageGeneral, IdOpenFolder, L"Ouvrir", kRight - 100, 148, 100);
        AddStatic(kPageGeneral,
                  L"Virtualiseur exécute la machine avec Windows Hypervisor Platform : Hyper-V, VBS et "
                  L"l'intégrité de la mémoire restent activés.",
                  kLeft, 204, kRight - kLeft, 60, ui::Font().caption, color::kSecondary);

        // Système.
        AddLabel(kPageSystem, L"Mémoire vive :", kLeft, 72);
        AddSlider(kPageSystem, IdMemory, kField, 70, kFieldWidth, MemorySpec(), c.memoryMiB);
        AddLabel(kPageSystem, L"Processeurs :", kLeft, 162);
        AddSlider(kPageSystem, IdCpus, kField, 160, kFieldWidth, CpuSpec(), c.cpus);
        AddLabel(kPageSystem, L"Ordre de démarrage :", kLeft, 256);
        AddCombo(kPageSystem, IdBoot, {L"Automatique (disque s'il contient Linux, sinon CD)", L"Lecteur CD (ISO)", L"Disque dur"},
                 kField, 256, kFieldWidth);
        SetComboIndex(IdBoot, c.boot == BootOrder::Cdrom ? 1 : c.boot == BootOrder::Disk ? 2 : 0);
        AddLabel(kPageSystem, L"Horloge matérielle :", kLeft, 296);
        AddCombo(kPageSystem, IdRtc, {L"UTC (Linux)", L"Heure locale (Windows)"}, kField, 296, kFieldWidth);
        SetComboIndex(IdRtc, c.rtcLocal ? 1 : 0);
        const auto& host = Host();
        AddStatic(kPageSystem,
                  L"Hôte : " + FormatBytes(static_cast<std::uint64_t>(host.memoryMiB) * 1024 * 1024) + L" de RAM, " +
                      std::to_wstring(host.physicalCores) + L" cœurs physiques, " + std::to_wstring(host.logicalCpus) +
                      L" processeurs logiques. Les vCPU de la VM sont des threads de ce PC.",
                  kLeft, 344, kRight - kLeft, 40, ui::Font().caption, color::kSecondary);

        // Affichage.
        AddLabel(kPageDisplay, L"Carte graphique :", kLeft, 70);
        AddCombo(kPageDisplay, IdGpu, {L"virtio-gpu (recommandé)", L"Framebuffer simple (compatibilité)"},
                 kField, 70, kFieldWidth);
        SetComboIndex(IdGpu, c.virtioGpu ? 0 : 1);
        AddLabel(kPageDisplay, L"Résolution initiale :", kLeft, 110);
        std::vector<std::wstring> resolutions(std::begin(kResolutions), std::end(kResolutions));
        if (std::find(resolutions.begin(), resolutions.end(), c.resolution) == resolutions.end()) {
            resolutions.push_back(c.resolution);
        }
        resolutions_ = resolutions;
        std::vector<std::wstring> shown;
        for (const auto& item : resolutions) {
            auto text = item;
            std::replace(text.begin(), text.end(), L'x', L'×');
            shown.push_back(text);
        }
        AddCombo(kPageDisplay, IdResolution, shown, kField, 110, kFieldWidth);
        SetComboIndex(IdResolution, static_cast<int>(std::find(resolutions.begin(), resolutions.end(), c.resolution) -
                                                     resolutions.begin()));
        AddCheck(kPageDisplay, Id3d, L"Activer l'accélération 3D (OpenGL avec virgl)", kField, 152, kFieldWidth);
        SetChecked(Id3d, c.gpu3d);
        AddStatic(kPageDisplay, L"", kField, 180, kFieldWidth, 40, ui::Font().caption, color::kSecondary, SS_LEFT, Id3dStatus);
        AddCheck(kPageDisplay, IdFullscreen, L"Démarrer en plein écran (Ctrl droit + F pour en sortir)", kField, 226,
                 kFieldWidth);
        SetChecked(IdFullscreen, c.fullscreen);
        AddStatic(kPageDisplay,
                  L"Avec virtio-gpu, la résolution de l'invité suit la taille de la fenêtre, en direct pendant qu'on "
                  L"l'étire, et en plein écran. L'accélération 3D fait dessiner le bureau et les applications OpenGL "
                  L"de l'invité par le GPU de l'hôte.",
                  kLeft, 272, kRight - kLeft, 60, ui::Font().caption, color::kSecondary);
        UpdateDisplayState();

        // Stockage.
        AddHeading(kPageStorage, L"Disque dur", kLeft, 64, kRight - kLeft);
        AddLabel(kPageStorage, L"Fichier :", kLeft, 98);
        AddEdit(kPageStorage, IdDisk, c.disk.wstring(), kField, 98, kFieldWidth - 108);
        AddButton(kPageStorage, IdBrowseDisk, L"Parcourir…", kRight - 100, 96, 100);
        AddLabel(kPageStorage, L"Taille :", kLeft, 140);
        AddSlider(kPageStorage, IdDiskSize, kField, 138, kFieldWidth, DiskSpec(2, 0), c.diskGiB);
        AddStatic(kPageStorage, L"", kField, 216, kFieldWidth, 36, ui::Font().caption, color::kSecondary, SS_LEFT, IdDiskInfo);
        AddLabel(kPageStorage, L"Contrôleur :", kLeft, 258);
        AddCombo(kPageStorage, IdDiskBus, {L"virtio-blk (rapide, recommandé)", L"IDE (compatibilité)"}, kField, 258, kFieldWidth);
        SetComboIndex(IdDiskBus, c.diskBus == DiskBus::Ide ? 1 : 0);
        AddHeading(kPageStorage, L"Lecteur optique", kLeft, 312, kRight - kLeft);
        AddLabel(kPageStorage, L"Image ISO :", kLeft, 346);
        AddEdit(kPageStorage, IdIso, c.iso.wstring(), kField, 346, kFieldWidth - 108);
        AddButton(kPageStorage, IdBrowseIso, L"Parcourir…", kRight - 100, 344, 100);
        AddStatic(kPageStorage, L"", kField, 384, kFieldWidth - 108, 20, ui::Font().caption, color::kSecondary, SS_LEFT, IdIsoInfo);
        AddButton(kPageStorage, IdEject, L"Éjecter", kRight - 100, 380, 100);
        UpdateDiskInfo();
        UpdateIsoInfo();

        // Audio.
        AddCheck(kPageAudio, IdAudio, L"Activer le son", kLeft, 70, kRight - kLeft);
        SetChecked(IdAudio, c.audio);
        AddLabel(kPageAudio, L"Contrôleur :", kLeft, 110);
        AddStatic(kPageAudio, L"virtio-sound (pilote Linux virtio_snd)", kField, 115, kFieldWidth);
        AddLabel(kPageAudio, L"Sortie hôte :", kLeft, 140);
        AddStatic(kPageAudio, L"Périphérique audio par défaut de Windows (WASAPI)", kField, 145, kFieldWidth);
        AddLabel(kPageAudio, L"Entrée hôte :", kLeft, 170);
        AddStatic(kPageAudio, L"Micro par défaut de Windows (WASAPI)", kField, 175, kFieldWidth);
        AddCheck(kPageAudio, IdMicrophone, L"Autoriser la machine à enregistrer avec le micro de Windows", kLeft, 210,
                 kRight - kLeft);
        SetChecked(IdMicrophone, c.microphone);
        AddStatic(kPageAudio,
                  L"Le son de la machine sort sur les haut-parleurs ou le casque choisis dans Windows, et suit "
                  L"automatiquement les changements de périphérique. Il se coupe à tout moment depuis le menu "
                  L"« Périphériques » de la fenêtre de la machine, et apparaît dans le mélangeur de volume de Windows. "
                  L"Sans l'autorisation du micro, la machine voit une entrée audio muette ; le micro de Windows n'est "
                  L"ouvert que pendant qu'elle enregistre, et le même menu l'autorise ou le coupe en cours de route.",
                  kLeft, 250, kRight - kLeft, 80, ui::Font().caption, color::kSecondary);

        // Réseau.
        AddCheck(kPageNetwork, IdNetwork, L"Activer la carte réseau (virtio-net)", kLeft, 64, kRight - kLeft);
        SetChecked(IdNetwork, c.network != NetworkMode::None);
        AddLabel(kPageNetwork, L"Mode d'accès :", kLeft, 100);
        AddCombo(kPageNetwork, IdNetMode, {L"NAT", L"Accès par pont", L"Réseau interne", L"Réseau privé hôte",
                                           L"Câble débranché"}, kField, 100, kFieldWidth);
        SetComboIndex(IdNetMode, ModeIndex(c.network));
        AddStatic(kPageNetwork, L"", kLeft, 145, 160, 20, nullptr, CLR_INVALID, SS_LEFT, IdNetNameLabel);
        adapters_ = HostNetworkAdapters();
        std::vector<std::wstring> adapterItems;
        int adapterIndex = 0;
        for (std::size_t index = 0; index < adapters_.size(); ++index) {
            const auto& adapter = adapters_[index];
            adapterItems.push_back(adapter.name + L" — " + adapter.description +
                                   (adapter.connected ? L"" : L" (déconnectée)"));
            if (_wcsicmp(adapter.name.c_str(), c.bridgeAdapter.c_str()) == 0) adapterIndex = static_cast<int>(index);
        }
        if (!c.bridgeAdapter.empty() && std::none_of(adapters_.begin(), adapters_.end(), [&](const HostAdapterInfo& item) {
                return _wcsicmp(item.name.c_str(), c.bridgeAdapter.c_str()) == 0;
            })) {
            adapters_.push_back({c.bridgeAdapter, L"", false, false});
            adapterItems.push_back(c.bridgeAdapter + L" (introuvable)");
            adapterIndex = static_cast<int>(adapters_.size()) - 1;
        }
        AddCombo(kPageNetwork, IdBridge, adapterItems, kField, 140, kFieldWidth);
        SetComboIndex(IdBridge, adapterIndex);
        std::vector<std::wstring> networks{L"intnet"};
        for (const auto& other : library_) {
            const auto& name = other.config.internalNetwork;
            if (!name.empty() && std::find(networks.begin(), networks.end(), name) == networks.end()) networks.push_back(name);
        }
        AddCombo(kPageNetwork, IdIntnet, networks, kField, 140, kFieldWidth, true);
        SetText(IdIntnet, c.internalNetwork);
        AddLabel(kPageNetwork, L"Adresse MAC :", kLeft, 180);
        AddEdit(kPageNetwork, IdMac, c.mac, kField, 180, 180);
        AddButton(kPageNetwork, IdMacGenerate, L"Générer", kField + 188, 178, 100);
        AddStatic(kPageNetwork, L"", kLeft, 224, kRight - kLeft, 54, ui::Font().caption, color::kSecondary, SS_LEFT, IdNetInfo);
        AddStatic(kPageNetwork, L"Redirection de ports (depuis 127.0.0.1 sur l'hôte)", kLeft, 290, kRight - kLeft, 22,
                  ui::Font().bold, color::kText, SS_LEFT, IdForwardTitle);
        AddList(kPageNetwork, IdForwards, kLeft, 318, kRight - kLeft - 112, 92);
        AddButton(kPageNetwork, IdForwardRemove, L"Supprimer", kRight - 100, 318, 100);
        AddCombo(kPageNetwork, IdForwardProto, {L"TCP", L"UDP"}, kLeft, 420, 80);
        AddEdit(kPageNetwork, IdForwardHost, L"", kLeft + 90, 420, 100, ES_NUMBER);
        SendMessageW(Item(IdForwardHost), EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Port hôte"));
        AddStatic(kPageNetwork, L"→", kLeft + 196, 423, 22, 20, nullptr, CLR_INVALID, SS_CENTER, IdForwardArrow);
        AddEdit(kPageNetwork, IdForwardGuest, L"", kLeft + 224, 420, 100, ES_NUMBER);
        SendMessageW(Item(IdForwardGuest), EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Port invité"));
        AddButton(kPageNetwork, IdForwardAdd, L"Ajouter", kLeft + 334, 418, 100);
        forwards_ = c.forwards;
        RefreshForwards();

        // Dossiers partagés.
        AddLabel(kPageShares, L"Nom (tag)", kLeft, 70, 120);
        AddLabel(kPageShares, L"Dossier Windows", kLeft + 130, 70, 300);
        for (int index = 0; index < static_cast<int>(kMaxShares); ++index) {
            const auto& share = c.shares[static_cast<std::size_t>(index)];
            const int y = 100 + index * 72;
            AddEdit(kPageShares, IdShareTag + index, share.tag, kLeft, y, 120);
            SendMessageW(Item(IdShareTag + index), EM_SETCUEBANNER, TRUE,
                         reinterpret_cast<LPARAM>(index == 0 ? L"partage" : L"tag"));
            AddEdit(kPageShares, IdSharePath + index, share.path.wstring(), kLeft + 130, y, kRight - kLeft - 130 - 108);
            SendMessageW(Item(IdSharePath + index), EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Aucun dossier"));
            AddButton(kPageShares, IdShareBrowse + index, L"Parcourir…", kRight - 100, y - 2, 100);
            AddCheck(kPageShares, IdShareReadOnly + index, L"Lecture seule", kLeft + 130, y + 32, 200);
            SetChecked(IdShareReadOnly + index, share.readOnly);
        }
        AddStatic(kPageShares,
                  L"Au démarrage, chaque dossier est monté tout seul dans la machine sur /mnt/<nom> par les outils "
                  L"invité, pour toute distribution Linux : machine allumée, Machine → Insérer le CD des outils invité, "
                  L"puis sh install.sh depuis le CD. Sans eux : sudo mount -t 9p -o trans=virtio,version=9p2000.L "
                  L"partage /mnt/partage. "
                  L"La machine ne peut pas sortir du dossier choisi ; liens symboliques et verrous de fichiers ne sont "
                  L"pas pris en charge. Effacer le chemin retire le partage.",
                  kLeft, 330, kRight - kLeft, 100, ui::Font().caption, color::kSecondary);

        // Installation.
        AddLabel(kPageInstall, L"Installateur Debian :", kLeft, 70);
        AddCombo(kPageInstall, IdInstaller, {L"Texte (clavier)", L"Graphique (souris)"}, kField, 70, kFieldWidth);
        SetComboIndex(IdInstaller, c.graphicalInstaller ? 1 : 0);
        AddLabel(kPageInstall, L"Fichier preseed :", kLeft, 110);
        AddEdit(kPageInstall, IdPreseed, c.preseed.wstring(), kField, 110, kFieldWidth - 108);
        SendMessageW(Item(IdPreseed), EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Aucun (installation interactive)"));
        AddButton(kPageInstall, IdBrowsePreseed, L"Parcourir…", kRight - 100, 108, 100);
        AddCheck(kPageInstall, IdGuestTools, L"Installer les outils invité pendant l'installation Debian", kField, 150, kFieldWidth);
        SetChecked(IdGuestTools, c.guestTools);
        AddStatic(kPageInstall,
                  L"Ces réglages ne servent qu'au démarrage sur l'ISO d'installation Debian. Le fichier preseed "
                  L"(exemple : docs\\preseed-debian.cfg) automatise entièrement l'installation. Les outils invité "
                  L"apportent le presse-papiers partagé et le dépôt de fichiers par glisser-déposer.",
                  kLeft, 196, kRight - kLeft, 80, ui::Font().caption, color::kSecondary);

        // Bottom bar.
        AddStatic(-1, L"", kLeft, kHeight - 42, kRight - kLeft - 230, 34, ui::Font().caption, color::kRed, SS_LEFT, IdMessage);
        AddButton(-1, IDOK, L"OK", kRight - 212, kHeight - 46, 100, 32, true);
        AddButton(-1, IDCANCEL, L"Annuler", kRight - 100, kHeight - 46, 100, 32);
    }

    void Focus() override { SetFocus(Item(IdCategories)); }

    void PaintBackground(HDC dc, const RECT& client) override
    {
        ui::Fill(dc, RECT{0, 0, Scale(216), client.bottom}, color::kWindow);
        ui::Fill(dc, RECT{Scale(216), 0, Scale(217), client.bottom}, color::kBorder);
        ui::Fill(dc, RECT{Scale(217), client.bottom - Scale(60), client.right, client.bottom - Scale(59)}, color::kBorder);
    }

    bool DrawItem(const DRAWITEMSTRUCT& item) override
    {
        if (item.CtlID != IdCategories || item.itemID >= kCategories.size()) return false;
        const auto& category = kCategories[item.itemID];
        ui::BufferedPaint buffer(item.hDC, item.rcItem);
        const HDC dc = buffer.Dc();
        ui::Fill(dc, item.rcItem, color::kWindow);
        const bool selected = (item.itemState & ODS_SELECTED) != 0;
        RECT box = item.rcItem;
        InflateRect(&box, -Scale(4), -Scale(2));
        if (selected) {
            ui::RoundedRect(dc, box, Scale(5), color::kPanel, color::kBorder);
            RECT bar{box.left + Scale(1), box.top + Scale(9), box.left + Scale(4), box.bottom - Scale(9)};
            ui::RoundedRect(dc, bar, Scale(1), color::kAccent);
        }
        RECT glyphRect{box.left + Scale(12), box.top, box.left + Scale(40), box.bottom};
        ui::Text(dc, std::wstring(1, category.glyph), glyphRect, ui::Font().glyph, PageColor(static_cast<int>(item.itemID)),
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        RECT textRect{box.left + Scale(48), box.top, box.right - Scale(4), box.bottom};
        ui::Text(dc, category.title, textRect, selected ? ui::Font().bold : ui::Font().body, color::kText);
        return true;
    }

    void OnPageShown(int page) override
    {
        SendMessageW(Item(IdCategories), LB_SETCURSEL, static_cast<WPARAM>(page), 0);
        SetText(IdPageTitle, kCategories[static_cast<std::size_t>(page)].title);
        UpdateNetworkState();
    }

    static constexpr std::array<NetworkMode, 5> kModes{NetworkMode::Nat, NetworkMode::Bridged, NetworkMode::Internal,
                                                       NetworkMode::HostOnly, NetworkMode::Disconnected};
    static int ModeIndex(NetworkMode mode)
    {
        const auto found = std::find(kModes.begin(), kModes.end(), mode);
        return found == kModes.end() ? 0 : static_cast<int>(found - kModes.begin());
    }
    NetworkMode SelectedMode() const
    {
        const int index = ComboIndex(IdNetMode);
        return index >= 0 && index < static_cast<int>(kModes.size()) ? kModes[static_cast<std::size_t>(index)]
                                                                     : NetworkMode::Nat;
    }

    void RefreshForwards()
    {
        HWND list = Item(IdForwards);
        SendMessageW(list, LB_RESETCONTENT, 0, 0);
        for (const auto& forward : forwards_) {
            const std::wstring text = std::wstring(forward.udp ? L"UDP" : L"TCP") + L"    127.0.0.1:" +
                                      std::to_wstring(forward.host) + L"   →   invité:" + std::to_wstring(forward.guest);
            SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
        }
    }

    // Fields that depend on the attachment mode, and the explanation.
    void UpdateNetworkState()
    {
        const bool onPage = page_ == kPageNetwork;
        const bool enabled = Checked(IdNetwork);
        const auto mode = SelectedMode();
        const bool forwards = mode == NetworkMode::Nat || mode == NetworkMode::HostOnly;
        for (const int id : {IdNetMode, IdMac, IdMacGenerate, IdBridge, IdIntnet}) EnableWindow(Item(id), enabled);
        SetText(IdNetNameLabel, mode == NetworkMode::Bridged ? L"Carte de l'hôte :" : L"Nom du réseau :");
        Show(IdNetNameLabel, onPage && (mode == NetworkMode::Bridged || mode == NetworkMode::Internal));
        Show(IdBridge, onPage && mode == NetworkMode::Bridged);
        Show(IdIntnet, onPage && mode == NetworkMode::Internal);
        for (const int id : {IdForwardTitle, IdForwards, IdForwardRemove, IdForwardProto, IdForwardHost, IdForwardArrow,
                             IdForwardGuest, IdForwardAdd}) {
            Show(id, onPage && forwards);
            EnableWindow(Item(id), enabled);
        }

        std::wstring info;
        COLORREF infoColor = color::kSecondary;
        if (!enabled) {
            info = L"La machine n'a pas de carte réseau.";
        } else {
            switch (mode) {
            case NetworkMode::Nat:
                info = L"La machine partage la connexion de l'hôte : adresse 10.0.2.15 par DHCP, accès à Internet. "
                       L"Elle n'est pas joignable depuis le réseau local, sauf par les redirections de ports.";
                break;
            case NetworkMode::Bridged: {
                const int index = ComboIndex(IdBridge);
                const bool wireless = index >= 0 && index < static_cast<int>(adapters_.size()) &&
                                      adapters_[static_cast<std::size_t>(index)].wireless;
                if (!NpcapAvailable()) {
                    info = L"Npcap est requis pour l'accès par pont : installe-le depuis https://npcap.com.";
                    infoColor = color::kOrange;
                } else if (adapters_.empty()) {
                    info = L"Aucune carte Ethernet ou Wi-Fi trouvée sur l'hôte.";
                    infoColor = color::kOrange;
                } else if (wireless) {
                    info = L"Wi-Fi : selon le pilote et le réseau, l'injection de trames par Npcap peut être refusée, "
                           L"et le pont échoue alors (voir vmm.log). Si ça marche, la machine emprunte la MAC de l'hôte "
                           L"(IPv4 seulement). Préfère une carte Ethernet, ou le NAT.";
                    infoColor = color::kOrange;
                } else {
                    info = L"La machine apparaît sur le réseau local comme un autre ordinateur, avec sa propre adresse "
                           L"MAC et l'adresse IP donnée par le routeur.";
                }
                break;
            }
            case NetworkMode::Internal:
                info = L"Les machines reliées au même nom de réseau communiquent entre elles, sans l'hôte ni Internet. "
                       L"Il n'y a pas de DHCP : donne-leur des adresses fixes (ex. 192.168.100.1, .2…).";
                break;
            case NetworkMode::HostOnly:
                info = L"La machine est isolée : ni Internet ni réseau local. L'hôte l'atteint par les redirections "
                       L"de ports (ex. SSH : TCP 2222 → 22, puis « ssh -p 2222 utilisateur@localhost »).";
                break;
            default:
                info = L"La carte est présente mais son câble est débranché : l'invité la voit sans connexion.";
                break;
            }
        }
        SetText(IdNetInfo, info);
        SetColor(IdNetInfo, infoColor);
    }

    void AddForward()
    {
        const auto host = std::wcstoul(TextOf(IdForwardHost).c_str(), nullptr, 10);
        const auto guest = std::wcstoul(TextOf(IdForwardGuest).c_str(), nullptr, 10);
        if (host == 0 || host > 65535) {
            Fail(kPageNetwork, IdForwardHost, L"Port hôte entre 1 et 65535.");
            return;
        }
        if (guest == 0 || guest > 65535) {
            Fail(kPageNetwork, IdForwardGuest, L"Port invité entre 1 et 65535.");
            return;
        }
        const bool udp = ComboIndex(IdForwardProto) == 1;
        for (const auto& forward : forwards_) {
            if (forward.udp == udp && forward.host == host) {
                Fail(kPageNetwork, IdForwardHost, L"Ce port de l'hôte est déjà redirigé.");
                return;
            }
        }
        forwards_.push_back({udp, static_cast<unsigned short>(host), static_cast<unsigned short>(guest)});
        RefreshForwards();
        SetText(IdForwardHost, L"");
        SetText(IdForwardGuest, L"");
        SetMessage(L"", false);
        SetFocus(Item(IdForwardHost));
    }

    void Command(int id, int code) override
    {
        switch (id) {
        case IdCategories:
            if (code == LBN_SELCHANGE) {
                const auto page = static_cast<int>(SendMessageW(Item(IdCategories), LB_GETCURSEL, 0, 0));
                if (page >= 0 && page < kPageCount && page != page_) {
                    ShowPage(page);
                    OnPageShown(page);
                }
            }
            break;
        case IdOpenFolder:
            ShellExecuteW(window_, L"open", vm_.directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case IdGpu:
            if (code == CBN_SELCHANGE) UpdateDisplayState();
            break;
        case IdBrowseDisk: {
            const auto path = BrowseFile(window_, L"Disque brut (*.img)\0*.img\0Tous les fichiers\0*.*\0\0",
                                         L"Disque virtuel", true, L"img", TextOf(IdDisk));
            if (!path.empty()) {
                SetText(IdDisk, path);
                UpdateDiskInfo();
            }
            break;
        }
        case IdDisk:
            if (code == EN_KILLFOCUS) UpdateDiskInfo();
            break;
        case IdBrowseIso: {
            const auto path = BrowseFile(window_, L"Images ISO (*.iso)\0*.iso\0Tous les fichiers\0*.*\0\0",
                                         L"Choisir une image ISO", false, nullptr, TextOf(IdIso));
            if (!path.empty()) {
                SetText(IdIso, path);
                UpdateIsoInfo();
            }
            break;
        }
        case IdIso:
            if (code == EN_KILLFOCUS) UpdateIsoInfo();
            break;
        case IdEject:
            SetText(IdIso, L"");
            UpdateIsoInfo();
            break;
        case IdShareBrowse:
        case IdShareBrowse + 1:
        case IdShareBrowse + 2: {
            const int index = id - IdShareBrowse;
            const auto path = BrowseFolder(window_, L"Dossier à partager avec la machine", TextOf(IdSharePath + index));
            if (!path.empty()) {
                SetText(IdSharePath + index, path);
                if (TextOf(IdShareTag + index).empty()) SetText(IdShareTag + index, SuggestTag(path, index));
            }
            break;
        }
        case IdBrowsePreseed: {
            const auto path = BrowseFile(window_, L"Preseed Debian (*.cfg)\0*.cfg\0Tous les fichiers\0*.*\0\0",
                                         L"Fichier preseed", false, nullptr, TextOf(IdPreseed));
            if (!path.empty()) SetText(IdPreseed, path);
            break;
        }
        case IdNetwork:
            if (code == BN_CLICKED) UpdateNetworkState();
            break;
        case IdNetMode:
        case IdBridge:
            if (code == CBN_SELCHANGE) UpdateNetworkState();
            break;
        case IdMacGenerate:
            SetText(IdMac, RandomMac());
            break;
        case IdForwardAdd:
            AddForward();
            break;
        case IdForwardRemove: {
            const auto index = static_cast<int>(SendMessageW(Item(IdForwards), LB_GETCURSEL, 0, 0));
            if (index >= 0 && index < static_cast<int>(forwards_.size())) {
                forwards_.erase(forwards_.begin() + index);
                RefreshForwards();
            }
            break;
        }
        default:
            break;
        }
    }

    void UpdateDisplayState()
    {
        const bool gpu = ComboIndex(IdGpu) == 0;
        const bool virgl = VirglAvailable();
        EnableWindow(Item(Id3d), gpu && virgl);
        if (!gpu) {
            SetText(Id3dStatus, L"Le framebuffer simple n'a pas d'accélération 3D.");
            SetColor(Id3dStatus, color::kSecondary);
        } else if (virgl) {
            SetText(Id3dStatus, L"virglrenderer est installé : rendu OpenGL par le GPU de l'hôte.");
            SetColor(Id3dStatus, color::kGreen);
        } else {
            SetText(Id3dStatus, L"virglrenderer est absent : installe mingw-w64-ucrt-x86_64-virglrenderer (MSYS2) "
                                L"puis relance build.ps1. La machine reste en 2D.");
            SetColor(Id3dStatus, color::kOrange);
        }
    }

    fs::path DiskPath() const
    {
        const auto text = TextOf(IdDisk);
        return text.empty() ? vm_.directory / L"disk.img" : fs::path(text);
    }

    void UpdateDiskInfo()
    {
        auto* slider = SliderById(IdDiskSize);
        const auto path = DiskPath();
        WIN32_FILE_ATTRIBUTE_DATA data{};
        const bool exists = GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) &&
                            (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
        std::uint64_t bytes = 0;
        std::uint64_t allocated = 0;
        if (exists) {
            bytes = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            DWORD high = 0;
            const DWORD low = GetCompressedFileSizeW(path.c_str(), &high);
            if (low != INVALID_FILE_SIZE || GetLastError() == NO_ERROR) allocated = (static_cast<std::uint64_t>(high) << 32) | low;
        }
        const auto free = FreeSpace(path);
        existingGiB_ = exists ? static_cast<unsigned>((bytes + kGiB - 1) / kGiB) : 0;
        // A raw disk can grow, never shrink.
        const unsigned minimum = exists ? (std::max)(2u, existingGiB_) : 2u;
        slider->spec.green = 0;
        slider->spec.hint = nullptr;
        const auto room = (free + allocated) / kGiB;
        if (room != 0) {
            const auto spec = DiskSpec(minimum, room);
            slider->spec.green = spec.green;
            slider->spec.orange = spec.orange;
            slider->spec.hint = spec.hint;
        }
        slider->SetRange(minimum, 4096);
        if (exists && slider->Value() < existingGiB_) slider->Set(existingGiB_);
        std::wstring info;
        if (exists) {
            info = L"Fichier existant : " + FormatBytes(bytes) + L", dont " + FormatBytes(allocated) +
                   L" occupés sur l'hôte. La taille peut seulement augmenter : agrandis ensuite la partition dans "
                   L"l'invité.";
        } else {
            info = L"Fichier dynamique créé au premier démarrage : il n'occupe que l'espace réellement utilisé"
                   L" (" + FormatBytes(free) + L" libres sur ce lecteur).";
        }
        SetText(IdDiskInfo, info);
        InvalidateRect(window_, nullptr, TRUE);
    }

    void UpdateIsoInfo()
    {
        const auto iso = TextOf(IdIso);
        if (iso.empty()) {
            SetText(IdIsoInfo, L"Lecteur vide");
            SetColor(IdIsoInfo, color::kSecondary);
        } else if (!PathExists(iso)) {
            SetText(IdIsoInfo, L"Fichier introuvable");
            SetColor(IdIsoInfo, color::kRed);
        } else {
            const auto os = DetectOsFromIso(iso);
            SetText(IdIsoInfo, os.empty() ? L"Système non reconnu par son nom" : L"Système détecté : " + os);
            SetColor(IdIsoInfo, os.empty() ? color::kSecondary : color::kGreen);
            if (!os.empty() && TextOf(IdOs).empty()) SetText(IdOs, os);
        }
    }

    bool Commit() override
    {
        VmConfig c = vm_.config;
        std::wstring error;
        c.name = TextOf(IdName);
        if (!ValidName(c.name, error)) {
            Fail(kPageGeneral, IdName, error);
            return false;
        }
        for (const auto& other : library_) {
            if (other.directory != vm_.directory && _wcsicmp(other.config.name.c_str(), c.name.c_str()) == 0) {
                Fail(kPageGeneral, IdName, L"Une autre machine porte déjà ce nom.");
                return false;
            }
        }
        c.os = TextOf(IdOs);

        unsigned value = 0;
        if (!SliderById(IdMemory)->Parse(value)) {
            Fail(kPageSystem, IdMemory, L"Mémoire vive invalide.");
            return false;
        }
        c.memoryMiB = value;
        if (!SliderById(IdCpus)->Parse(value)) {
            Fail(kPageSystem, IdCpus, L"Nombre de processeurs invalide.");
            return false;
        }
        c.cpus = value;
        const int boot = ComboIndex(IdBoot);
        c.boot = boot == 1 ? BootOrder::Cdrom : boot == 2 ? BootOrder::Disk : BootOrder::Auto;
        c.rtcLocal = ComboIndex(IdRtc) == 1;

        c.virtioGpu = ComboIndex(IdGpu) == 0;
        const int resolution = ComboIndex(IdResolution);
        if (resolution >= 0 && resolution < static_cast<int>(resolutions_.size())) c.resolution = resolutions_[resolution];
        c.gpu3d = Checked(Id3d);

        c.disk = DiskPath();
        if (!SliderById(IdDiskSize)->Parse(value)) {
            Fail(kPageStorage, IdDiskSize, L"Taille de disque invalide.");
            return false;
        }
        c.diskGiB = value;
        c.diskBus = ComboIndex(IdDiskBus) == 1 ? DiskBus::Ide : DiskBus::Virtio;
        c.iso = TextOf(IdIso);
        if (!c.iso.empty() && !PathExists(c.iso)) {
            Fail(kPageStorage, IdIso, L"L'image ISO indiquée est introuvable.");
            return false;
        }
        c.fullscreen = Checked(IdFullscreen);
        c.audio = Checked(IdAudio);
        c.microphone = Checked(IdMicrophone);
        c.network = Checked(IdNetwork) ? SelectedMode() : NetworkMode::None;
        c.mac = TextOf(IdMac);
        if (!ValidMac(c.mac)) {
            Fail(kPageNetwork, IdMac, L"Adresse MAC invalide : six octets comme 52:54:00:12:34:56 (premier octet pair).");
            return false;
        }
        if (c.network == NetworkMode::Bridged) {
            const int index = ComboIndex(IdBridge);
            if (index < 0 || index >= static_cast<int>(adapters_.size())) {
                Fail(kPageNetwork, IdBridge, L"Choisis la carte réseau de l'hôte pour l'accès par pont.");
                return false;
            }
            c.bridgeAdapter = adapters_[static_cast<std::size_t>(index)].name;
        }
        c.internalNetwork = TextOf(IdIntnet);
        if (c.internalNetwork.empty()) c.internalNetwork = L"intnet";
        c.forwards = forwards_;
        c.graphicalInstaller = ComboIndex(IdInstaller) == 1;
        c.preseed = TextOf(IdPreseed);
        if (!c.preseed.empty() && !PathExists(c.preseed)) {
            Fail(kPageInstall, IdPreseed, L"Le fichier preseed indiqué est introuvable.");
            return false;
        }
        for (int index = 0; index < static_cast<int>(kMaxShares); ++index) {
            auto& share = c.shares[static_cast<std::size_t>(index)];
            share.path = TextOf(IdSharePath + index);
            share.tag = TextOf(IdShareTag + index);
            share.readOnly = Checked(IdShareReadOnly + index);
            if (share.path.empty()) continue;
            if (!PathExists(share.path) || !fs::is_directory(share.path)) {
                Fail(kPageShares, IdSharePath + index, L"Ce dossier partagé est introuvable.");
                return false;
            }
            std::wstring tagError;
            if (!ValidTag(share.tag, tagError)) {
                Fail(kPageShares, IdShareTag + index, tagError);
                return false;
            }
            for (int other = 0; other < index; ++other) {
                const auto& previous = c.shares[static_cast<std::size_t>(other)];
                if (!previous.path.empty() && previous.tag == share.tag) {
                    Fail(kPageShares, IdShareTag + index, L"Deux dossiers partagés ne peuvent pas porter le même nom.");
                    return false;
                }
            }
        }
        c.guestTools = Checked(IdGuestTools);
        if (c.os.empty()) c.os = DetectOsFromIso(c.iso);
        result_ = c;
        return true;
    }

    const VmEntry& vm_;
    const std::vector<VmEntry>& library_;
    VmConfig& result_;
    std::vector<std::wstring> resolutions_;
    std::vector<HostAdapterInfo> adapters_;
    std::vector<PortForward> forwards_;
    unsigned existingGiB_ = 0;
};

// --- New VM ------------------------------------------------------------------------------

class NewVmDialog final : public Form {
public:
    NewVmDialog(const fs::path& library, const std::vector<VmEntry>& existing, NewVmRequest& request)
        : library_(library), existing_(existing), request_(request)
    {
    }

private:
    static constexpr int kWidth = 660;
    static constexpr int kHeight = 700;
    static constexpr int kLeft = 28;
    static constexpr int kField = 176;
    static constexpr int kRight = kWidth - 28;
    static constexpr int kFieldWidth = kRight - kField;

    bool NameTaken(const std::wstring& name) const
    {
        for (const auto& vm : existing_) {
            if (_wcsicmp(vm.config.name.c_str(), name.c_str()) == 0) return true;
        }
        return PathExists(library_ / name);
    }

    std::wstring UniqueName(const std::wstring& base) const
    {
        std::wstring name = base;
        for (int index = 2; NameTaken(name); ++index) name = base + L" (" + std::to_wstring(index) + L")";
        return name;
    }

    void Build() override
    {
        const auto& host = Host();
        AddStatic(-1, L"Nouvelle machine virtuelle", kLeft, 18, kRight - kLeft, 36, ui::Font().title, color::kText);
        AddStatic(-1,
                  L"Donne un nom à la machine et choisis l'image ISO d'installation. Le disque virtuel est créé "
                  L"au premier démarrage et n'occupe que l'espace réellement utilisé.",
                  kLeft, 58, kRight - kLeft, 40, ui::Font().caption, color::kSecondary);

        AddLabel(-1, L"Nom :", kLeft, 108);
        settingName_ = true;
        AddEdit(-1, IdName, UniqueName(L"Nouvelle VM"), kField, 108, kFieldWidth);
        settingName_ = false;
        AddLabel(-1, L"Image ISO :", kLeft, 148);
        AddEdit(-1, IdIso, L"", kField, 148, kFieldWidth - 108);
        SendMessageW(Item(IdIso), EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"debian-13-amd64-netinst.iso…"));
        AddButton(-1, IdBrowseIso, L"Parcourir…", kRight - 100, 146, 100);
        AddStatic(-1, L"Debian, Ubuntu, Fedora, Arch, openSUSE… (ISO Linux 64 bits)", kField, 180, kFieldWidth, 20,
                  ui::Font().caption, color::kSecondary, SS_LEFT, IdIsoInfo);

        AddHeading(-1, L"Matériel", kLeft, 214, kRight - kLeft);
        const unsigned memory = std::clamp(host.memoryMiB / 4 / 512 * 512, 1024u, 4096u);
        AddLabel(-1, L"Mémoire vive :", kLeft, 248);
        AddSlider(-1, IdMemory, kField, 246, kFieldWidth, MemorySpec(), memory);
        AddLabel(-1, L"Processeurs :", kLeft, 330);
        AddSlider(-1, IdCpus, kField, 328, kFieldWidth, CpuSpec(), (std::min)(2u, host.logicalCpus));

        AddHeading(-1, L"Disque dur", kLeft, 414, kRight - kLeft);
        AddLabel(-1, L"Taille :", kLeft, 448);
        AddSlider(-1, IdDiskSize, kField, 446, kFieldWidth, DiskSpec(2, FreeSpace(library_) / kGiB), 25);

        AddHeading(-1, L"Options", kLeft, 532, kRight - kLeft);
        const int column = kLeft + (kRight - kLeft) / 2;
        AddCheck(-1, Id3d, L"Accélération 3D (OpenGL)", kLeft, 564, column - kLeft - 8);
        AddCheck(-1, IdAudio, L"Son (sortie vers Windows)", column, 564, kRight - column);
        AddCheck(-1, IdNetwork, L"Réseau NAT (accès Internet)", kLeft, 592, column - kLeft - 8);
        AddCheck(-1, IdInstaller, L"Installateur Debian graphique", column, 592, kRight - column);
        const bool virgl = VirglAvailable();
        SetChecked(Id3d, virgl);
        EnableWindow(Item(Id3d), virgl);
        SetChecked(IdAudio, true);
        SetChecked(IdNetwork, true);
        SetChecked(IdInstaller, false); // The GTK installer still falls back to text mode (see docs).

        AddStatic(-1, L"", kLeft, kHeight - 86, kRight - kLeft, 20, ui::Font().caption, color::kRed, SS_LEFT, IdMessage);
        AddCheck(-1, IdStartNow, L"Démarrer la machine après sa création", kLeft, kHeight - 44, 300);
        SetChecked(IdStartNow, true);
        AddButton(-1, IDOK, L"Créer", kRight - 212, kHeight - 46, 100, 32, true);
        AddButton(-1, IDCANCEL, L"Annuler", kRight - 100, kHeight - 46, 100, 32);
    }

    void Focus() override
    {
        SetFocus(Item(IdName));
        SendMessageW(Item(IdName), EM_SETSEL, 0, -1);
    }

    void PaintBackground(HDC dc, const RECT& client) override
    {
        ui::Fill(dc, RECT{0, client.bottom - Scale(60), client.right, client.bottom - Scale(59)}, color::kBorder);
    }

    void Command(int id, int code) override
    {
        switch (id) {
        case IdName:
            if (code == EN_CHANGE && !settingName_) nameEdited_ = true;
            break;
        case IdBrowseIso: {
            const auto path = BrowseFile(window_, L"Images ISO (*.iso)\0*.iso\0Tous les fichiers\0*.*\0\0",
                                         L"Choisir une image ISO", false, nullptr, TextOf(IdIso));
            if (!path.empty()) {
                SetText(IdIso, path);
                UpdateIso();
            }
            break;
        }
        case IdIso:
            if (code == EN_KILLFOCUS) UpdateIso();
            break;
        default:
            break;
        }
    }

    void UpdateIso()
    {
        const auto iso = TextOf(IdIso);
        if (iso.empty()) return;
        if (!PathExists(iso)) {
            SetText(IdIsoInfo, L"Fichier introuvable");
            SetColor(IdIsoInfo, color::kRed);
            return;
        }
        os_ = DetectOsFromIso(iso);
        SetText(IdIsoInfo, os_.empty() ? L"Système non reconnu par son nom : les profils connus seront essayés."
                                       : L"Système détecté : " + os_);
        SetColor(IdIsoInfo, os_.empty() ? color::kSecondary : color::kGreen);
        if (!nameEdited_ && !os_.empty()) {
            settingName_ = true;
            SetText(IdName, UniqueName(os_));
            settingName_ = false;
        }
        // The Debian installer options only make sense for Debian ISOs.
        EnableWindow(Item(IdInstaller), os_.empty() || os_.rfind(L"Debian", 0) == 0);
    }

    bool Commit() override
    {
        std::wstring error;
        const auto name = TextOf(IdName);
        if (!ValidName(name, error)) {
            Fail(-1, IdName, error);
            return false;
        }
        if (NameTaken(name)) {
            Fail(-1, IdName, L"Une machine ou un dossier porte déjà ce nom.");
            return false;
        }
        const auto iso = TextOf(IdIso);
        if (iso.empty()) {
            Fail(-1, IdIso, L"Choisis l'image ISO d'installation.");
            return false;
        }
        if (!PathExists(iso)) {
            Fail(-1, IdIso, L"L'image ISO indiquée est introuvable.");
            return false;
        }
        unsigned memory = 0;
        unsigned cpus = 0;
        unsigned disk = 0;
        if (!SliderById(IdMemory)->Parse(memory)) {
            Fail(-1, IdMemory, L"Mémoire vive invalide.");
            return false;
        }
        if (!SliderById(IdCpus)->Parse(cpus)) {
            Fail(-1, IdCpus, L"Nombre de processeurs invalide.");
            return false;
        }
        if (!SliderById(IdDiskSize)->Parse(disk)) {
            Fail(-1, IdDiskSize, L"Taille de disque invalide.");
            return false;
        }
        VmConfig c;
        c.name = name;
        c.os = DetectOsFromIso(iso);
        c.iso = iso;
        c.memoryMiB = memory;
        c.cpus = cpus;
        c.diskGiB = disk;
        c.disk = library_ / name / L"disk.img";
        c.gpu3d = Checked(Id3d) || !VirglAvailable(); // Without virglrenderer: enabled once it is installed.
        c.audio = Checked(IdAudio);
        c.network = Checked(IdNetwork) ? NetworkMode::Nat : NetworkMode::None;
        c.mac = RandomMac(); // Unique per machine: several VMs can share a network.
        c.graphicalInstaller = Checked(IdInstaller) && IsWindowEnabled(Item(IdInstaller));
        request_.config = c;
        request_.startNow = Checked(IdStartNow);
        return true;
    }

    const fs::path& library_;
    const std::vector<VmEntry>& existing_;
    NewVmRequest& request_;
    std::wstring os_;
    bool nameEdited_ = false;
    bool settingName_ = false;
};

} // namespace

// --- Snapshots ---------------------------------------------------------------------------

// Name and kind of a clone.
class CloneDialog final : public Form {
public:
    CloneDialog(std::wstring origin, std::wstring suggestedName, CloneRequest& request)
        : origin_(std::move(origin)), suggestedName_(std::move(suggestedName)), request_(request) {}

private:
    static constexpr int kWidth = 560;
    static constexpr int kHeight = 330;
    static constexpr int kLeft = 24;
    static constexpr int kField = 150;
    static constexpr int kRight = kWidth - 24;

    void Build() override
    {
        AddStatic(-1, origin_, kLeft, 20, kRight - kLeft, 20, ui::Font().caption, color::kSecondary);
        AddLabel(-1, L"Nom :", kLeft, 60, 120);
        AddEdit(-1, IdCloneName, suggestedName_, kField, 58, kRight - kField);
        AddLabel(-1, L"Type :", kLeft, 102, 120);
        AddCombo(-1, IdCloneKind, {L"Clone complet (indépendant)", L"Clone lié (léger, dépend de la source)"}, kField, 100,
                 kRight - kField);
        SetComboIndex(IdCloneKind, 0);
        AddStatic(-1, L"", kLeft, 142, kRight - kLeft, 76, ui::Font().caption, color::kSecondary, SS_LEFT, IdSnapshotDetail);
        AddStatic(-1, L"", kLeft, kHeight - 86, kRight - kLeft, 34, ui::Font().caption, color::kRed, SS_LEFT, IdMessage);
        AddButton(-1, IDOK, L"Cloner", kRight - 212, kHeight - 46, 100, 32, true);
        AddButton(-1, IDCANCEL, L"Annuler", kRight - 100, kHeight - 46, 100, 32);
        Explain();
    }

    void Focus() override
    {
        SetFocus(Item(IdCloneName));
        SendMessageW(Item(IdCloneName), EM_SETSEL, 0, -1);
    }

    void Command(int id, int code) override
    {
        if (id == IdCloneKind && code == CBN_SELCHANGE) Explain();
    }

    void Explain()
    {
        SetText(IdSnapshotDetail,
                ComboIndex(IdCloneKind) == 1
                    ? L"Le clone ne stocke que ses propres modifications : il est créé tout de suite et prend peu de place, "
                      L"mais il lit le disque de la machine source. Tant qu'il existe, la source ne peut être supprimée, "
                      L"ni l'instantané dont il part."
                    : L"Copie complète et indépendante du disque, sans l'historique des instantanés. Seuls les blocs "
                      L"utilisés sont écrits ; la copie peut prendre un moment pour un gros disque.");
    }

    bool Commit() override
    {
        std::wstring error;
        const auto name = TextOf(IdCloneName);
        if (!ValidName(name, error)) {
            Fail(-1, IdCloneName, error);
            return false;
        }
        request_.name = name;
        request_.linked = ComboIndex(IdCloneKind) == 1;
        return true;
    }

    std::wstring origin_;
    std::wstring suggestedName_;
    CloneRequest& request_;
};

namespace {

// Modal window for a long operation run on a thread: caption, percentage bar
// and Annuler. Annuler, Escape or the close box ask the operation to stop;
// the window closes once the thread has returned (and cleaned up).
class ProgressDialog final : public Form {
public:
    using Work = std::function<bool(const Progress&)>;
    ProgressDialog(std::wstring caption, Work work) : caption_(std::move(caption)), work_(std::move(work)) {}
    ~ProgressDialog() override
    {
        cancel_ = true;
        if (worker_.joinable()) worker_.join();
    }
    bool Cancelled() const noexcept { return cancel_; }

    static constexpr int kWidth = 480;
    static constexpr int kHeight = 176;

private:
    static constexpr int kLeft = 24;
    static constexpr int kRight = kWidth - 24;
    static constexpr int kBarTop = 64;
    static constexpr UINT_PTR kTimer = 1;

    void Build() override
    {
        AddStatic(-1, caption_, kLeft, 20, kRight - kLeft, 36, ui::Font().body, color::kText);
        AddStatic(-1, L"Préparation…", kLeft, kBarTop + 18, kRight - kLeft, 20, ui::Font().caption, color::kSecondary, SS_LEFT,
                  IdMessage);
        AddButton(-1, IDCANCEL, L"Annuler", kRight - 100, kHeight - 46, 100, 32);
    }

    void Focus() override
    {
        if (worker_.joinable()) return;
        SetFocus(Item(IDCANCEL));
        worker_ = std::thread([this] {
            ok_ = work_([this](std::uint64_t done, std::uint64_t total) {
                doneCount_ = done;
                totalCount_ = total;
                return !cancel_.load();
            });
            finished_ = true;
        });
        SetTimer(window_, kTimer, 100, nullptr);
    }

    void Timer(UINT_PTR) override
    {
        if (finished_) {
            KillTimer(window_, kTimer);
            worker_.join();
            Close(ok_);
            return;
        }
        if (cancel_) return;
        const std::uint64_t total = totalCount_;
        const auto permille = total == 0 ? 0u : static_cast<unsigned>((std::min)(doneCount_.load(), total) * 1000 / total);
        if (permille != permille_) {
            permille_ = permille;
            SetMessage(std::to_wstring(permille / 10) + L" %", false);
            RECT bar{Scale(kLeft), Scale(kBarTop), Scale(kRight), Scale(kBarTop + 8)};
            InvalidateRect(window_, &bar, FALSE);
        }
    }

    bool CanClose() override
    {
        if (finished_) return true;
        cancel_ = true;
        EnableWindow(Item(IDCANCEL), FALSE);
        SetMessage(L"Annulation…", false);
        return false; // Closed by Timer once the thread has cleaned up.
    }

    void PaintBackground(HDC dc, const RECT&) override
    {
        RECT track{Scale(kLeft), Scale(kBarTop), Scale(kRight), Scale(kBarTop + 8)};
        ui::RoundedRect(dc, track, Scale(4), color::kBorder);
        if (permille_ > 0) {
            RECT fill = track;
            fill.right = fill.left + static_cast<int>(static_cast<long long>(track.right - track.left) * permille_ / 1000);
            if (fill.right - fill.left >= Scale(8)) ui::RoundedRect(dc, fill, Scale(4), color::kAccent);
        }
    }

    bool Commit() override { return false; } // No OK button: Enter does nothing.

    std::wstring caption_;
    Work work_;
    std::thread worker_;
    std::atomic_bool cancel_ = false;
    std::atomic_bool finished_ = false;
    std::atomic<std::uint64_t> doneCount_ = 0;
    std::atomic<std::uint64_t> totalCount_ = 0;
    bool ok_ = false; // Written by the thread before `finished_`.
    unsigned permille_ = 0;
};

// Import: what the file holds, and the new machine's name.
class ImportDialog final : public Form {
public:
    ImportDialog(std::wstring summary, std::wstring suggestedName, const fs::path& library, const std::vector<VmEntry>& vms,
                 std::wstring& name)
        : summary_(std::move(summary)), suggestedName_(std::move(suggestedName)), library_(library), vms_(vms), name_(name) {}

    static constexpr int kWidth = 560;
    static constexpr int kHeight = 300;

private:
    static constexpr int kLeft = 24;
    static constexpr int kField = 150;
    static constexpr int kRight = kWidth - 24;

    void Build() override
    {
        AddStatic(-1, summary_, kLeft, 20, kRight - kLeft, 40, ui::Font().caption, color::kSecondary);
        AddLabel(-1, L"Nom :", kLeft, 76, 120);
        AddEdit(-1, IdCloneName, suggestedName_, kField, 74, kRight - kField);
        AddStatic(-1,
                  L"La machine reçoit une nouvelle adresse MAC et un nouvel identifiant : elle peut tourner à côté de "
                  L"celle d'origine. Le disque est recréé sans l'historique des instantanés ; seuls les blocs utilisés "
                  L"prennent de la place.",
                  kLeft, 118, kRight - kLeft, 60, ui::Font().caption, color::kSecondary);
        AddStatic(-1, L"", kLeft, kHeight - 86, kRight - kLeft, 34, ui::Font().caption, color::kRed, SS_LEFT, IdMessage);
        AddButton(-1, IDOK, L"Importer", kRight - 212, kHeight - 46, 100, 32, true);
        AddButton(-1, IDCANCEL, L"Annuler", kRight - 100, kHeight - 46, 100, 32);
    }

    void Focus() override
    {
        SetFocus(Item(IdCloneName));
        SendMessageW(Item(IdCloneName), EM_SETSEL, 0, -1);
    }

    bool Commit() override
    {
        std::wstring error;
        const auto name = TextOf(IdCloneName);
        if (!CheckNewVmName(name, library_, vms_, error)) {
            Fail(-1, IdCloneName, error);
            return false;
        }
        name_ = name;
        return true;
    }

    std::wstring summary_;
    std::wstring suggestedName_;
    const fs::path& library_;
    const std::vector<VmEntry>& vms_;
    std::wstring& name_;
};

void ShowError(HWND owner, const wchar_t* title, const std::wstring& text)
{
    MessageBoxW(owner, text.c_str(), title, MB_OK | MB_ICONWARNING);
}

} // namespace

std::wstring ShowExportDialog(HWND owner, const VmEntry& vm, bool stopped, std::uint32_t snapshot)
{
    if (snapshot == 0 && !stopped) {
        ShowError(owner, L"Exporter une machine",
                  L"« " + vm.config.name + L" » est allumée : son disque change pendant qu'elle tourne.\n\n"
                  L"Éteins-la, ou exporte un de ses instantanés depuis la fenêtre Instantanés.");
        return {};
    }
    const auto target = BrowseFile(owner, L"Machine Virtualiseur (*.vrtx)\0*.vrtx\0\0", L"Exporter la machine", true, L"vrtx",
                                   vm.config.name + L".vrtx");
    if (target.empty()) return {};
    std::wstring message;
    bool cancelled = false;
    ProgressDialog progress(L"Export de « " + vm.config.name + L" » : compression des blocs utilisés du disque…",
                            [&](const Progress& report) {
                                return ExportVm(vm, stopped, snapshot, target, report, cancelled, message);
                            });
    const bool ok = progress.Run(owner, L"Exporter « " + vm.config.name + L" »", ProgressDialog::kWidth, ProgressDialog::kHeight);
    if (!ok && !cancelled) ShowError(owner, L"Exporter une machine", message);
    return message;
}

std::wstring ShowImportDialog(HWND owner, const fs::path& library, const std::vector<VmEntry>& vms, std::wstring& message)
{
    message.clear();
    const auto archive = BrowseFile(owner, L"Machine Virtualiseur (*.vrtx)\0*.vrtx\0Tous les fichiers\0*.*\0\0",
                                    L"Importer une machine", false, L"vrtx", L"");
    if (archive.empty()) return {};
    ArchivePreview preview;
    std::wstring error;
    if (!PreviewArchive(archive, preview, error)) {
        ShowError(owner, L"Importer une machine", L"Ce fichier ne peut pas être importé.\n\n" + error);
        return {};
    }
    // A free name: "<name>", else "<name> (2)", "(3)"…
    std::wstring suggested = preview.name;
    std::wstring ignored;
    for (int index = 2; !CheckNewVmName(suggested, library, vms, ignored) && index < 100; ++index) {
        suggested = preview.name + L" (" + std::to_wstring(index) + L")";
    }
    std::wstring summary = fs::path(archive).filename().wstring() + L"\n";
    summary += preview.os.empty() ? std::wstring(L"Linux") : preview.os;
    if (preview.memoryMiB != 0) summary += L"  ·  " + FormatNumber(preview.memoryMiB) + L" Mio";
    if (preview.cpus != 0) summary += L", " + std::to_wstring(preview.cpus) + (preview.cpus > 1 ? L" processeurs" : L" processeur");
    summary += L"  ·  disque de " + FormatBytes(preview.diskBytes);
    std::wstring name;
    ImportDialog dialog(summary, suggested, library, vms, name);
    if (!dialog.Run(owner, L"Importer une machine", ImportDialog::kWidth, ImportDialog::kHeight)) return {};

    bool cancelled = false;
    ProgressDialog progress(L"Import de « " + name + L" » : vérification et écriture du disque…", [&](const Progress& report) {
        return ImportVm(archive, name, library, vms, report, cancelled, message);
    });
    if (progress.Run(owner, L"Importer « " + name + L" »", ProgressDialog::kWidth, ProgressDialog::kHeight)) return name;
    if (!cancelled) ShowError(owner, L"Importer une machine", message);
    return {};
}

class SnapshotsDialog final : public Form {
public:
    SnapshotsDialog(const VmEntry& vm, bool stopped, const fs::path& library, const std::vector<VmEntry>& vms)
        : vm_(vm), stopped_(stopped), library_(library), vms_(vms) {}
    int Result() const noexcept { return (changed_ ? kSnapshotsChanged : 0) | (cloned_ ? kLibraryChanged : 0); }

private:
    static constexpr int kWidth = 640;
    static constexpr int kHeight = 540;
    static constexpr int kLeft = 24;
    static constexpr int kRight = kWidth - 24;

    void Build() override
    {
        AddStatic(-1, L"Un instantané fige le disque de la machine (pas sa mémoire) ; restaurer y ramène le disque, "
                      L"et la machine redémarre dans cet état. Supprimer un instantané ne change ni l'état actuel ni "
                      L"les autres instantanés.",
                  kLeft, 18, kRight - kLeft, 40, ui::Font().caption, color::kSecondary);
        // Owner-drawn tree: one colored marker per snapshot (how it was taken).
        Add(-1, IdSnapshotList, WC_LISTBOXW, L"", WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
            kLeft, 64, kRight - kLeft, 250, WS_EX_CLIENTEDGE);
        AddStatic(-1, L"", kLeft, 322, kRight - kLeft, 40, ui::Font().caption, CLR_INVALID, SS_LEFT, IdSnapshotDetail);
        AddLabel(-1, L"Nouvel instantané :", kLeft, 378, 150);
        AddEdit(-1, IdSnapshotName, L"", kLeft + 150, 376, kRight - kLeft - 150 - 150);
        SendMessageW(Item(IdSnapshotName), EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Nom (ex. Avant mise à jour)"));
        AddButton(-1, IDOK, L"Prendre", kRight - 140, 374, 140, 32, true);
        AddButton(-1, IdSnapshotRestore, L"Restaurer", kLeft, 420, 140);
        AddButton(-1, IdSnapshotDelete, L"Supprimer", kLeft + 150, 420, 140);
        AddButton(-1, IdSnapshotClone, L"Cloner…", kLeft + 300, 420, 140);
        AddButton(-1, IdSnapshotExport, L"Exporter…", kLeft + 450, 420, 142);
        AddStatic(-1, L"", kLeft, kHeight - 84, kRight - kLeft, 34, ui::Font().caption, color::kRed, SS_LEFT, IdMessage);
        AddButton(-1, IDCANCEL, L"Fermer", kRight - 100, kHeight - 46, 100, 32);
        AddButton(-1, IdCloneCancel, L"Annuler la copie", kRight - 280, kHeight - 46, 170, 32);
        Reload(0);
        if (!stopped_) {
            SetMessage(L"Machine allumée : « Prendre » gèle d'abord ses systèmes de fichiers (qemu-guest-agent) ; "
                       L"restaurer ou supprimer demande de l'éteindre.", false);
        }
    }

    // Running VM: the VM process owns the disk and takes the snapshot.
    void TakeLive(const std::wstring& name)
    {
        const int length = WideCharToMultiByte(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), nullptr, 0, nullptr, nullptr);
        std::string command = "snapshot-take ";
        const auto offset = command.size();
        command.resize(offset + static_cast<std::size_t>(length));
        WideCharToMultiByte(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), command.data() + offset, length, nullptr, nullptr);
        SetCursor(LoadCursorW(nullptr, IDC_WAIT));
        SetMessage(L"Gel des systèmes de fichiers de la machine…", false);
        UpdateWindow(window_);
        const auto reply = SendCommand(vm_.config.name, command.c_str());
        const auto wide = [](const std::string& text) {
            std::wstring result(text.size(), L'\0');
            result.resize(static_cast<std::size_t>(MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                                                       result.data(), static_cast<int>(result.size()))));
            return result;
        };
        if (!reply || reply->rfind("ok ", 0) != 0) {
            Reload(0);
            SetMessage(reply ? L"Instantané impossible : " + wide(reply->rfind("error: ", 0) == 0 ? reply->substr(7) : *reply)
                             : L"La machine ne répond pas.");
            MessageBeep(MB_ICONWARNING);
            return;
        }
        // "ok <id> <frozen|crash> <detail>"
        std::istringstream fields(reply->substr(3));
        std::uint32_t id = 0;
        std::string state;
        fields >> id >> state;
        std::string detail;
        std::getline(fields, detail);
        if (!detail.empty() && detail.front() == ' ') detail.erase(0, 1);
        changed_ = true;
        SetText(IdSnapshotName, L"");
        Reload(id);
        if (state == "frozen") {
            SetMessage(L"Instantané « " + name + L" » pris, machine allumée (" + wide(detail) + L").", false);
        } else {
            SetMessage(L"Instantané « " + name + L" » pris " + wide(detail) + L".");
        }
    }

    void Focus() override
    {
        ShowWindow(Item(IdCloneCancel), SW_HIDE); // Only during a full clone (ShowPage showed it).
        SetFocus(Item(IdSnapshotName));
    }

    // IDOK: take a snapshot (the window stays open).
    bool Commit() override
    {
        auto name = TextOf(IdSnapshotName);
        if (name.empty()) {
            wchar_t date[32] = {};
            const auto now = std::time(nullptr);
            std::tm local{};
            localtime_s(&local, &now);
            wcsftime(date, 32, L"%d/%m/%Y %H:%M", &local);
            name = L"Instantané du " + std::wstring(date);
        }
        if (!stopped_) {
            TakeLive(name);
            return false;
        }
        Apply([&](virtualiseur::whp::SnapshotStore& store) {
            const auto id = store.Take(name, L"");
            SetText(IdSnapshotName, L"");
            SetMessage(L"Instantané « " + name + L" » pris.", false);
            return id;
        });
        return false;
    }

    void Command(int id, int code) override
    {
        if (id == IdSnapshotList && code == LBN_SELCHANGE) ShowSelection();
        if (id == IdSnapshotClone && code == BN_CLICKED) Clone();
        if (id == IdSnapshotExport && code == BN_CLICKED) {
            // The selected snapshot, or the current state (machine off).
            const auto* snapshot = SelectedSnapshot();
            const auto message = ShowExportDialog(window_, vm_, stopped_, snapshot != nullptr ? snapshot->id : 0);
            if (!message.empty()) SetMessage(message, false);
        }
        if (id == IdCloneCancel && code == BN_CLICKED) cancel_ = true;
        if (id == IdSnapshotRestore && code == BN_CLICKED) {
            const auto* snapshot = SelectedSnapshot();
            if (snapshot == nullptr) return;
            const auto text = L"Restaurer « " + snapshot->name + L" » ?\n\nLe disque reviendra à son état du " + snapshot->date +
                              L" : les modifications faites depuis le dernier instantané seront perdues.";
            if (MessageBoxW(window_, text.c_str(), L"Restaurer un instantané", MB_OKCANCEL | MB_ICONWARNING) != IDOK) return;
            const auto restored = snapshot->id;
            const auto label = snapshot->name;
            Apply([&](virtualiseur::whp::SnapshotStore& store) {
                store.Restore(restored);
                SetMessage(L"Disque ramené à « " + label + L" ».", false);
                return 0u;
            });
        }
        if (id == IdSnapshotDelete && code == BN_CLICKED) {
            const auto* snapshot = SelectedSnapshot();
            if (snapshot == nullptr) return;
            const auto text = L"Supprimer l'instantané « " + snapshot->name +
                              L" » ?\n\nOn ne pourra plus y revenir ; l'état actuel et les autres instantanés ne changent pas. "
                              L"La fusion des couches peut prendre un moment.";
            if (MessageBoxW(window_, text.c_str(), L"Supprimer un instantané", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
            const auto removed = snapshot->id;
            const auto label = snapshot->name;
            Apply([&](virtualiseur::whp::SnapshotStore& store) {
                store.Remove(removed);
                SetMessage(L"Instantané « " + label + L" » supprimé.", false);
                return 0u;
            });
        }
    }

    // Clones the selected snapshot, or the current state.
    void Clone()
    {
        const auto* snapshot = SelectedSnapshot();
        CloneRequest request;
        request.snapshot = snapshot != nullptr ? snapshot->id : 0;
        const auto origin = snapshot != nullptr ? L"Depuis l'instantané « " + snapshot->name + L" » de « " + vm_.config.name + L" »."
                                                : L"Depuis l'état actuel de « " + vm_.config.name + L" »" +
                                                      (stopped_ ? L"." : L" (allumée : un instantané gelé est pris d'abord pour un clone lié).");
        CloneDialog dialog(origin, vm_.config.name + L" - clone", request);
        if (!dialog.Run(window_, L"Cloner « " + vm_.config.name + L" »", 560, 330)) return;

        // The copy runs on a thread; this window keeps painting and shows the progress.
        std::atomic<std::uint64_t> done = 0;
        std::atomic<std::uint64_t> total = 0;
        std::atomic_bool finished = false;
        bool ok = false;
        std::wstring message;
        // Everything but "Annuler la copie" is disabled during the copy.
        std::vector<HWND> disabled;
        for (const int id : std::initializer_list<int>{IdSnapshotList, IdSnapshotName, IDOK, IdSnapshotRestore, IdSnapshotDelete, IdSnapshotClone, IdSnapshotExport, IDCANCEL}) {
            if (HWND control = Item(id); control != nullptr && IsWindowEnabled(control)) {
                EnableWindow(control, FALSE);
                disabled.push_back(control);
            }
        }
        cancel_ = false;
        ShowWindow(Item(IdCloneCancel), SW_SHOW);
        SetMessage(L"Clonage en cours…", false);
        std::thread worker([&] {
            ok = CloneVm(vm_, stopped_, request, library_, vms_,
                         [&](std::uint64_t current, std::uint64_t count) {
                             done = current;
                             total = count;
                             return !cancel_.load(); // false: the copy stops and the clone folder is removed.
                         },
                         message);
            finished = true;
        });
        std::uint64_t shown = ~0ull;
        while (!finished) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
            MSG pending{};
            while (PeekMessageW(&pending, nullptr, 0, 0, PM_REMOVE)) {
                if (!IsDialogMessageW(window_, &pending)) {
                    TranslateMessage(&pending);
                    DispatchMessageW(&pending);
                }
            }
            if (Closing()) cancel_ = true; // Closing the window cancels the copy first.
            if (cancel_) {
                SetMessage(L"Annulation de la copie…", false);
            } else if (total != 0 && done * 100 / total != shown) {
                shown = done * 100 / total;
                SetMessage(L"Copie du disque… " + std::to_wstring(shown) + L" % (" + std::to_wstring(done * 64 / 1024) + L" / " +
                               std::to_wstring(total * 64 / 1024) + L" Mio examinés)",
                           false);
            }
        }
        worker.join();
        ShowWindow(Item(IdCloneCancel), SW_HIDE);
        for (HWND control : disabled) EnableWindow(control, TRUE);
        SetForegroundWindow(window_);
        if (!ok && cancel_) {
            Reload(request.snapshot);
            SetMessage(L"Clonage annulé : rien n'a été créé.", false);
            return;
        }
        if (ok) {
            cloned_ = true;
            changed_ = true; // A linked clone from the current state added a snapshot.
            Reload(request.snapshot);
            SetMessage(message, false);
        } else {
            Reload(request.snapshot);
            SetMessage(message);
            MessageBeep(MB_ICONWARNING);
        }
    }

    template <typename Action>
    void Apply(Action action)
    {
        if (!stopped_) {
            SetMessage(L"Éteins la machine d'abord.");
            return;
        }
        try {
            virtualiseur::whp::SnapshotStore store(vm_.config.disk);
            SetCursor(LoadCursorW(nullptr, IDC_WAIT));
            const auto select = action(store);
            changed_ = true;
            Reload(select);
        } catch (const std::exception& error) {
            const std::string text = error.what();
            std::wstring wide(text.size(), L'\0');
            wide.resize(static_cast<std::size_t>(MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                                                     wide.data(), static_cast<int>(wide.size()))));
            Reload(0);
            SetMessage(wide);
            MessageBeep(MB_ICONWARNING);
        }
    }

    // One tree row: indentation, a marker colored by how the snapshot was
    // taken (indigo: machine off, green: frozen, orange: without freeze),
    // name, then date and state on the right. The current state is accent blue.
    bool DrawItem(const DRAWITEMSTRUCT& item) override
    {
        if (item.CtlID != IdSnapshotList) return false;
        ui::BufferedPaint buffer(item.hDC, item.rcItem);
        const HDC dc = buffer.Dc();
        const bool selected = (item.itemState & ODS_SELECTED) != 0;
        ui::Fill(dc, item.rcItem, selected ? color::kAccentSoft : color::kPanel);
        if (item.itemID >= rows_.size()) return true;
        const auto& row = rows_[item.itemID];
        RECT rect = item.rcItem;
        rect.left += Scale(10) + row.depth * Scale(22);
        rect.right -= Scale(10);
        const int middle = (rect.top + rect.bottom) / 2;
        if (row.depth > 0) {
            // Elbow joining the row to its parent.
            const int x = rect.left - Scale(13);
            ui::Fill(dc, RECT{x, rect.top, x + 1, middle + 1}, color::kBorder);
            ui::Fill(dc, RECT{x, middle, rect.left - Scale(3), middle + 1}, color::kBorder);
        }
        if (row.id == 0) {
            RECT marker{rect.left, middle - Scale(8), rect.left + Scale(16), middle + Scale(8)};
            ui::Text(dc, std::wstring(1, ui::glyph::kPlay), marker, ui::Font().glyph, color::kAccent,
                     DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            RECT text{marker.right + Scale(8), rect.top, rect.right, rect.bottom};
            ui::Text(dc, stopped_ ? L"État actuel" : L"État actuel (machine allumée)", text, ui::Font().bold, color::kAccent);
            return true;
        }
        const auto* snapshot = FindSnapshot(row.id);
        if (snapshot == nullptr) return true;
        const bool frozen = snapshot->state == L"frozen";
        const bool crash = snapshot->state == L"crash";
        const COLORREF tone = frozen ? color::kGreen : crash ? color::kOrange : color::kIndigo;
        RECT dot{rect.left + Scale(3), middle - Scale(5), rect.left + Scale(13), middle + Scale(5)};
        ui::Circle(dc, dot, tone);
        // Right column: state, then date.
        const std::wstring stateText = frozen ? L"allumée, gelée" : crash ? L"allumée, sans gel" : L"";
        RECT date{rect.right - Scale(150), rect.top, rect.right, rect.bottom};
        ui::Text(dc, snapshot->date, date, ui::Font().caption, color::kSecondary, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        int nameRight = date.left - Scale(8);
        if (!stateText.empty()) {
            RECT state{date.left - Scale(130), rect.top, date.left - Scale(10), rect.bottom};
            ui::Text(dc, stateText, state, ui::Font().caption, tone, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            nameRight = state.left - Scale(8);
        }
        RECT name{dot.right + Scale(10), rect.top, nameRight, rect.bottom};
        ui::Text(dc, snapshot->name, name, ui::Font().body, color::kText);
        return true;
    }

    // Tree order: each snapshot under the one it derives from; "État actuel"
    // under the snapshot the current disk comes from.
    void Reload(std::uint32_t select)
    {
        rows_.clear();
        snapshots_.clear();
        std::uint32_t current = 0;
        try {
            const virtualiseur::whp::SnapshotStore store(vm_.config.disk);
            snapshots_ = store.Snapshots();
            current = store.CurrentParent();
        } catch (const std::exception&) {
        }
        const std::function<void(std::uint32_t, int)> add = [&](std::uint32_t parent, int depth) {
            for (const auto& snapshot : snapshots_) {
                if (snapshot.parent != parent) continue;
                rows_.push_back({snapshot.id, depth});
                add(snapshot.id, depth + 1);
            }
            if (parent == current) rows_.push_back({0, depth});
        };
        add(0, 0);
        HWND list = Item(IdSnapshotList);
        SendMessageW(list, LB_RESETCONTENT, 0, 0);
        int selected = -1;
        for (std::size_t index = 0; index < rows_.size(); ++index) {
            const auto& row = rows_[index];
            std::wstring text(static_cast<std::size_t>(row.depth) * 6, L' ');
            if (row.id == 0) {
                text += L"▶  État actuel";
            } else {
                const auto* snapshot = FindSnapshot(row.id);
                text += L"●  " + snapshot->name + L"   —   " + snapshot->date;
                if (snapshot->state == L"frozen") text += L"   (machine allumée)";
                else if (snapshot->state == L"crash") text += L"   (machine allumée, sans gel)";
            }
            SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
            if (row.id == select) selected = static_cast<int>(index);
        }
        if (selected < 0) {
            for (std::size_t index = 0; index < rows_.size(); ++index) {
                if (rows_[index].id == 0) selected = static_cast<int>(index);
            }
        }
        SendMessageW(list, LB_SETCURSEL, static_cast<WPARAM>(selected), 0);
        ShowSelection();
    }

    void ShowSelection()
    {
        const auto* snapshot = SelectedSnapshot();
        std::wstring detail;
        if (snapshot != nullptr) {
            detail = L"« " + snapshot->name + L" », pris le " + snapshot->date + L". ";
            if (!snapshot->description.empty()) detail += snapshot->description;
            else detail += L"Machine éteinte.";
        } else if (snapshots_.empty()) {
            detail = L"Aucun instantané pour l'instant : « Prendre » fige l'état actuel du disque.";
        } else {
            detail = L"État actuel : ce que la machine utilise, construit sur l'instantané au-dessus.";
        }
        SetText(IdSnapshotDetail, detail);
        EnableWindow(Item(IdSnapshotRestore), stopped_ && snapshot != nullptr);
        EnableWindow(Item(IdSnapshotDelete), stopped_ && snapshot != nullptr);
        EnableWindow(Item(IdSnapshotExport), stopped_ || snapshot != nullptr); // Running: snapshots only.
        EnableWindow(Item(IDOK), TRUE); // Running: taken live by the VM.
    }

    const virtualiseur::whp::SnapshotStore::Snapshot* FindSnapshot(std::uint32_t id) const
    {
        for (const auto& snapshot : snapshots_) {
            if (snapshot.id == id) return &snapshot;
        }
        return nullptr;
    }

    const virtualiseur::whp::SnapshotStore::Snapshot* SelectedSnapshot() const
    {
        const auto index = static_cast<int>(SendMessageW(Item(IdSnapshotList), LB_GETCURSEL, 0, 0));
        if (index < 0 || index >= static_cast<int>(rows_.size())) return nullptr;
        return rows_[static_cast<std::size_t>(index)].id == 0 ? nullptr : FindSnapshot(rows_[static_cast<std::size_t>(index)].id);
    }

    struct Row {
        std::uint32_t id = 0; // 0: the current state.
        int depth = 0;
    };
    const VmEntry& vm_;
    bool stopped_ = true;
    bool changed_ = false;
    bool cloned_ = false;
    std::atomic_bool cancel_ = false; // "Annuler la copie" pressed during a full clone.
    const fs::path& library_;
    const std::vector<VmEntry>& vms_;
    std::vector<virtualiseur::whp::SnapshotStore::Snapshot> snapshots_;
    std::vector<Row> rows_;
};

int ShowSnapshotsDialog(HWND owner, const VmEntry& vm, bool stopped, const fs::path& library, const std::vector<VmEntry>& vms)
{
    SnapshotsDialog dialog(vm, stopped, library, vms);
    dialog.Run(owner, vm.config.name + L" — Instantanés", 640, 540);
    return dialog.Result();
}

bool ShowSettingsDialog(HWND owner, const VmEntry& vm, const std::vector<VmEntry>& library, VmConfig& config,
                        int startPage)
{
    SettingsDialog dialog(vm, library, config, startPage);
    return dialog.Run(owner, vm.config.name + L" — Configuration", 840, 590);
}

bool ShowNewVmDialog(HWND owner, const fs::path& library, const std::vector<VmEntry>& existing, NewVmRequest& request)
{
    NewVmDialog dialog(library, existing, request);
    return dialog.Run(owner, L"Nouvelle machine virtuelle", 660, 700);
}

} // namespace manager
