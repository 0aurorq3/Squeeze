#include "engine.hpp"
#include "embedded_font.hpp"
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <commctrl.h>
#include <windowsx.h>
#include <thread>
#include <optional>
#include <memory>
#include <cmath>
#include <algorithm>
#include <array>
#include <sstream>
#include <iomanip>
#include <chrono>

using namespace squeeze;
constexpr UINT WM_ENGINE = WM_APP + 1, WM_NAVIGATE = WM_APP + 2;
constexpr float MARGIN = 32;
enum Hit {
    None,
    Drop,
    Browse,
    Size,
    Unit,
    Quality,
    Balanced,
    Speed,
    Action,
    Folder,
    Minimize,
    Close,
    UnitMB,
    UnitGB,
    About,
    AboutClose,
    HitCount
};
struct Message {
    enum Kind { Loaded, Progressed, Finished, Failed } kind;
    Media media;
    fs::path thumbnail, output;
    Update update;
    std::wstring error;
};
template <class T> static void release(T *&p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}
static D2D1_RECT_F rect(float x, float y, float w, float h) {
    return D2D1::RectF(x, y, x + w, y + h);
}
static bool contains(D2D1_RECT_F r, float x, float y) {
    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}
static float width(D2D1_RECT_F r) {
    return r.right - r.left;
}
static float height(D2D1_RECT_F r) {
    return r.bottom - r.top;
}
static D2D1_COLOR_F color(UINT32 n, float a = 1) {
    return D2D1::ColorF(n, a);
}
static UINT32 blendColor(UINT32 from, UINT32 to, float amount) {
    amount = std::clamp(amount, 0.0f, 1.0f);
    UINT32 result = 0;
    for (int shift : {0, 8, 16}) {
        float channel = float((from >> shift) & 255) +
                        (float((to >> shift) & 255) - float((from >> shift) & 255)) * amount;
        result |= UINT32(std::lround(channel)) << shift;
    }
    return result;
}
static bool spring(float &value, float &velocity, float destination, float dt, bool instant,
                   float frequency = 28) {
    if (instant || (std::abs(value - destination) < 0.001f && std::abs(velocity) < 0.01f)) {
        value = destination;
        velocity = 0;
        return false;
    }
    float offset = value - destination;
    float impulse = (velocity + frequency * offset) * dt;
    float decay = std::exp(-frequency * dt);
    value = destination + (offset + impulse) * decay;
    velocity = (velocity - frequency * impulse) * decay;
    return true;
}
static std::wstring sizeText(uint64_t bytes) {
    std::wostringstream s;
    s << std::fixed << std::setprecision(bytes < 10000000 ? 2 : 1)
      << double(bytes) / (bytes >= 1000000000 ? 1e9 : 1e6) << (bytes >= 1000000000 ? L" GB" : L" MB");
    return s.str();
}
static std::wstring timeText(double seconds) {
    int n = static_cast<int>(std::round(seconds));
    wchar_t b[32]{};
    if (n >= 3600)
        swprintf_s(b, L"%d:%02d:%02d", n / 3600, n / 60 % 60, n % 60);
    else
        swprintf_s(b, L"%d:%02d", n / 60, n % 60);
    return b;
}

class App;
class VideoDrop final : public IDropTarget {
    LONG references = 1;
    App *app;
    bool accepts(IDataObject *data);

  public:
    explicit VideoDrop(App *owner) : app(owner) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **p) override {
        if (!p)
            return E_POINTER;
        *p = nullptr;
        if (id == IID_IUnknown || id == IID_IDropTarget) {
            *p = this;
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return InterlockedIncrement(&references);
    }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG n = InterlockedDecrement(&references);
        if (!n)
            delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject *, DWORD, POINTL, DWORD *) override;
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD *) override;
    HRESULT STDMETHODCALLTYPE DragLeave() override;
    HRESULT STDMETHODCALLTYPE Drop(IDataObject *, DWORD, POINTL, DWORD *) override;
};

class App {
  public:
    HWND hwnd = nullptr, edit = nullptr;
    Engine engine;
    std::atomic_bool shuttingDown = false, cancelOperation = false, cancelDetection = false;
    std::thread detectionThread, worker;
    bool loading = false, busy = false, done = false, dragging = false, unitOpen = false, gb = false,
         keyboardFocus = false, aboutOpen = false;
    Hit hover = None, pressed = None, focus = Browse;
    std::optional<Media> media;
    fs::path incoming, output;
    Preset preset = Preset::Balanced;
    std::wstring error, phase = L"Ready";
    double targetProgress = 0, displayProgress = 0;
    int secondsLeft = -1;
    double encodingFps = 0;
    UINT dpi = 96;
    float w = 940, h = 648;
    float presetX = 0, presetVelocity = 0, unitReveal = 0, unitVelocity = 0;
    std::array<float, 3> presetSelection{0, 1, 0}, presetSelectionVelocity{};
    std::array<float, HitCount> hoverAlpha{};
    std::array<D2D1_RECT_F, HitCount> zones{};
    D2D1_RECT_F left{}, right{}, sizeBox{}, actionBox{};
    HFONT inputFont = nullptr;
    EmbeddedFont uiFont;
    int inputLineHeight = 0;
    ID2D1Factory *factory = nullptr;
    IDWriteFactory *textFactory = nullptr;
    IWICImagingFactory *wic = nullptr;
    ID2D1HwndRenderTarget *target = nullptr;
    ID2D1SolidColorBrush *brush = nullptr;
    ID2D1Bitmap *preview = nullptr;
    ID2D1Bitmap *iconBitmap = nullptr, *markBitmap = nullptr;
    VideoDrop *dropTarget = nullptr;
    std::array<IDWriteTextFormat *, 10> formats{};
    uint64_t startTick = 0;
    std::chrono::steady_clock::time_point lastAnimationTime;
    bool reducedMotion = false;
    ~App() {
        shuttingDown = true;
        cancelOperation = true;
        cancelDetection = true;
        if (worker.joinable())
            worker.join();
        if (detectionThread.joinable())
            detectionThread.join();
        release(preview);
        release(iconBitmap);
        release(markBitmap);
        release(brush);
        release(target);
        for (auto &f : formats)
            release(f);
        release(wic);
        release(textFactory);
        release(factory);
        if (inputFont)
            DeleteObject(inputFont);
        if (dropTarget)
            dropTarget->Release();
    }
    bool initialize() {
        dpi = GetDpiForWindow(hwnd);
        BOOL animate = TRUE;
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animate, 0);
        reducedMotion = !animate;
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &factory);
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown **>(&textFactory));
        if (!uiFont.load(GetModuleHandleW(nullptr), textFactory))
            return false;
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
        const float sizes[]{11, 12, 13, 14, 16, 18, 22, 30, 24, 16};
        if (textFactory)
            for (int i = 0; i < int(formats.size()); i++) {
                auto weight = i == 8   ? DWRITE_FONT_WEIGHT_MEDIUM
                              : i == 9 ? DWRITE_FONT_WEIGHT_NORMAL
                              : i >= 4 ? DWRITE_FONT_WEIGHT_SEMI_BOLD
                                       : DWRITE_FONT_WEIGHT_NORMAL;
                textFactory->CreateTextFormat(
                    uiFont.family.c_str(), uiFont.collection(), weight,
                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, sizes[i], L"en-US", &formats[i]);
                if (formats[i]) {
                    formats[i]->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    formats[i]->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                    DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                    IDWriteInlineObject *ellipsis = nullptr;
                    textFactory->CreateEllipsisTrimmingSign(formats[i], &ellipsis);
                    formats[i]->SetTrimming(&trim, ellipsis);
                    release(ellipsis);
                }
            }
        edit = CreateWindowExW(0, L"EDIT", L"25", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0,
                               0, 0, hwnd, reinterpret_cast<HMENU>(100), GetModuleHandle(nullptr), nullptr);
        SendMessageW(edit, EM_SETLIMITTEXT, 14, 0);
        SetWindowSubclass(edit, editProc, 1, reinterpret_cast<DWORD_PTR>(this));
        updateFont();
        dropTarget = new VideoDrop(this);
        RegisterDragDrop(hwnd, dropTarget);
        layout();
        startTick = GetTickCount64();
        lastAnimationTime = std::chrono::steady_clock::now();
        SetTimer(hwnd, 1, 16, nullptr);
        detectionThread = std::thread([this] {
            try {
                engine.detect(cancelDetection);
            } catch (...) {
            }
        });
        return true;
    }
    void updateFont() {
        if (inputFont)
            DeleteObject(inputFont);
        inputFont = CreateFontW(-MulDiv(28, int(dpi), 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH, uiFont.family.c_str());
        SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(inputFont), TRUE);
        SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(0, 0));
        HDC dc = GetDC(edit);
        auto previous = SelectObject(dc, inputFont);
        TEXTMETRICW metrics{};
        if (GetTextMetricsW(dc, &metrics))
            inputLineHeight = metrics.tmHeight;
        SelectObject(dc, previous);
        ReleaseDC(edit, dc);
    }
    void layout() {
        RECT client{};
        GetClientRect(hwnd, &client);
        w = float(client.right) * 96 / dpi;
        h = float(client.bottom) * 96 / dpi;
        const float content = w - 2 * MARGIN;
        float leftWidth = (content - 20) * 0.51f;
        left = rect(MARGIN, 166, leftWidth, 300);
        right = rect(left.right + 20, 166, content - leftWidth - 20, 300);
        sizeBox = rect(right.left + 24, 222, width(right) - 48, 58);
        actionBox = rect(MARGIN, 488, content, h - 532);
        if (height(actionBox) < 104)
            actionBox.bottom = actionBox.top + 104;
        zones[Drop] = left;
        zones[Browse] = rect(left.left + (width(left) - 146) / 2, 358, 146, 42);
        zones[Size] = sizeBox;
        zones[Unit] = rect(sizeBox.right - 78, 222, 78, 58);
        float buttonWidth = (width(right) - 64) / 3;
        for (int i = 0; i < 3; i++)
            zones[Quality + i] = rect(right.left + 24 + i * (buttonWidth + 8), 344, buttonWidth, 58);
        if (presetX == 0)
            presetX = zones[Quality + static_cast<int>(preset)].left;
        float actionCenterY = (actionBox.top + actionBox.bottom) / 2;
        zones[Action] = rect(actionBox.right - 196, actionCenterY - 26, 172, 52);
        zones[Folder] = rect(actionBox.right - 310, actionCenterY - 17, 106, 34);
        zones[Minimize] = rect(w - 92, 15, 38, 36);
        zones[Close] = rect(w - 50, 15, 38, 36);
        zones[UnitMB] = rect(sizeBox.right - 92, 288, 92, 42);
        zones[UnitGB] = rect(sizeBox.right - 92, 330, 92, 42);
        zones[About] = rect(w - 150, h - 33, 118, 20);
        zones[AboutClose] = rect(w / 2 - 60, h / 2 + 84, 120, 40);
        float scale = float(dpi) / 96;
        int fieldHeight = int(std::lround(height(sizeBox) * scale));
        int lineHeight = inputLineHeight > 0 ? inputLineHeight : int(std::lround(34 * scale));
        int inputTop = int(std::lround(sizeBox.top * scale)) + (fieldHeight - lineHeight) / 2;
        SetWindowPos(edit, nullptr, int(std::lround((sizeBox.left + 18) * scale)), inputTop,
                     int(std::lround((width(sizeBox) - 112) * scale)), lineHeight,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    bool mainEnabled() const {
        return !loading && (busy || done || media.has_value());
    }
    D2D1_RECT_F unitMenuBounds() const {
        float reveal = std::clamp(unitReveal, 0.0f, 1.0f);
        return rect(zones[UnitMB].left, 288 - 6 * (1 - reveal), 92, 84 * reveal);
    }
    D2D1_RECT_F unitRowBounds(Hit id) const {
        auto r = zones[id];
        float offset = 6 * (1 - std::clamp(unitReveal, 0.0f, 1.0f));
        r.top -= offset;
        r.bottom -= offset;
        return r;
    }
    void showAbout(bool open) {
        aboutOpen = open;
        unitOpen = false;
        ShowWindow(edit, open ? SW_HIDE : SW_SHOW);
        if (open) {
            focus = AboutClose;
            SetFocus(hwnd);
        }
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    Hit hit(float x, float y) const {
        if (aboutOpen)
            return contains(zones[AboutClose], x, y) ? AboutClose : None;
        if (unitReveal > 0.01f && contains(unitMenuBounds(), x, y)) {
            if (!unitOpen)
                return None;
            if (contains(unitRowBounds(UnitMB), x, y))
                return UnitMB;
            if (contains(unitRowBounds(UnitGB), x, y))
                return UnitGB;
        }
        for (Hit id : {Close, Minimize, About, Action, Folder, Quality, Balanced, Speed, Unit, Browse}) {
            if (!contains(zones[id], x, y))
                continue;
            if (id == Action && !mainEnabled())
                continue;
            if (id == Folder && !done)
                continue;
            if ((id == Quality || id == Balanced || id == Speed || id == Unit || id == Browse) &&
                (busy || loading))
                continue;
            if (id == Browse && media)
                return Drop;
            return id;
        }
        if (contains(left, x, y) && !loading && !busy)
            return Drop;
        return None;
    }
    void post(Message *m) {
        if (shuttingDown.load() || !PostMessageW(hwnd, WM_ENGINE, 0, reinterpret_cast<LPARAM>(m)))
            delete m;
    }
    void beginLoad(const fs::path &file) {
        if (busy || loading)
            return;
        if (worker.joinable())
            worker.join();
        incoming = file;
        loading = true;
        done = false;
        error.clear();
        phase = L"Reading video";
        targetProgress = displayProgress = 0;
        cancelOperation = false;
        release(preview);
        unitOpen = false;
        InvalidateRect(hwnd, nullptr, FALSE);
        worker = std::thread([this, file] {
            try {
                auto m = std::make_unique<Message>();
                m->kind = Message::Loaded;
                m->media = engine.probe(file, cancelOperation);
                m->thumbnail = engine.thumbnail(m->media, cancelOperation);
                if (cancelOperation.load())
                    throw std::runtime_error("Cancelled");
                post(m.release());
            } catch (const std::exception &e) {
                auto m = new Message{};
                m->kind = Message::Failed;
                m->error = utf16(e.what());
                post(m);
            }
        });
    }
    void browse() {
        if (busy || loading)
            return;
        IFileOpenDialog *dialog = nullptr;
        if (FAILED(
                CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
            return;
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
        COMDLG_FILTERSPEC filters[] = {{L"Videos", L"*.mp4;*.mov;*.mkv;*.webm;*.avi;*.m4v;*.wmv;*.flv;*.mpeg;"
                                                   L"*.mpg;*.ts;*.mts;*.m2ts;*.ogv;*.3gp;*.av1;*.ivf"},
                                       {L"All files", L"*.*"}};
        dialog->SetFileTypes(2, filters);
        dialog->SetTitle(L"Choose a video");
        dialog->SetOkButtonLabel(L"Open");
        dialog->SetFileNameLabel(L"File &name");
        if (SUCCEEDED(dialog->Show(hwnd))) {
            IShellItem *item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    fs::path file(path);
                    CoTaskMemFree(path);
                    item->Release();
                    dialog->Release();
                    beginLoad(file);
                    return;
                }
                item->Release();
            }
        }
        dialog->Release();
    }
    uint64_t targetBytes() {
        wchar_t value[32]{};
        GetWindowTextW(edit, value, 32);
        wchar_t *end = nullptr;
        double amount = wcstod(value, &end);
        while (end && *end == L' ')
            ++end;
        double bytes = amount * (gb ? 1e9 : 1e6);
        if (end == value || !end || *end || !std::isfinite(bytes) || bytes < 1 || bytes > 1e12)
            throw std::runtime_error("Enter a valid target size");
        return static_cast<uint64_t>(std::floor(bytes));
    }
    void changed() {
        if (busy || loading)
            return;
        error.clear();
        if (done) {
            done = false;
            phase = L"Ready";
            targetProgress = displayProgress = 0;
        }
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    void compress() {
        if (!media || loading || busy)
            return;
        uint64_t bytes = 0;
        try {
            bytes = targetBytes();
            if (bytes >= media->bytes) {
                output = media->path;
                done = true;
                error.clear();
                phase = L"Already below target";
                targetProgress = 1;
                InvalidateRect(hwnd, nullptr, FALSE);
                return;
            }
            makePlan(*media, bytes);
        } catch (const std::exception &e) {
            error = utf16(e.what());
            phase = L"Check target size";
            SetFocus(edit);
            SendMessageW(edit, EM_SETSEL, 0, -1);
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }
        if (worker.joinable())
            worker.join();
        busy = true;
        done = false;
        error.clear();
        phase = L"Preparing";
        targetProgress = displayProgress = 0;
        secondsLeft = -1;
        encodingFps = 0;
        cancelOperation = false;
        unitOpen = false;
        EnableWindow(edit, FALSE);
        auto source = *media;
        auto mode = preset;
        worker = std::thread([this, source, bytes, mode] {
            try {
                auto path =
                    engine.compress(source, bytes, mode, cancelOperation, [this](const Update &update) {
                        auto m = new Message{};
                        m->kind = Message::Progressed;
                        m->update = update;
                        post(m);
                    });
                auto m = new Message{};
                m->kind = Message::Finished;
                m->output = path;
                post(m);
            } catch (const std::exception &e) {
                auto m = new Message{};
                m->kind = Message::Failed;
                m->error = utf16(e.what());
                post(m);
            }
        });
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    void openFolder() {
        if (output.empty())
            return;
        PIDLIST_ABSOLUTE item = nullptr;
        if (SUCCEEDED(SHParseDisplayName(output.c_str(), nullptr, &item, 0, nullptr))) {
            SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
            CoTaskMemFree(item);
        }
    }
    void act(Hit id) {
        if (id != Unit && id != UnitMB && id != UnitGB)
            unitOpen = false;
        switch (id) {
        case Drop:
        case Browse:
            browse();
            break;
        case Unit:
            if (!busy && !loading)
                unitOpen = !unitOpen;
            break;
        case UnitMB:
        case UnitGB:
            gb = id == UnitGB;
            unitOpen = false;
            changed();
            break;
        case Quality:
        case Balanced:
        case Speed:
            if (!busy && !loading) {
                preset = static_cast<Preset>(id - Quality);
                changed();
            }
            break;
        case Action:
            if (busy) {
                cancelOperation = true;
                phase = L"Cancelling";
            } else if (done) {
                ShellExecuteW(hwnd, L"open", output.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            } else
                compress();
            break;
        case Folder:
            openFolder();
            break;
        case Minimize:
            ShowWindow(hwnd, SW_MINIMIZE);
            break;
        case Close:
            SendMessageW(hwnd, WM_CLOSE, 0, 0);
            break;
        case About:
            showAbout(true);
            break;
        case AboutClose:
            showAbout(false);
            break;
        default:
            break;
        }
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    void navigate(bool backwards) {
        if (aboutOpen) {
            focus = AboutClose;
            keyboardFocus = true;
            SetFocus(hwnd);
            return;
        }
        std::vector<Hit> order;
        if (!busy && !loading)
            order = {Browse, Size, Unit, Quality, Balanced, Speed};
        if (mainEnabled())
            order.push_back(Action);
        if (done)
            order.push_back(Folder);
        if (order.empty())
            return;
        auto it = std::find(order.begin(), order.end(), GetFocus() == edit ? Size : focus);
        int i = it == order.end() ? (backwards ? 0 : -1) : int(it - order.begin());
        i = (i + (backwards ? -1 : 1) + int(order.size())) % int(order.size());
        focus = order[i];
        keyboardFocus = true;
        SetFocus(focus == Size ? edit : hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    static LRESULT CALLBACK editProc(HWND control, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
        auto app = reinterpret_cast<App *>(data);
        if (msg == WM_KEYDOWN) {
            if (wp == VK_TAB) {
                PostMessageW(app->hwnd, WM_NAVIGATE, (GetKeyState(VK_SHIFT) & 0x8000) != 0, 0);
                return 0;
            }
            if (wp == VK_RETURN) {
                app->compress();
                return 0;
            }
            if (wp == VK_ESCAPE) {
                SetFocus(app->hwnd);
                return 0;
            }
        }
        if (msg == WM_CHAR && (wp == L'\t' || wp == L'\r'))
            return 0;
        if (msg == WM_SETFOCUS) {
            app->focus = Size;
            InvalidateRect(app->hwnd, nullptr, FALSE);
        }
        if (msg == WM_KILLFOCUS)
            InvalidateRect(app->hwnd, nullptr, FALSE);
        return DefSubclassProc(control, msg, wp, lp);
    }
    bool loadBitmapResource(UINT id, ID2D1Bitmap **bitmap) {
        auto module = GetModuleHandleW(nullptr);
        auto resource = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
        if (!resource || !wic || !target)
            return false;
        auto bytes = SizeofResource(module, resource);
        auto data = static_cast<BYTE *>(LockResource(LoadResource(module, resource)));
        IWICStream *stream = nullptr;
        IWICBitmapDecoder *decoder = nullptr;
        IWICBitmapFrameDecode *frame = nullptr;
        IWICFormatConverter *converter = nullptr;
        bool loaded = data && bytes && SUCCEEDED(wic->CreateStream(&stream)) &&
                      SUCCEEDED(stream->InitializeFromMemory(data, bytes)) &&
                      SUCCEEDED(wic->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad,
                                                            &decoder)) &&
                      SUCCEEDED(decoder->GetFrame(0, &frame)) &&
                      SUCCEEDED(wic->CreateFormatConverter(&converter)) &&
                      SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppPBGRA,
                                                      WICBitmapDitherTypeNone, nullptr, 0,
                                                      WICBitmapPaletteTypeMedianCut)) &&
                      SUCCEEDED(target->CreateBitmapFromWicBitmap(converter, nullptr, bitmap));
        release(converter);
        release(frame);
        release(decoder);
        release(stream);
        return loaded;
    }
    bool resources() {
        if (target)
            return true;
        if (!factory || !textFactory)
            return false;
        RECT r{};
        GetClientRect(hwnd, &r);
        if (FAILED(factory->CreateHwndRenderTarget(
                D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                             D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_IGNORE),
                                             float(dpi), float(dpi)),
                D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(r.right, r.bottom),
                                                 D2D1_PRESENT_OPTIONS_IMMEDIATELY),
                &target)))
            return false;
        target->CreateSolidColorBrush(color(0), &brush);
        target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
        if (!brush || !loadBitmapResource(103, &iconBitmap) || !loadBitmapResource(104, &markBitmap)) {
            release(iconBitmap);
            release(markBitmap);
            release(brush);
            release(target);
            return false;
        }
        return true;
    }
    void fill(D2D1_RECT_F r, UINT32 c, float radius = 10, float a = 1) {
        brush->SetColor(color(c, a));
        target->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush);
    }
    void border(D2D1_RECT_F r, UINT32 c, float radius = 10, float thickness = 1, float a = 1) {
        brush->SetColor(color(c, a));
        target->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush, thickness);
    }
    void line(float x1, float y1, float x2, float y2, UINT32 c, float thickness = 1, float a = 1) {
        brush->SetColor(color(c, a));
        target->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), brush, thickness);
    }
    void circle(float x, float y, float radius, UINT32 c, float a = 1) {
        brush->SetColor(color(c, a));
        target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), radius, radius), brush);
    }
    void text(const std::wstring &s, D2D1_RECT_F r, int format, UINT32 c,
              DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING, float opacity = 1) {
        if (!formats[format])
            return;
        auto f = formats[format];
        f->SetTextAlignment(align);
        brush->SetColor(color(c, opacity));
        target->DrawTextW(s.data(), static_cast<UINT32>(s.size()), f, r, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    void logo(float x, float y, float size, bool background = true) {
        auto bitmap = background ? iconBitmap : markBitmap;
        if (bitmap)
            target->DrawBitmap(bitmap, rect(x, y, size, size), 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }
    void arrow(float x, float y, UINT32 c) {
        line(x - 6, y, x + 6, y, c, 1.8f);
        line(x + 1, y - 5, x + 6, y, c, 1.8f);
        line(x + 1, y + 5, x + 6, y, c, 1.8f);
    }
    void receive(Message *raw) {
        std::unique_ptr<Message> message(raw);
        switch (message->kind) {
        case Message::Loaded:
            media = message->media;
            loading = false;
            phase = L"Ready";
            error.clear();
            loadPreview(message->thumbnail);
            break;
        case Message::Progressed:
            phase = message->update.phase;
            if (message->update.fraction < targetProgress && message->update.fraction == 0)
                displayProgress = 0;
            targetProgress = message->update.fraction;
            secondsLeft = message->update.secondsLeft;
            encodingFps = message->update.encodingFps;
            break;
        case Message::Finished:
            output = message->output;
            loading = busy = false;
            done = true;
            phase = L"Done";
            targetProgress = 1;
            secondsLeft = 0;
            EnableWindow(edit, TRUE);
            break;
        case Message::Failed:
            loading = busy = done = false;
            targetProgress = displayProgress = 0;
            EnableWindow(edit, TRUE);
            if (message->error == L"Cancelled") {
                error.clear();
                phase = L"Cancelled";
            } else {
                error = message->error;
                phase = media ? L"Try again" : L"Choose a video";
            }
            break;
        }
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    void loadPreview(const fs::path &path) {
        if (path.empty())
            return;
        resources();
        IWICBitmapDecoder *decoder = nullptr;
        IWICBitmapFrameDecode *frame = nullptr;
        IWICFormatConverter *converter = nullptr;
        if (wic && target &&
            SUCCEEDED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                     WICDecodeMetadataCacheOnLoad, &decoder)) &&
            SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(wic->CreateFormatConverter(&converter)) &&
            SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                            nullptr, 0, WICBitmapPaletteTypeMedianCut))) {
            release(preview);
            target->CreateBitmapFromWicBitmap(converter, nullptr, &preview);
        }
        release(converter);
        release(frame);
        release(decoder);
        std::error_code ec;
        fs::remove(path, ec);
    }
    void paint() {
        PAINTSTRUCT paint{};
        BeginPaint(hwnd, &paint);
        if (!resources()) {
            EndPaint(hwnd, &paint);
            return;
        }
        target->BeginDraw();
        target->Clear(color(0xf5f6f8));
        // Lightweight custom title bar, with native resize, snap, DPI and shadow behavior.
        logo(32, 24, 34);
        text(L"Squeeze", rect(78, 22, 220, 36), 6, 0x30363b);
        fill(zones[Minimize], 0xe9edf3, 8, hoverAlpha[Minimize]);
        fill(zones[Close], 0xfee8e8, 8, hoverAlpha[Close]);
        float cx = zones[Minimize].left + 19, cy = zones[Minimize].top + 18;
        line(cx - 5, cy + 2, cx + 5, cy + 2, 0x606a73, 1.3f);
        cx = zones[Close].left + 19;
        line(cx - 4, cy - 4, cx + 4, cy + 4, 0x606a73, 1.3f);
        line(cx + 4, cy - 4, cx - 4, cy + 4, 0x606a73, 1.3f);
        line(32, 82, w - 32, 82, 0xe1e5eb);
        text(L"Compress a video", rect(32, 103, w - 152, 42), 7, 0x30363b);
        border(rect(w - 91, 110, 59, 28), 0xc8cfd7, 14);
        text(L"MP4", rect(w - 91, 110, 59, 28), 2, 0x69737d, DWRITE_TEXT_ALIGNMENT_CENTER);
        fill(left, 0xffffff, 12);
        border(left, dragging ? 0x1460f5 : 0xc4ccd6, 12, dragging ? 2.0f : 1.0f);
        if (dragging) {
            fill(rect(left.left + 1, left.top + 1, width(left) - 2, height(left) - 2), 0xe9f0ff, 11);
        }
        if (media && !loading) {
            text(L"VIDEO", rect(left.left + 24, 184, 100, 24), 0, 0x84909a);
            auto change = rect(left.right - 86, 181, 64, 30);
            fill(change, 0xeaf0ff, 8, hoverAlpha[Drop]);
            text(L"Change", change, 1, 0x1460f5, DWRITE_TEXT_ALIGNMENT_CENTER);
            auto imageRect = rect(left.left + 24, 220, width(left) - 48, 150);
            fill(imageRect, 0xf0f3f8, 8);
            if (preview) {
                auto imageSize = preview->GetSize();
                float ratio =
                    std::min(width(imageRect) / imageSize.width, height(imageRect) / imageSize.height);
                float iw = imageSize.width * ratio, ih = imageSize.height * ratio;
                auto destination = rect(imageRect.left + (width(imageRect) - iw) / 2,
                                        imageRect.top + (height(imageRect) - ih) / 2, iw, ih);
                target->PushAxisAlignedClip(imageRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                target->DrawBitmap(preview, destination, 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                target->PopAxisAlignedClip();
            } else
                logo(imageRect.left + width(imageRect) / 2 - 27, imageRect.top + 48, 54, false);
            text(media->path.filename().wstring(), rect(left.left + 24, 386, width(left) - 48, 26), 4,
                 0x30363b);
            std::wstring details = std::to_wstring(media->width) + L" × " + std::to_wstring(media->height) +
                                   L"  ·  " + timeText(media->duration) + L"  ·  " + sizeText(media->bytes);
            text(details, rect(left.left + 24, 416, width(left) - 48, 22), 2, 0x69737d);
        } else {
            float center = left.left + width(left) / 2;
            float bob = reducedMotion ? 0
                        : dragging    ? float(std::sin(double(GetTickCount64()) / 180)) * 3
                                      : 0;
            fill(rect(center - 32, 220 + bob, 64, 64), 0xe9f0ff, 18);
            logo(center - 26, 226 + bob, 52, false);
            text(loading    ? L"Reading video…"
                 : dragging ? L"Drop it here"
                            : L"Drop a video",
                 rect(left.left + 20, 300, width(left) - 40, 34), 6, 0x30363b, DWRITE_TEXT_ALIGNMENT_CENTER);
            if (loading)
                text(incoming.filename().wstring(), rect(left.left + 24, 334, width(left) - 48, 22), 1,
                     0x84909a, DWRITE_TEXT_ALIGNMENT_CENTER);
            if (!loading) {
                auto b = zones[Browse];
                fill(b, 0xffffff);
                fill(b, 0xeaf0ff, 10, hoverAlpha[Browse]);
                border(b, 0xb8c9e9);
                text(L"Browse files", b, 9, 0x1460f5, DWRITE_TEXT_ALIGNMENT_CENTER);
            } else {
                float a =
                    reducedMotion ? 0.6f : 0.4f + float(std::sin(double(GetTickCount64()) / 250)) * 0.2f;
                fill(rect(center - 40, 383, 80, 4), 0x1460f5, 2, a);
            }
        }
        fill(right, 0xffffff, 12);
        border(right, 0xc4ccd6, 12);
        text(L"Target size", rect(right.left + 24, 186, width(right) - 48, 25), 9, 0x30363b);
        fill(sizeBox, 0xffffff, 10);
        border(sizeBox,
               (!error.empty() && phase == L"Check target size") ? 0xe18a68
               : GetFocus() == edit                              ? 0x1460f5
                                                                 : 0xc4ccd6,
               10, GetFocus() == edit ? 1.5f : 1.0f);
        line(sizeBox.right - 79, sizeBox.top + 12, sizeBox.right - 79, sizeBox.bottom - 12, 0xe1e5eb);
        fill(rect(zones[Unit].left + 3, zones[Unit].top + 3, width(zones[Unit]) - 6, height(zones[Unit]) - 6),
             0xeaf0ff, 7, hoverAlpha[Unit]);
        text(gb ? L"GB" : L"MB", rect(zones[Unit].left + 6, sizeBox.top, 48, 58), 9,
             busy || loading ? 0x9aa3ad : 0x30363b, DWRITE_TEXT_ALIGNMENT_CENTER);
        float ux = zones[Unit].right - 19, uy = sizeBox.top + 29;
        line(ux - 4, uy - 2, ux, uy + 2, 0x7d8791, 1.5f);
        line(ux, uy + 2, ux + 4, uy - 2, 0x7d8791, 1.5f);
        text(L"Preset", rect(right.left + 24, 304, width(right) - 48, 26), 9, 0x30363b);
        for (int i = 0; i < 3; i++) {
            Hit id = static_cast<Hit>(Quality + i);
            auto r = zones[id];
            fill(r, 0xffffff, 10);
            fill(r, 0xeaf0ff, 10, hoverAlpha[id] * (1 - presetSelection[i]));
            border(r, 0xc4ccd6, 10, 1, 1 - presetSelection[i]);
        }
        auto selected = zones[Quality + static_cast<int>(preset)];
        selected.left = presetX;
        selected.right = presetX + width(zones[Quality]);
        fill(selected, 0xd4e1ff, 10);
        border(selected, 0x1460f5, 10);
        for (int i = 0; i < 3; i++) {
            Hit id = static_cast<Hit>(Quality + i);
            auto r = zones[id];
            text(std::array{L"Quality", L"Balanced", L"Speed"}[i], r, 9,
                 blendColor(0x30363b, 0x1460f5, presetSelection[i]), DWRITE_TEXT_ALIGNMENT_CENTER,
                 busy || loading ? 0.55f : 1.0f);
        }
        bool success = done && !busy;
        fill(actionBox, success ? 0xe2f5ec : 0xffffff, 12);
        border(actionBox, success ? 0xb8decb : 0xc4ccd6, 12);
        float statusX = actionBox.left + 24;
        std::wstring detail;
        if (!error.empty())
            detail = error;
        else if (success) {
            std::error_code ec;
            auto bytes = fs::file_size(output, ec);
            if (!ec) {
                detail = sizeText(bytes);
                if (media && bytes < media->bytes)
                    detail +=
                        L"  ·  " +
                        std::to_wstring(int(std::round((1.0 - double(bytes) / double(media->bytes)) * 100))) +
                        L"% smaller";
            }
        } else if (busy) {
            detail = std::to_wstring(int(displayProgress * 100)) + L"%  ·  " +
                     std::to_wstring(int(std::round(std::clamp(encodingFps, 0.0, 1000000.0)))) + L" fps";
            if (secondsLeft >= 0 && phase != L"Cancelling")
                detail +=
                    L"  ·  " + (secondsLeft < 60 ? std::to_wstring(secondsLeft) + L"s left"
                                                 : std::to_wstring((secondsLeft + 59) / 60) + L"m left");
        }
        // Center a single status line and a title/detail pair around the same action-row axis.
        float statusCenterY = (zones[Action].top + zones[Action].bottom) / 2;
        float statusY = statusCenterY - (detail.empty() ? 17.0f : 29.0f);
        text(phase, rect(statusX, statusY, width(actionBox) - 250, 34), 8, success ? 0x246b53 : 0x30363b);
        text(detail,
             rect(statusX, statusY + 36,
                  width(actionBox) - (busy   ? 320
                                      : done ? 354
                                             : 250),
                  22),
             3,
             !error.empty() ? 0xad603c
             : success      ? 0x4a8970
                            : 0x69737d);
        if (busy) {
            auto bar = rect(statusX, actionBox.bottom - 24, width(actionBox) - 50, 5);
            fill(bar, 0xd4e1ff, 2.5f);
            if (targetProgress > 0)
                fill(rect(bar.left, bar.top, width(bar) * float(displayProgress), 5), 0x1460f5, 2.5f);
            else {
                float t = float((GetTickCount64() - startTick) % 1600) / 1600;
                float segment = width(bar) * 0.16f;
                float x = bar.left + (width(bar) - segment) * t;
                fill(rect(x, bar.top, segment, 5), 0x1460f5, 2.5f);
            }
        }
        auto button = zones[Action];
        bool enabled = mainEnabled();
        UINT32 blue = enabled ? 0x1460f5 : 0xc1d3fb;
        fill(button, blue, 10);
        if (enabled)
            fill(button, pressed == Action ? 0x003ecc : 0x084bda, 10, hoverAlpha[Action] * 0.7f);
        text(busy   ? L"Cancel"
             : done ? L"Open video"
                    : L"Compress",
             rect(button.left + 12, button.top, width(button) - 37, height(button)), 5,
             enabled ? 0xffffff : 0xf6f9ff, DWRITE_TEXT_ALIGNMENT_CENTER);
        if (!busy)
            arrow(button.right - 23, button.top + 26, 0xffffff);
        if (done) {
            fill(zones[Folder], 0xd4eddf, 8, hoverAlpha[Folder]);
            text(L"Show in folder", zones[Folder], 2, 0x30785c, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        text(L"Squeeze 1.0", zones[About], 0, hover == About ? 0x1460f5 : 0x9aa3ad,
             DWRITE_TEXT_ALIGNMENT_TRAILING);
        if (keyboardFocus && GetFocus() == hwnd && focus != None && focus != Size)
            border(rect(zones[focus].left - 3, zones[focus].top - 3, width(zones[focus]) + 6,
                        height(zones[focus]) + 6),
                   0x1460f5, 12, 1.5f, 0.6f);
        if (unitReveal > 0.001f) {
            auto menu = unitMenuBounds();
            float alpha = std::clamp(unitReveal, 0.0f, 1.0f);
            fill(rect(menu.left + 1, menu.top + 3, width(menu), height(menu)), 0xdbe1ec, 9, 0.7f * alpha);
            fill(menu, 0xffffff, 9, alpha);
            border(menu, 0xc4ccd6, 9, 1, alpha);
            target->PushAxisAlignedClip(
                rect(menu.left + 1, menu.top + 1, width(menu) - 2, std::max(0.0f, height(menu) - 2)),
                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            for (Hit id : {UnitMB, UnitGB}) {
                auto r = unitRowBounds(id);
                fill(rect(r.left + 4, r.top + 4, width(r) - 8, height(r) - 8), 0xeaf0ff, 6,
                     (hover == id || gb == (id == UnitGB)) ? alpha : 0.0f);
                text(id == UnitMB ? L"MB" : L"GB", r, 9, 0x30363b, DWRITE_TEXT_ALIGNMENT_CENTER, alpha);
            }
            target->PopAxisAlignedClip();
        }
        if (aboutOpen) {
            fill(rect(0, 0, w, h), 0x30363b, 0, 0.18f);
            auto card = rect(w / 2 - 220, h / 2 - 140, 440, 280);
            fill(card, 0xffffff, 14);
            border(card, 0xc4ccd6, 14);
            text(L"Squeeze 1.0", rect(card.left + 28, card.top + 22, 384, 34), 6, 0x30363b);
            text(L"MIT License", rect(card.left + 28, card.top + 76, 384, 24), 3, 0x30363b);
            text(L"FFmpeg / x264: GPL", rect(card.left + 28, card.top + 106, 384, 24), 2, 0x84909a);
            text(L"No warranty.", rect(card.left + 28, card.top + 132, 384, 24), 2, 0x84909a);
            text(L"Source: Squeeze-1.0-source.zip", rect(card.left + 28, card.top + 163, 384, 24), 1,
                 0x84909a);
            fill(zones[AboutClose], 0x1460f5, 10);
            text(L"Close", zones[AboutClose], 3, 0xffffff, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        HRESULT hr = target->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET) {
            release(preview);
            release(iconBitmap);
            release(markBitmap);
            release(brush);
            release(target);
        }
        EndPaint(hwnd, &paint);
        // HWND child controls use GDI. Repaint after Direct2D presents the parent,
        // so a parent animation cannot cover the edit's text or caret.
        if (IsWindow(edit))
            RedrawWindow(edit, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    }
    void tick() {
        auto now = std::chrono::steady_clock::now();
        float dt = std::clamp(std::chrono::duration<float>(now - lastAnimationTime).count(), 0.0f, 0.05f);
        lastAnimationTime = now;
        float step = reducedMotion ? 1 : 1 - std::exp(-dt / 0.065f);
        bool changed = false;
        for (size_t i = 0; i < hoverAlpha.size(); ++i) {
            float desired = hover == static_cast<Hit>(i) ? 1.0f : 0.0f;
            if (std::abs(hoverAlpha[i] - desired) > 0.005f) {
                hoverAlpha[i] += (desired - hoverAlpha[i]) * step;
                changed = true;
            } else
                hoverAlpha[i] = desired;
        }
        float selected = zones[Quality + static_cast<int>(preset)].left;
        changed |= spring(presetX, presetVelocity, selected, dt, reducedMotion, 30);
        for (int i = 0; i < 3; ++i)
            changed |= spring(presetSelection[i], presetSelectionVelocity[i],
                              i == static_cast<int>(preset) ? 1.0f : 0.0f, dt, reducedMotion, 30);
        changed |= spring(unitReveal, unitVelocity, unitOpen ? 1.0f : 0.0f, dt, reducedMotion);
        if (std::abs(displayProgress - targetProgress) > 0.001) {
            displayProgress += (targetProgress - displayProgress) * step;
            changed = true;
        } else
            displayProgress = targetProgress;
        if (changed || busy || loading || dragging)
            InvalidateRect(hwnd, nullptr, FALSE);
    }
    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
        auto app = reinterpret_cast<App *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            app = reinterpret_cast<App *>(reinterpret_cast<CREATESTRUCTW *>(lp)->lpCreateParams);
            app->hwnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        if (!app)
            return DefWindowProcW(hwnd, message, wp, lp);
        switch (message) {
        case WM_CREATE:
            return app->initialize() ? 0 : -1;
        case WM_NCCALCSIZE:
            if (wp)
                return 0;
            break;
        case WM_NCHITTEST: {
            LRESULT native = DefWindowProcW(hwnd, message, wp, lp);
            if (native != HTCLIENT)
                return native;
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &p);
            float x = float(p.x) * 96 / app->dpi, y = float(p.y) * 96 / app->dpi;
            const float edge = 6;
            bool l = x<edge, r = x> app->w - edge, t = y<edge, b = y> app->h - edge;
            if (l && t)
                return HTTOPLEFT;
            if (r && t)
                return HTTOPRIGHT;
            if (l && b)
                return HTBOTTOMLEFT;
            if (r && b)
                return HTBOTTOMRIGHT;
            if (l)
                return HTLEFT;
            if (r)
                return HTRIGHT;
            if (t)
                return HTTOP;
            if (b)
                return HTBOTTOM;
            if (y < 78 && x < app->w - 100)
                return HTCAPTION;
            return HTCLIENT;
        }
        case WM_GETMINMAXINFO: {
            auto info = reinterpret_cast<MINMAXINFO *>(lp);
            info->ptMinTrackSize = {MulDiv(880, app->dpi, 96), MulDiv(638, app->dpi, 96)};
            MONITORINFO monitor{sizeof(monitor)};
            if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) {
                info->ptMaxPosition = {monitor.rcWork.left - monitor.rcMonitor.left,
                                       monitor.rcWork.top - monitor.rcMonitor.top};
                info->ptMaxSize = {monitor.rcWork.right - monitor.rcWork.left,
                                   monitor.rcWork.bottom - monitor.rcWork.top};
            }
            return 0;
        }
        case WM_SIZE:
            if (app->edit) {
                app->layout();
                if (app->target)
                    app->target->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_DPICHANGED: {
            app->dpi = HIWORD(wp);
            auto r = reinterpret_cast<RECT *>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            app->updateFont();
            if (app->target)
                app->target->SetDpi(float(app->dpi), float(app->dpi));
            app->layout();
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            app->paint();
            return 0;
        case WM_TIMER:
            app->tick();
            return 0;
        case WM_ENGINE:
            app->receive(reinterpret_cast<Message *>(lp));
            return 0;
        case WM_NAVIGATE:
            app->navigate(wp != 0);
            return 0;
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC:
            if (reinterpret_cast<HWND>(lp) == app->edit) {
                SetTextColor(reinterpret_cast<HDC>(wp), RGB(48, 54, 59));
                SetBkColor(reinterpret_cast<HDC>(wp), RGB(255, 255, 255));
                return reinterpret_cast<LRESULT>(GetStockObject(WHITE_BRUSH));
            }
            break;
        case WM_COMMAND:
            if (LOWORD(wp) == 100 && HIWORD(wp) == EN_CHANGE)
                app->changed();
            return 0;
        case WM_MOUSEMOVE: {
            float x = GET_X_LPARAM(lp) * 96.0f / app->dpi, y = GET_Y_LPARAM(lp) * 96.0f / app->dpi;
            Hit id = app->hit(x, y);
            if (id != app->hover) {
                app->hover = id;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&track);
            SetCursor(LoadCursor(nullptr, id == None ? IDC_ARROW : IDC_HAND));
            return 0;
        }
        case WM_MOUSELEAVE:
            app->hover = None;
            return 0;
        case WM_LBUTTONDOWN: {
            float x = GET_X_LPARAM(lp) * 96.0f / app->dpi, y = GET_Y_LPARAM(lp) * 96.0f / app->dpi;
            app->pressed = app->hit(x, y);
            app->keyboardFocus = false;
            if (app->pressed != None) {
                app->focus = app->pressed;
                SetFocus(hwnd);
                SetCapture(hwnd);
            } else if (app->unitOpen) {
                app->unitOpen = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            auto id = app->hit(GET_X_LPARAM(lp) * 96.0f / app->dpi, GET_Y_LPARAM(lp) * 96.0f / app->dpi);
            auto pressed = app->pressed;
            app->pressed = None;
            ReleaseCapture();
            if (id == pressed && id != None)
                app->act(id);
            return 0;
        }
        case WM_CAPTURECHANGED:
            app->pressed = None;
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_TAB) {
                app->navigate((GetKeyState(VK_SHIFT) & 0x8000) != 0);
                return 0;
            }
            if (wp == VK_RETURN || wp == VK_SPACE) {
                app->act(app->focus);
                return 0;
            }
            if (wp == VK_ESCAPE) {
                if (app->aboutOpen)
                    app->showAbout(false);
                else if (app->unitOpen)
                    app->unitOpen = false;
                else if (app->busy || app->loading) {
                    app->cancelOperation = true;
                    app->phase = L"Cancelling";
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if ((GetKeyState(VK_CONTROL) & 0x8000) && wp == 'O') {
                app->browse();
                return 0;
            }
            if (app->focus == Unit && (wp == VK_UP || wp == VK_DOWN)) {
                app->gb = !app->gb;
                app->changed();
                return 0;
            }
            if (app->focus >= Quality && app->focus <= Speed && (wp == VK_LEFT || wp == VK_RIGHT)) {
                int i = (static_cast<int>(app->preset) + (wp == VK_RIGHT ? 1 : 2)) % 3;
                app->focus = static_cast<Hit>(Quality + i);
                app->act(app->focus);
                return 0;
            }
            break;
        case WM_CLOSE:
            app->shuttingDown = true;
            app->cancelOperation = true;
            app->cancelDetection = true;
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, 1);
            RevokeDragDrop(hwnd);
            if (app->worker.joinable())
                app->worker.join();
            if (app->detectionThread.joinable())
                app->detectionThread.join();
            {
                MSG pending{};
                while (PeekMessageW(&pending, hwnd, WM_ENGINE, WM_ENGINE, PM_REMOVE))
                    delete reinterpret_cast<Message *>(pending.lParam);
            }
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, message, wp, lp);
    }
};

bool VideoDrop::accepts(IDataObject *data) {
    FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    return !app->busy && !app->loading && data && SUCCEEDED(data->QueryGetData(&format));
}
HRESULT VideoDrop::DragEnter(IDataObject *data, DWORD, POINTL, DWORD *effect) {
    app->dragging = accepts(data);
    *effect = app->dragging ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    InvalidateRect(app->hwnd, nullptr, FALSE);
    return S_OK;
}
HRESULT VideoDrop::DragOver(DWORD, POINTL, DWORD *effect) {
    *effect = app->dragging ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    return S_OK;
}
HRESULT VideoDrop::DragLeave() {
    app->dragging = false;
    InvalidateRect(app->hwnd, nullptr, FALSE);
    return S_OK;
}
HRESULT VideoDrop::Drop(IDataObject *data, DWORD, POINTL, DWORD *effect) {
    app->dragging = false;
    *effect = DROPEFFECT_NONE;
    FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    if (accepts(data) && SUCCEEDED(data->GetData(&format, &medium))) {
        auto files = reinterpret_cast<HDROP>(medium.hGlobal);
        UINT count = DragQueryFileW(files, 0xffffffff, nullptr, 0);
        if (count == 1) {
            UINT length = DragQueryFileW(files, 0, nullptr, 0);
            std::wstring file(length + 1, L'\0');
            DragQueryFileW(files, 0, file.data(), length + 1);
            file.resize(length);
            app->beginLoad(file);
            *effect = DROPEFFECT_COPY;
        } else {
            app->error = L"Drop one video at a time";
            app->phase = L"Choose a video";
        }
        ReleaseStgMedium(&medium);
    }
    InvalidateRect(app->hwnd, nullptr, FALSE);
    return S_OK;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int show) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    ULONG languages = 0;
    SetThreadPreferredUILanguages(MUI_LANGUAGE_NAME, L"en-US\0", &languages);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    OleInitialize(nullptr);
    struct OleContext {
        ~OleContext() {
            OleUninitialize();
        }
    } oleContext;
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    App app;
    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.lpfnWndProc = App::windowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.lpszClassName = L"SqueezeMainWindow";
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    windowClass.hIconSm = windowClass.hIcon;
    RegisterClassExW(&windowClass);
    UINT dpi = GetDpiForSystem();
    int width = MulDiv(940, dpi, 96), height = MulDiv(648, dpi, 96);
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, windowClass.lpszClassName, L"Squeeze",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, (work.left + work.right - width) / 2,
                                (work.top + work.bottom - height) / 2, width, height, nullptr, nullptr,
                                instance, &app);
    if (!hwnd)
        return 1;
    const DWORD corner = 2;
    DwmSetWindowAttribute(hwnd, 33, &corner, sizeof(corner));
    const BOOL dark = FALSE;
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
    MARGINS margins{1, 1, 1, 1};
    DwmExtendFrameIntoClientArea(hwnd, &margins);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);
    int argc = 0;
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        if (argc > 1 && fs::is_regular_file(argv[1]))
            app.beginLoad(argv[1]);
        LocalFree(argv);
    }
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
