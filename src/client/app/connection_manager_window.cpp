/**
 * @file connection_manager_window.cpp
 * @brief 接続管理Window、履歴候補、session生成要求を実装する。
 */

#include "client/app/connection_manager_window.h"

#include "client/app/rdp_user_identity.h"
#include "client/app/resource.h"
#include "common/diagnostic_log.h"

#include <CommCtrl.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <limits>
#include <ranges>
#include <string_view>
#include <utility>

namespace zrdp::client::app {
namespace {

constexpr wchar_t kManagerWindowClassName[] =
    L"ZRdpSvnBridge.ConnectionManagerWindow";
constexpr UINT kMessageConnectionHistoryReady = WM_APP + 20U;
constexpr int kInitialClientWidth = 1040;
constexpr int kInitialClientHeight = 186;
constexpr int kMinimumWindowWidth = 760;
constexpr int kMinimumWindowHeight = 222;
constexpr int kMargin = 8;
constexpr int kGap = 7;
constexpr int kRowHeight = 25;
constexpr int kStatusHeight = 27;

/**
 * @brief UI入力の前後にある空白だけを除いた所有文字列を返す。
 */
[[nodiscard]] std::wstring TrimCopy(std::wstring value) {
    const auto not_space = [](const wchar_t character) {
        return std::iswspace(static_cast<wint_t>(character)) == 0;
    };

    const auto first =
        std::find_if(value.begin(), value.end(), not_space);
    const auto last =
        std::find_if(value.rbegin(), value.rend(), not_space).base();
    // 空白だけの入力を接続先またはlogon識別子として扱わない。
    if (first >= last) {
        return {};
    }

    return std::wstring(first, last);
}

/**
 * @brief Windows ordinal規則で大小文字を無視して比較する。
 */
[[nodiscard]] bool EqualCaseInsensitive(
    const std::wstring_view left,
    const std::wstring_view right) noexcept {
    // 長さ違いを同一候補として扱わず、巨大値をWin32のintへ切り詰めない。
    if (left.size() != right.size() ||
        left.size() >
            static_cast<std::size_t>(
                std::numeric_limits<int>::max())) {
        return false;
    }

    if (left.empty()) {
        return true;
    }

    return CompareStringOrdinal(
               left.data(),
               static_cast<int>(left.size()),
               right.data(),
               static_cast<int>(right.size()),
               TRUE) == CSTR_EQUAL;
}

/**
 * @brief 候補一覧に大小文字差だけの重複があるか確認する。
 */
[[nodiscard]] bool ContainsCaseInsensitive(
    const std::vector<std::wstring>& values,
    const std::wstring_view candidate) noexcept {
    return std::ranges::any_of(
        values,
        [candidate](const std::wstring& value) {
            return EqualCaseInsensitive(value, candidate);
        });
}

/**
 * @brief コンボの有効な選択項目を所有文字列として取得する。
 */
[[nodiscard]] std::wstring ReadComboItemText(
    const HWND combo,
    const LRESULT index) {
    // 未選択または不正indexを履歴値として読み込まない。
    if (index == CB_ERR ||
        index < 0) {
        return {};
    }

    const LRESULT length = SendMessageW(
        combo,
        CB_GETLBTEXTLEN,
        static_cast<WPARAM>(index),
        0L);
    if (length == CB_ERR ||
        length < 0) {
        return {};
    }

    std::wstring value(
        static_cast<std::size_t>(length) + 1U,
        L'\0');
    const LRESULT copied = SendMessageW(
        combo,
        CB_GETLBTEXT,
        static_cast<WPARAM>(index),
        reinterpret_cast<LPARAM>(value.data()));
    // 部分取得した候補を入力欄へ公開しない。
    if (copied == CB_ERR ||
        copied < 0) {
        return {};
    }

    value.resize(static_cast<std::size_t>(copied));
    return value;
}

}  // namespace

ConnectionManagerWindow::ConnectionManagerWindow(
    const HINSTANCE instance,
    IConnectionManagerObserver& observer)
    : instance_(instance),
      observer_(observer) {
}

ConnectionManagerWindow::~ConnectionManagerWindow() {
    connection_history_worker_.Stop();
    // Create後の例外経路でも所有top-level WindowをUI thread上で残さない。
    if (window_ != nullptr &&
        IsWindow(window_)) {
        DestroyWindow(window_);
    }
}

bool ConnectionManagerWindow::Create(
    const int show_command) {
    // class登録失敗時は部分的な接続UIを生成しない。
    if (!RegisterWindowClass()) {
        return false;
    }

    const UINT dpi = GetDpiForSystem();
    // controlと同じ論理寸法を使い、高DPI起動時にも一行目の入力欄を重ねない。
    RECT bounds{
        0,
        0,
        MulDiv(
            kInitialClientWidth,
            static_cast<int>(dpi),
            USER_DEFAULT_SCREEN_DPI),
        MulDiv(
            kInitialClientHeight,
            static_cast<int>(dpi),
            USER_DEFAULT_SCREEN_DPI),
    };
    if (!AdjustWindowRectExForDpi(
            &bounds,
            WS_OVERLAPPEDWINDOW,
            FALSE,
            WS_EX_CONTROLPARENT,
            dpi)) {
        return false;
    }

    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    const int screen_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int screen_y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int screen_width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int screen_height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    const int x =
        screen_x + std::max(0, (screen_width - width) / 2);
    const int y =
        screen_y + std::max(0, (screen_height - height) / 2);
    const std::wstring title =
        LoadStringResource(IDS_CONNECTION_MANAGER_TITLE);

    window_ = CreateWindowExW(
        WS_EX_CONTROLPARENT,
        kManagerWindowClassName,
        title.c_str(),
        WS_OVERLAPPEDWINDOW,
        x,
        y,
        width,
        height,
        nullptr,
        nullptr,
        instance_,
        this);
    if (window_ == nullptr) {
        return false;
    }

    ShowWindow(window_, show_command);
    UpdateWindow(window_);
    return true;
}

bool ConnectionManagerWindow::ProcessDialogMessage(
    MSG& message) const noexcept {
    // 他session Window宛てのmessageをmanagerのTab処理へ取り込まない。
    if (window_ == nullptr ||
        (message.hwnd != window_ &&
         !IsChild(window_, message.hwnd))) {
        return false;
    }

    return IsDialogMessageW(window_, &message) != FALSE;
}

HWND ConnectionManagerWindow::window() const noexcept {
    return window_;
}

void ConnectionManagerWindow::UpdateRuntimeStatus(
    const std::size_t active_sessions,
    const std::optional<std::uint16_t> next_port,
    const proxy::ClientProxyConfigurationError
        configuration_error) {
    std::array<wchar_t, 256> text{};
    int written = -1;
    if (configuration_error !=
        proxy::ClientProxyConfigurationError::None) {
        const std::wstring format =
            LoadStringResource(
                IDS_MANAGER_PROXY_CONFIG_ERROR_FORMAT);
        written = swprintf_s(
            text.data(),
            text.size(),
            format.c_str(),
            static_cast<unsigned int>(active_sessions),
            static_cast<unsigned int>(
                configuration_error));
    } else if (next_port.has_value()) {
        const std::wstring format =
            LoadStringResource(
                IDS_MANAGER_STATUS_FORMAT);
        written = swprintf_s(
            text.data(),
            text.size(),
            format.c_str(),
            static_cast<unsigned int>(active_sessions),
            static_cast<unsigned int>(*next_port));
    } else {
        const std::wstring format =
            LoadStringResource(
                IDS_MANAGER_NO_PORT_FORMAT);
        written = swprintf_s(
            text.data(),
            text.size(),
            format.c_str(),
            static_cast<unsigned int>(active_sessions));
    }

    const std::wstring fallback =
        LoadStringResource(IDS_UNEXPECTED_ERROR);
    SetWindowTextW(
        status_text_,
        written < 0
            ? fallback.c_str()
            : text.data());
}

void ConnectionManagerWindow::RememberSuccessfulConnection(
    std::wstring connection_name,
    std::wstring server,
    std::wstring domain,
    std::wstring user_name,
    const rdp::RdpAudioRedirectionSettings audio) noexcept {
    const HRESULT result =
        connection_history_worker_.QueueRemember(
            std::move(connection_name),
            std::move(server),
            std::move(domain),
            std::move(user_name),
            audio);
    // 履歴保存失敗は成立済みRDP／Bridge sessionへ影響させない。
    if (FAILED(result)) {
        common::WriteDiagnosticLog(
            common::DiagnosticLevel::Warning,
            common::DiagnosticEvent::
                ConnectionHistorySaveReturned,
            {
                common::DiagnosticHResult(result),
                common::DiagnosticValue(
                    common::DiagnosticFieldName::RecordCount,
                    connection_history_entries_.size()),
            });
    }
}

LRESULT CALLBACK ConnectionManagerWindow::WindowProcedure(
    const HWND window,
    const UINT message,
    const WPARAM wparam,
    const LPARAM lparam) noexcept {
    auto* self = reinterpret_cast<ConnectionManagerWindow*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));

    // 最初の生成messageでCreateWindowExWのownerをHWNDへ関連付ける。
    if (message == WM_NCCREATE) {
        const auto* create_data =
            reinterpret_cast<const CREATESTRUCTW*>(lparam);
        self = static_cast<ConnectionManagerWindow*>(
            create_data->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(
            window,
            GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(self));
    }

    if (self == nullptr) {
        return DefWindowProcW(
            window,
            message,
            wparam,
            lparam);
    }

    try {
        const LRESULT result =
            self->HandleMessage(message, wparam, lparam);
        // 破棄済みHWNDへ後続messageを誤dispatchしない。
        if (message == WM_NCDESTROY) {
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            self->window_ = nullptr;
        }

        return result;
    } catch (...) {
        // C++例外をUSER32 callback境界から漏らさない。
        common::WriteDiagnosticLog(
            common::DiagnosticLevel::Error,
            common::DiagnosticEvent::UnexpectedException);
        return DefWindowProcW(
            window,
            message,
            wparam,
            lparam);
    }
}

LRESULT ConnectionManagerWindow::HandleMessage(
    const UINT message,
    const WPARAM wparam,
    const LPARAM lparam) {
    switch (message) {
        case WM_CREATE:
            // 必須入力controlが欠けたmanagerを接続可能状態へしない。
            if (!CreateControls()) {
                return -1;
            }

            {
                RECT bounds{};
                GetClientRect(window_, &bounds);
                LayoutControls(bounds.right, bounds.bottom);
            }
            return 0;
        case WM_SIZE:
            // 最小化中の0寸法を通常layoutへ適用しない。
            if (wparam != SIZE_MINIMIZED) {
                LayoutControls(
                    LOWORD(lparam),
                    HIWORD(lparam));
            }
            return 0;
        case WM_DPICHANGED: {
            const auto* bounds =
                reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(
                window_,
                nullptr,
                bounds->left,
                bounds->top,
                bounds->right - bounds->left,
                bounds->bottom - bounds->top,
                SWP_NOACTIVATE | SWP_NOZORDER);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* info =
                reinterpret_cast<MINMAXINFO*>(lparam);
            info->ptMinTrackSize.x =
                Scale(kMinimumWindowWidth);
            info->ptMinTrackSize.y =
                Scale(kMinimumWindowHeight);
            return 0;
        }
        case WM_COMMAND: {
            const WORD identifier = LOWORD(wparam);
            const WORD notification = HIWORD(wparam);
            if (notification == BN_CLICKED) {
                switch (identifier) {
                    case IDC_CONNECT:
                        OnConnect();
                        return 0;
                    case IDC_DISPLAY_MODE_NATIVE:
                        display_mode_ =
                            rdp::RdpDisplayMode::NativeScroll;
                        return 0;
                    case IDC_DISPLAY_MODE_SMART_SIZING:
                        display_mode_ =
                            rdp::RdpDisplayMode::SmartSizing;
                        return 0;
                    case IDC_DISPLAY_MODE_DYNAMIC_RESOLUTION:
                        display_mode_ =
                            rdp::RdpDisplayMode::
                                DynamicResolution;
                        return 0;
                    default:
                        break;
                }
            }

            // 履歴comboの明示選択だけを関連field補完の契機にする。
            if (identifier == IDC_CONNECTION_NAME &&
                notification == CBN_SELCHANGE) {
                OnConnectionNameSelected();
                return 0;
            }

            if (identifier == IDC_SERVER &&
                notification == CBN_SELCHANGE) {
                OnServerHistorySelected();
                return 0;
            }
            break;
        }
        case WM_CLOSE:
            DestroyWindow(window_);
            return 0;
        case WM_DESTROY:
            connection_history_worker_.Stop();
            return 0;
        case kMessageConnectionHistoryReady:
            OnConnectionHistoryReady();
            return 0;
        default:
            break;
    }

    return DefWindowProcW(
        window_,
        message,
        wparam,
        lparam);
}

bool ConnectionManagerWindow::RegisterWindowClass() const noexcept {
    HICON large_icon = static_cast<HICON>(
        LoadImageW(
            instance_,
            MAKEINTRESOURCEW(IDI_ZRDP_APP),
            IMAGE_ICON,
            GetSystemMetrics(SM_CXICON),
            GetSystemMetrics(SM_CYICON),
            LR_DEFAULTCOLOR));
    HICON small_icon = static_cast<HICON>(
        LoadImageW(
            instance_,
            MAKEINTRESOURCEW(IDI_ZRDP_APP),
            IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON),
            GetSystemMetrics(SM_CYSMICON),
            LR_DEFAULTCOLOR));

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc =
        &ConnectionManagerWindow::WindowProcedure;
    window_class.hInstance = instance_;
    window_class.hIcon = large_icon;
    window_class.hCursor =
        LoadCursorW(nullptr, IDC_ARROW);
    // STATIC／BUTTONと同じsystem colorを親へ使い、control矩形を背景へなじませる。
    window_class.hbrBackground =
        GetSysColorBrush(COLOR_BTNFACE);
    window_class.lpszClassName =
        kManagerWindowClassName;
    window_class.hIconSm = small_icon;

    if (RegisterClassExW(&window_class) != 0U) {
        return true;
    }

    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

bool ConnectionManagerWindow::CreateControls() {
    const auto create_control =
        [this](
            const wchar_t* class_name,
            const std::wstring& text,
            const DWORD style,
            const int identifier) {
            return CreateWindowExW(
                0U,
                class_name,
                text.c_str(),
                WS_CHILD | WS_VISIBLE | style,
                0,
                0,
                0,
                0,
                window_,
                reinterpret_cast<HMENU>(
                    static_cast<INT_PTR>(identifier)),
                instance_,
                nullptr);
        };

    connection_name_label_ = create_control(
        L"STATIC",
        LoadStringResource(IDS_CONNECTION_NAME_LABEL),
        SS_LEFT | SS_CENTERIMAGE,
        -1);
    connection_name_combo_ = create_control(
        L"COMBOBOX",
        L"",
        WS_TABSTOP | CBS_DROPDOWN |
            CBS_AUTOHSCROLL | WS_VSCROLL,
        IDC_CONNECTION_NAME);
    server_label_ = create_control(
        L"STATIC",
        LoadStringResource(IDS_SERVER_LABEL),
        SS_LEFT | SS_CENTERIMAGE,
        -1);
    server_combo_ = create_control(
        L"COMBOBOX",
        L"",
        WS_TABSTOP | CBS_DROPDOWN |
            CBS_AUTOHSCROLL | WS_VSCROLL,
        IDC_SERVER);
    domain_label_ = create_control(
        L"STATIC",
        LoadStringResource(IDS_DOMAIN_LABEL),
        SS_LEFT | SS_CENTERIMAGE,
        -1);
    domain_combo_ = create_control(
        L"COMBOBOX",
        L"",
        WS_TABSTOP | CBS_DROPDOWN |
            CBS_AUTOHSCROLL | WS_VSCROLL,
        IDC_DOMAIN);
    user_name_label_ = create_control(
        L"STATIC",
        LoadStringResource(IDS_USER_NAME_LABEL),
        SS_LEFT | SS_CENTERIMAGE,
        -1);
    user_name_combo_ = create_control(
        L"COMBOBOX",
        L"",
        WS_TABSTOP | CBS_DROPDOWN |
            CBS_AUTOHSCROLL | WS_VSCROLL,
        IDC_USER_NAME);
    connect_button_ = create_control(
        L"BUTTON",
        LoadStringResource(IDS_CONNECT_NEW_SESSION),
        WS_TABSTOP | BS_DEFPUSHBUTTON,
        IDC_CONNECT);
    display_mode_label_ = create_control(
        L"STATIC",
        LoadStringResource(IDS_DISPLAY_MODE_LABEL),
        SS_LEFT | SS_CENTERIMAGE,
        -1);
    native_mode_radio_ = create_control(
        L"BUTTON",
        LoadStringResource(IDS_DISPLAY_MODE_NATIVE),
        WS_GROUP | WS_TABSTOP | BS_AUTORADIOBUTTON,
        IDC_DISPLAY_MODE_NATIVE);
    smart_sizing_mode_radio_ = create_control(
        L"BUTTON",
        LoadStringResource(
            IDS_DISPLAY_MODE_SMART_SIZING),
        WS_TABSTOP | BS_AUTORADIOBUTTON,
        IDC_DISPLAY_MODE_SMART_SIZING);
    dynamic_resolution_mode_radio_ = create_control(
        L"BUTTON",
        LoadStringResource(
            IDS_DISPLAY_MODE_DYNAMIC_RESOLUTION),
        WS_TABSTOP | BS_AUTORADIOBUTTON,
        IDC_DISPLAY_MODE_DYNAMIC_RESOLUTION);
    local_resources_label_ = create_control(
        L"STATIC",
        LoadStringResource(IDS_LOCAL_RESOURCES_LABEL),
        SS_LEFT | SS_CENTERIMAGE,
        -1);
    redirect_microphone_check_ = create_control(
        L"BUTTON",
        LoadStringResource(IDS_REDIRECT_MICROPHONE),
        WS_GROUP | WS_TABSTOP | BS_AUTOCHECKBOX,
        IDC_REDIRECT_MICROPHONE);
    remote_audio_label_ = create_control(
        L"STATIC",
        LoadStringResource(IDS_REMOTE_AUDIO_LABEL),
        SS_LEFT | SS_CENTERIMAGE,
        -1);
    audio_playback_combo_ = create_control(
        L"COMBOBOX",
        L"",
        WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        IDC_AUDIO_PLAYBACK_MODE);
    status_text_ = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"STATIC",
        L"",
        WS_CHILD | WS_VISIBLE |
            SS_LEFT | SS_CENTERIMAGE,
        0,
        0,
        0,
        0,
        window_,
        reinterpret_cast<HMENU>(
            static_cast<INT_PTR>(IDC_STATUS)),
        instance_,
        nullptr);

    const std::array<HWND, 18> controls{
        connection_name_label_,
        connection_name_combo_,
        server_label_,
        server_combo_,
        domain_label_,
        domain_combo_,
        user_name_label_,
        user_name_combo_,
        connect_button_,
        display_mode_label_,
        native_mode_radio_,
        smart_sizing_mode_radio_,
        dynamic_resolution_mode_radio_,
        local_resources_label_,
        redirect_microphone_check_,
        remote_audio_label_,
        audio_playback_combo_,
        status_text_,
    };
    // 必須controlが一つでも欠ける場合はmanager生成全体を失敗させる。
    for (const HWND control : controls) {
        if (control == nullptr) {
            return false;
        }
    }

    SendMessageW(
        connection_name_combo_,
        CB_LIMITTEXT,
        kMaximumConnectionNameCharacters,
        0L);
    SendMessageW(server_combo_, CB_LIMITTEXT, 255U, 0L);
    SendMessageW(
        domain_combo_,
        CB_LIMITTEXT,
        kMaximumRdpDomainCharacters,
        0L);
    SendMessageW(
        user_name_combo_,
        CB_LIMITTEXT,
        kMaximumRdpUserNameCharacters,
        0L);
    SendMessageW(
        native_mode_radio_,
        BM_SETCHECK,
        BST_CHECKED,
        0L);

    constexpr std::array<UINT, 3> audio_option_ids{
        IDS_AUDIO_PLAY_ON_CLIENT,
        IDS_AUDIO_PLAY_ON_REMOTE,
        IDS_AUDIO_DO_NOT_PLAY,
    };
    // enum値と同じ順序で有限の再生先だけをdrop-downへ登録する。
    for (const UINT option_id : audio_option_ids) {
        const std::wstring option =
            LoadStringResource(option_id);
        const LRESULT added = SendMessageW(
            audio_playback_combo_,
            CB_ADDSTRING,
            0U,
            reinterpret_cast<LPARAM>(option.c_str()));
        if (added == CB_ERR ||
            added == CB_ERRSPACE) {
            return false;
        }
    }

    SendMessageW(
        audio_playback_combo_,
        CB_SETCURSEL,
        static_cast<WPARAM>(
            rdp::RdpAudioPlaybackMode::PlayOnClient),
        0L);

    LoadConnectionHistory();
    ApplyControlFont();
    return true;
}

void ConnectionManagerWindow::LayoutControls(
    const int client_width,
    const int client_height) {
    const int margin = Scale(kMargin);
    const int gap = Scale(kGap);
    const int row_height = Scale(kRowHeight);
    const int status_height = Scale(kStatusHeight);
    const int label_width = Scale(62);
    const int connect_width = Scale(116);
    const int connection_name_width = Scale(210);
    const int first_y = margin;
    const int second_y = first_y + row_height + gap;
    const int third_y = second_y + row_height + gap;
    const int fourth_y = third_y + row_height + gap;

    int x = margin;
    MoveWindow(
        connection_name_label_,
        x,
        first_y,
        label_width,
        row_height,
        TRUE);
    x += label_width + gap;
    MoveWindow(
        connection_name_combo_,
        x,
        first_y,
        connection_name_width,
        Scale(240),
        TRUE);
    x += connection_name_width + gap;
    MoveWindow(
        server_label_,
        x,
        first_y,
        label_width,
        row_height,
        TRUE);
    x += label_width + gap;
    const int server_width = std::max(
        0,
        client_width - x - connect_width -
            gap - margin);
    MoveWindow(
        server_combo_,
        x,
        first_y,
        server_width,
        Scale(240),
        TRUE);
    MoveWindow(
        connect_button_,
        client_width - margin - connect_width,
        first_y,
        connect_width,
        row_height,
        TRUE);

    const int domain_width = Scale(260);
    const int user_label_width = Scale(82);
    x = margin;
    MoveWindow(
        domain_label_,
        x,
        second_y,
        label_width,
        row_height,
        TRUE);
    x += label_width + gap;
    MoveWindow(
        domain_combo_,
        x,
        second_y,
        domain_width,
        Scale(240),
        TRUE);
    x += domain_width + gap;
    MoveWindow(
        user_name_label_,
        x,
        second_y,
        user_label_width,
        row_height,
        TRUE);
    x += user_label_width + gap;
    MoveWindow(
        user_name_combo_,
        x,
        second_y,
        std::max(Scale(180), client_width - x - margin),
        Scale(240),
        TRUE);

    x = margin;
    MoveWindow(
        display_mode_label_,
        x,
        third_y,
        label_width + Scale(10),
        row_height,
        TRUE);
    x += label_width + Scale(10) + gap;
    const int radio_width = Scale(190);
    MoveWindow(
        native_mode_radio_,
        x,
        third_y,
        radio_width,
        row_height,
        TRUE);
    x += radio_width;
    MoveWindow(
        smart_sizing_mode_radio_,
        x,
        third_y,
        radio_width,
        row_height,
        TRUE);
    x += radio_width;
    MoveWindow(
        dynamic_resolution_mode_radio_,
        x,
        third_y,
        Scale(220),
        row_height,
        TRUE);

    x = margin;
    const int local_resources_label_width = Scale(118);
    MoveWindow(
        local_resources_label_,
        x,
        fourth_y,
        local_resources_label_width,
        row_height,
        TRUE);
    x += local_resources_label_width + gap;
    const int microphone_width = Scale(240);
    MoveWindow(
        redirect_microphone_check_,
        x,
        fourth_y,
        microphone_width,
        row_height,
        TRUE);
    x += microphone_width + gap;
    const int remote_audio_label_width = Scale(92);
    MoveWindow(
        remote_audio_label_,
        x,
        fourth_y,
        remote_audio_label_width,
        row_height,
        TRUE);
    x += remote_audio_label_width + gap;
    MoveWindow(
        audio_playback_combo_,
        x,
        fourth_y,
        std::max(
            Scale(180),
            client_width - x - margin),
        Scale(180),
        TRUE);

    MoveWindow(
        status_text_,
        0,
        std::max(0, client_height - status_height),
        client_width,
        status_height,
        TRUE);
}

void ConnectionManagerWindow::OnConnect() {
    std::wstring connection_name =
        TrimCopy(ReadWindowText(connection_name_combo_));
    std::wstring server =
        TrimCopy(ReadWindowText(server_combo_));
    // 空接続先ではsession slotとActiveXを生成しない。
    if (server.empty()) {
        const std::wstring message =
            LoadStringResource(IDS_SERVER_REQUIRED);
        MessageBoxW(
            window_,
            message.c_str(),
            L"ZRdp SVN Bridge",
            MB_OK | MB_ICONWARNING);
        SetFocus(server_combo_);
        return;
    }

    RdpUserIdentity identity;
    const HRESULT identity_result =
        NormalizeRdpUserIdentity(
            TrimCopy(ReadWindowText(domain_combo_)),
            TrimCopy(ReadWindowText(user_name_combo_)),
            identity);
    // managerとsessionで異なるlogon対象を解釈しないよう生成前に正規化する。
    if (FAILED(identity_result)) {
        const std::wstring message =
            LoadStringResource(IDS_RDP_IDENTITY_INVALID);
        MessageBoxW(
            window_,
            message.c_str(),
            L"ZRdp SVN Bridge",
            MB_OK | MB_ICONWARNING);
        SetFocus(user_name_combo_);
        return;
    }

    SetWindowTextW(
        domain_combo_,
        identity.domain.c_str());
    SetWindowTextW(
        user_name_combo_,
        identity.user_name.c_str());

    const std::optional<rdp::RdpAudioRedirectionSettings>
        audio = ReadAudioSettingsSelection();
    // 再生先未選択を暗黙のActiveX既定値へ置換せず、利用者に修正を求める。
    if (!audio.has_value()) {
        const std::wstring message =
            LoadStringResource(IDS_AUDIO_SETTING_INVALID);
        MessageBoxW(
            window_,
            message.c_str(),
            L"ZRdp SVN Bridge",
            MB_OK | MB_ICONWARNING);
        SetFocus(audio_playback_combo_);
        return;
    }

    SessionConnectionRequest request{
        std::move(connection_name),
        std::move(server),
        std::move(identity.domain),
        std::move(identity.user_name),
        display_mode_,
        *audio,
    };
    const SessionOpenResult result =
        observer_.OpenSession(std::move(request));
    if (result.succeeded()) {
        return;
    }

    UINT message_id = IDS_SESSION_CREATE_FAILED;
    switch (result.error) {
        case SessionOpenError::SessionLimitReached:
            message_id = IDS_SESSION_LIMIT_REACHED;
            break;
        case SessionOpenError::ProxyPortUnavailable:
            message_id = IDS_SESSION_PORT_UNAVAILABLE;
            break;
        case SessionOpenError::WindowCreationFailed:
            message_id = IDS_SESSION_CREATE_FAILED;
            break;
        case SessionOpenError::ResourceExhausted:
            message_id = IDS_SESSION_RESOURCE_EXHAUSTED;
            break;
        case SessionOpenError::None:
            break;
    }

    const std::wstring message =
        LoadStringResource(message_id);
    MessageBoxW(
        window_,
        message.c_str(),
        L"ZRdp SVN Bridge",
        MB_OK | MB_ICONERROR);
}

void ConnectionManagerWindow::LoadConnectionHistory() {
    const HRESULT result =
        connection_history_worker_.Start(
            window_,
            kMessageConnectionHistoryReady);
    // 履歴補助機能が失敗しても新規RDP session入力を利用可能に保つ。
    if (FAILED(result)) {
        common::WriteDiagnosticLog(
            common::DiagnosticLevel::Warning,
            common::DiagnosticEvent::
                ConnectionHistoryLoadReturned,
            {
                common::DiagnosticHResult(result),
                common::DiagnosticValue(
                    common::DiagnosticFieldName::RecordCount,
                    0U),
            });
    }
}

void ConnectionManagerWindow::OnConnectionHistoryReady() {
    std::vector<ConnectionHistoryResult> results =
        connection_history_worker_.TakeResults();
    // worker完了順にsnapshotを取り込み、最後を最新候補とする。
    for (ConnectionHistoryResult& result : results) {
        common::WriteDiagnosticLog(
            FAILED(result.result)
                ? common::DiagnosticLevel::Warning
                : common::DiagnosticLevel::Information,
            result.operation ==
                    ConnectionHistoryOperation::Load
                ? common::DiagnosticEvent::
                    ConnectionHistoryLoadReturned
                : common::DiagnosticEvent::
                    ConnectionHistorySaveReturned,
            {
                common::DiagnosticHResult(result.result),
                common::DiagnosticValue(
                    common::DiagnosticFieldName::RecordCount,
                    result.entries.size()),
            });
        connection_history_entries_ =
            std::move(result.entries);
    }

    if (!results.empty()) {
        PopulateConnectionHistoryCombos();
    }
}

void ConnectionManagerWindow::PopulateConnectionHistoryCombos() {
    std::wstring current_connection_name =
        TrimCopy(ReadWindowText(connection_name_combo_));
    std::wstring current_server =
        TrimCopy(ReadWindowText(server_combo_));
    std::wstring current_domain =
        TrimCopy(ReadWindowText(domain_combo_));
    std::wstring current_user =
        TrimCopy(ReadWindowText(user_name_combo_));
    rdp::RdpAudioRedirectionSettings current_audio =
        ReadAudioSettingsSelection().value_or(
            rdp::RdpAudioRedirectionSettings{});
    // 初回だけ最新成功接続を既定入力として復元する。
    if (current_server.empty() &&
        !connection_history_entries_.empty()) {
        current_connection_name =
            connection_history_entries_.front().connection_name;
        current_server =
            connection_history_entries_.front().server;
        current_domain =
            connection_history_entries_.front().domain;
        current_user =
            connection_history_entries_.front().user_name;
        current_audio =
            connection_history_entries_.front().audio;
    }

    SendMessageW(
        connection_name_combo_,
        CB_RESETCONTENT,
        0U,
        0L);
    SendMessageW(server_combo_, CB_RESETCONTENT, 0U, 0L);
    SendMessageW(domain_combo_, CB_RESETCONTENT, 0U, 0L);
    SendMessageW(user_name_combo_, CB_RESETCONTENT, 0U, 0L);

    std::vector<std::wstring> connection_names;
    std::vector<std::wstring> servers;
    std::vector<std::wstring> domains;
    std::vector<std::wstring> users;
    connection_names.reserve(connection_history_entries_.size());
    servers.reserve(connection_history_entries_.size());
    domains.reserve(connection_history_entries_.size());
    users.reserve(connection_history_entries_.size());

    // 大小文字差だけの候補を各comboへ一度だけ追加する。
    for (const ConnectionHistoryEntry& entry :
         connection_history_entries_) {
        if (!entry.connection_name.empty() &&
            !ContainsCaseInsensitive(
                connection_names,
                entry.connection_name)) {
            const LRESULT added = SendMessageW(
                connection_name_combo_,
                CB_ADDSTRING,
                0U,
                reinterpret_cast<LPARAM>(
                    entry.connection_name.c_str()));
            if (added != CB_ERR &&
                added != CB_ERRSPACE) {
                connection_names.push_back(
                    entry.connection_name);
            }
        }

        if (!ContainsCaseInsensitive(
                servers,
                entry.server)) {
            const LRESULT added = SendMessageW(
                server_combo_,
                CB_ADDSTRING,
                0U,
                reinterpret_cast<LPARAM>(
                    entry.server.c_str()));
            if (added != CB_ERR &&
                added != CB_ERRSPACE) {
                servers.push_back(entry.server);
            }
        }

        if (!entry.domain.empty() &&
            !ContainsCaseInsensitive(
                domains,
                entry.domain)) {
            const LRESULT added = SendMessageW(
                domain_combo_,
                CB_ADDSTRING,
                0U,
                reinterpret_cast<LPARAM>(
                    entry.domain.c_str()));
            if (added != CB_ERR &&
                added != CB_ERRSPACE) {
                domains.push_back(entry.domain);
            }
        }

        if (!entry.user_name.empty() &&
            !ContainsCaseInsensitive(
                users,
                entry.user_name)) {
            const LRESULT added = SendMessageW(
                user_name_combo_,
                CB_ADDSTRING,
                0U,
                reinterpret_cast<LPARAM>(
                    entry.user_name.c_str()));
            if (added != CB_ERR &&
                added != CB_ERRSPACE) {
                users.push_back(entry.user_name);
            }
        }
    }

    SetWindowTextW(
        connection_name_combo_,
        current_connection_name.c_str());
    SetWindowTextW(
        server_combo_,
        current_server.c_str());
    SetWindowTextW(
        domain_combo_,
        current_domain.c_str());
    SetWindowTextW(
        user_name_combo_,
        current_user.c_str());
    SetAudioSettingsSelection(current_audio);
}

void ConnectionManagerWindow::OnConnectionNameSelected() {
    const LRESULT selection = SendMessageW(
        connection_name_combo_,
        CB_GETCURSEL,
        0U,
        0L);
    const std::wstring selected =
        ReadComboItemText(connection_name_combo_, selection);
    if (selected.empty()) {
        return;
    }

    // 名前は履歴内で一意なので、選択したentryの接続情報を組のまま復元する。
    for (const ConnectionHistoryEntry& entry :
         connection_history_entries_) {
        if (EqualCaseInsensitive(
                entry.connection_name,
                selected)) {
            SetWindowTextW(
                server_combo_,
                entry.server.c_str());
            SetWindowTextW(
                domain_combo_,
                entry.domain.c_str());
            SetWindowTextW(
                user_name_combo_,
                entry.user_name.c_str());
            SetAudioSettingsSelection(entry.audio);
            return;
        }
    }
}

void ConnectionManagerWindow::OnServerHistorySelected() {
    const LRESULT selection = SendMessageW(
        server_combo_,
        CB_GETCURSEL,
        0U,
        0L);
    const std::wstring selected =
        ReadComboItemText(server_combo_, selection);
    if (selected.empty()) {
        return;
    }

    // 新しい順の最初の完全一致から接続名とlogon識別子を補完する。
    for (const ConnectionHistoryEntry& entry :
         connection_history_entries_) {
        if (EqualCaseInsensitive(
                entry.server,
                selected)) {
            SetWindowTextW(
                connection_name_combo_,
                entry.connection_name.c_str());
            SetWindowTextW(
                domain_combo_,
                entry.domain.c_str());
            SetWindowTextW(
                user_name_combo_,
                entry.user_name.c_str());
            SetAudioSettingsSelection(entry.audio);
            return;
        }
    }
}

std::optional<rdp::RdpAudioRedirectionSettings>
ConnectionManagerWindow::ReadAudioSettingsSelection()
    const noexcept {
    const LRESULT microphone_state = SendMessageW(
        redirect_microphone_check_,
        BM_GETCHECK,
        0U,
        0L);
    // 二値check box以外の異常状態をprivacy設定として推測しない。
    if (microphone_state != BST_CHECKED &&
        microphone_state != BST_UNCHECKED) {
        return std::nullopt;
    }

    const LRESULT selection = SendMessageW(
        audio_playback_combo_,
        CB_GETCURSEL,
        0U,
        0L);
    rdp::RdpAudioPlaybackMode playback_mode{};
    switch (selection) {
        case 0:
            playback_mode =
                rdp::RdpAudioPlaybackMode::PlayOnClient;
            break;
        case 1:
            playback_mode =
                rdp::RdpAudioPlaybackMode::
                    PlayOnRemoteComputer;
            break;
        case 2:
            playback_mode =
                rdp::RdpAudioPlaybackMode::DoNotPlay;
            break;
        default:
            // 未選択または外部から改変されたindexをActiveX値へ変換しない。
            return std::nullopt;
    }

    return rdp::RdpAudioRedirectionSettings{
        microphone_state == BST_CHECKED,
        playback_mode,
    };
}

void ConnectionManagerWindow::SetAudioSettingsSelection(
    rdp::RdpAudioRedirectionSettings audio) noexcept {
    // 復元元が未知値でもUIを安全なローカル再生へ縮退させる。
    if (!rdp::IsValidAudioPlaybackMode(
            audio.playback_mode)) {
        audio = {};
    }

    SendMessageW(
        redirect_microphone_check_,
        BM_SETCHECK,
        audio.redirect_microphone
            ? BST_CHECKED
            : BST_UNCHECKED,
        0L);
    SendMessageW(
        audio_playback_combo_,
        CB_SETCURSEL,
        static_cast<WPARAM>(audio.playback_mode),
        0L);
}

void ConnectionManagerWindow::ApplyControlFont() const noexcept {
    const HFONT font = static_cast<HFONT>(
        GetStockObject(DEFAULT_GUI_FONT));
    if (font == nullptr) {
        return;
    }

    const std::array<HWND, 18> controls{
        connection_name_label_,
        connection_name_combo_,
        server_label_,
        server_combo_,
        domain_label_,
        domain_combo_,
        user_name_label_,
        user_name_combo_,
        connect_button_,
        display_mode_label_,
        native_mode_radio_,
        smart_sizing_mode_radio_,
        dynamic_resolution_mode_radio_,
        local_resources_label_,
        redirect_microphone_check_,
        remote_audio_label_,
        audio_playback_combo_,
        status_text_,
    };
    // 同じsystem fontを全manager controlへ適用する。
    for (const HWND control : controls) {
        if (control != nullptr) {
            SendMessageW(
                control,
                WM_SETFONT,
                reinterpret_cast<WPARAM>(font),
                TRUE);
        }
    }
}

int ConnectionManagerWindow::Scale(
    const int value) const noexcept {
    const UINT dpi =
        window_ == nullptr
            ? GetDpiForSystem()
            : GetDpiForWindow(window_);
    return MulDiv(
        value,
        static_cast<int>(dpi),
        USER_DEFAULT_SCREEN_DPI);
}

std::wstring ConnectionManagerWindow::LoadStringResource(
    const UINT string_id) const {
    std::array<wchar_t, 1024> buffer{};
    const int length = LoadStringW(
        instance_,
        string_id,
        buffer.data(),
        static_cast<int>(buffer.size()));
    // 欠落resourceを未初期化文字列として扱わない。
    if (length <= 0) {
        return {};
    }

    return std::wstring(
        buffer.data(),
        static_cast<std::size_t>(length));
}

std::wstring ConnectionManagerWindow::ReadWindowText(
    const HWND control) const {
    if (control == nullptr) {
        return {};
    }

    const int length = GetWindowTextLengthW(control);
    // Win32 text lengthを有限な接続入力上限へ制限する。
    if (length < 0 ||
        length > 1024) {
        return {};
    }

    std::wstring value(
        static_cast<std::size_t>(length) + 1U,
        L'\0');
    const int copied = GetWindowTextW(
        control,
        value.data(),
        length + 1);
    if (copied < 0) {
        return {};
    }

    value.resize(static_cast<std::size_t>(copied));
    return value;
}

}  // namespace zrdp::client::app
