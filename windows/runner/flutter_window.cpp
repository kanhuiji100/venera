#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "comctl32.lib")
#include "flutter_window.h"
#include <commctrl.h>
#include <optional>
#include <winhttp.h>
#include <Windows.h>
#include <winbase.h>
#include <flutter/method_channel.h>
#include <flutter/event_channel.h>
#include <flutter/event_sink.h>
#include <flutter/event_stream_handler_functions.h>
#include <flutter/standard_method_codec.h>
#include "flutter/generated_plugin_registrant.h"
#include <thread>

#define _CRT_SECURE_NO_WARNINGS

std::unique_ptr<flutter::EventSink<flutter::EncodableValue>>&& mouseEvents = nullptr;

std::atomic<bool> mainThreadAlive(true);
std::atomic<std::chrono::steady_clock::time_point> lastHeartbeat(std::chrono::steady_clock::now());
std::thread* monitorThread = nullptr;

char* wideCharToMultiByte(wchar_t* pWCStrKey)
{
    size_t pSize = WideCharToMultiByte(CP_OEMCP, 0, pWCStrKey, wcslen(pWCStrKey), NULL, 0, NULL, NULL);
    char* pCStrKey = new char[pSize + 1];
    WideCharToMultiByte(CP_OEMCP, 0, pWCStrKey, wcslen(pWCStrKey), pCStrKey, pSize, NULL, NULL);
    pCStrKey[pSize] = '\0';
    GlobalFree(pWCStrKey);
    return pCStrKey;
}

char* getProxy() {
    _WINHTTP_CURRENT_USER_IE_PROXY_CONFIG net;
    WinHttpGetIEProxyConfigForCurrentUser(&net);
    if (net.lpszProxy == nullptr) {
        GlobalFree(net.lpszAutoConfigUrl);
        GlobalFree(net.lpszProxyBypass);
        return nullptr;
    }
    else {
        GlobalFree(net.lpszAutoConfigUrl);
        GlobalFree(net.lpszProxyBypass);
        return wideCharToMultiByte(net.lpszProxy);
    }
}

FlutterWindow::FlutterWindow(const flutter::DartProject& project)
    : project_(project) {}

FlutterWindow::~FlutterWindow() {}

// ---------------------------------------------------------------------------
// 修复：Windows 平板（触屏、无物理键盘）上"点击窗口任意位置都会弹出触摸键盘"
//
// 成因（Flutter 引擎行为，见 flutter/flutter#99050）：
//   引擎在 Flutter 视图窗口收到 WM_IME_SETCONTEXT 时，会调用
//   TextInputManager::CreateImeWindow() 在该窗口上 ::CreateCaret() 创建一个系统光标；
//   而这个光标只有收到 WM_IME_ENDCOMPOSITION（输入法组字结束）时才由
//   DestroyImeWindow() 销毁。用户不打中文/不组字时，光标就永久挂在窗口上，
//   Windows 的触摸键盘自动唤起逻辑（看 GetGUIThreadInfo().hwndCaret）因此把整个窗口
//   当成文本编辑器，任何位置的触摸都会把触摸键盘(TabTip/SIP)唤出来。
//
// 处理：
//   Dart 侧（lib/utils/windows_touch_keyboard.dart）把"当前是否正在编辑文本"同步过来。
//   - 不在编辑：销毁系统光标，触摸键盘不再被任意点击唤起；
//   - 进入编辑：补回系统光标（等价于引擎的 CreateImeWindow），触摸键盘照常可用。
// ---------------------------------------------------------------------------
namespace {

HWND g_view_hwnd = nullptr;
std::atomic<bool> g_text_editing(false);

// 建立/销毁系统光标，从而让 Windows 认为/不认为当前窗口是文本编辑器。
void SetImeCaret(bool enabled) {
  HWND hwnd = g_view_hwnd;
  if (hwnd == nullptr) {
    return;
  }
  if (enabled) {
    // 与引擎 TextInputManager::CreateImeWindow() 相同的调用
    ::CreateCaret(hwnd, nullptr, 1, 1);
  } else {
    ::DestroyCaret();
  }
}

LRESULT CALLBACK SuppressImeCaretProc(HWND hwnd, UINT message, WPARAM wparam,
                                      LPARAM lparam, UINT_PTR subclass_id,
                                      DWORD_PTR ref_data) {
  // 先让引擎处理消息（保留它对 IME 组合窗口标志的处理），再按需清掉它创建的光标
  LRESULT result = DefSubclassProc(hwnd, message, wparam, lparam);
  if (message == WM_IME_SETCONTEXT && wparam != 0 && !g_text_editing.load()) {
    ::DestroyCaret();
  }
  return result;
}

}  // namespace

void monitorUIThread() {
    const auto timeout = std::chrono::seconds(5);

    while (mainThreadAlive.load()) {
        auto now = std::chrono::steady_clock::now();
        auto duration = now - lastHeartbeat.load();

        if (duration > timeout) {
            std::cerr << "The UI thread is dead. Terminate the application.";
            std::exit(0);
        }

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

bool FlutterWindow::OnCreate() {
  if (!Win32Window::OnCreate()) {
    return false;
  }

  RECT frame = GetClientArea();

  // The size here must match the window dimensions to avoid unnecessary surface
  // creation / destruction in the startup path.
  flutter_controller_ = std::make_unique<flutter::FlutterViewController>(
      frame.right - frame.left, frame.bottom - frame.top, project_);
  // Ensure that basic setup of the controller was successful.
  if (!flutter_controller_->engine() || !flutter_controller_->view()) {
    return false;
  }
  RegisterPlugins(flutter_controller_->engine());

  const flutter::MethodChannel<> channel(
      flutter_controller_->engine()->messenger(), "venera/method_channel",
      &flutter::StandardMethodCodec::GetInstance()
  );
  channel.SetMethodCallHandler(
    [](const flutter::MethodCall<>& call,const std::unique_ptr<flutter::MethodResult<>>& result) {
      if(call.method_name() == "getProxy"){
        const auto res = getProxy();
        if (res != nullptr){
          std::string s = res;
          result->Success(s);
          }
        else
          result->Success(flutter::EncodableValue("No Proxy"));
        delete(res);
        return;
      }
#ifdef NDEBUG
      else if (call.method_name() == "heartBeat") {

          if (monitorThread == nullptr) {
              monitorThread = new std::thread{ monitorUIThread };
          }
          lastHeartbeat = std::chrono::steady_clock::now();
          result->Success();
          return;
      }
#endif
      result->Success(); // Default response for unhandled method calls
  });

  flutter::EventChannel<> channel2(
    flutter_controller_->engine()->messenger(), "venera/mouse",
    &flutter::StandardMethodCodec::GetInstance()
  );

  auto eventHandler = std::make_unique<
    flutter::StreamHandlerFunctions<flutter::EncodableValue>>(
    [](
      const flutter::EncodableValue* arguments,
      std::unique_ptr<flutter::EventSink<flutter::EncodableValue>>&& events){
        mouseEvents = std::move(events);
        return nullptr;
    },
    [](const flutter::EncodableValue* arguments)
      -> std::unique_ptr<flutter::StreamHandlerError<flutter::EncodableValue>> {
        mouseEvents = nullptr;
        return nullptr;
    }
  );

  channel2.SetStreamHandler(std::move(eventHandler));

  const flutter::MethodChannel<> channel3(
    flutter_controller_->engine()->messenger(), "venera/clipboard",
    &flutter::StandardMethodCodec::GetInstance()
  );
  channel3.SetMethodCallHandler(
    [](const flutter::MethodCall<>& call,const std::unique_ptr<flutter::MethodResult<>>& result) {
      if(call.method_name() == "writeImageToClipboard"){
          flutter::EncodableMap arguments = std::get<flutter::EncodableMap>(*call.arguments());
          std::vector<uint8_t> data = std::get<std::vector<uint8_t>>(arguments["data"]);
          std::int32_t width = std::get<std::int32_t>(arguments["width"]);
          std::int32_t height = std::get<std::int32_t>(arguments["height"]);

          // convert rgba to bgra
          for (int i = 0; i < data.size()/4; i++) {
              uint8_t temp = data[i * 4];
              data[i * 4] = data[i * 4 + 2];
              data[i * 4 + 2] = temp;
          }
          
          auto bitmap = CreateBitmap((int)width, (int)height, 1, 32, data.data());

          if (!bitmap) {
              result->Error("0", "Invalid Image Data");
              return;
          }

          if (OpenClipboard(NULL))
          {
              EmptyClipboard();
              SetClipboardData(CF_BITMAP, bitmap);
              CloseClipboard();
              result->Success();
          }
          else {
              result->Error("Failed to open clipboard");
          }

          DeleteObject(bitmap);
      }
  });

  HWND view_hwnd = flutter_controller_->view()->GetNativeWindow();
  SetChildContent(view_hwnd);

  // 见上方 SuppressImeCaretProc：抑制平板触摸键盘的自动唤起
  if (view_hwnd != nullptr) {
    g_view_hwnd = view_hwnd;
    SetWindowSubclass(view_hwnd, SuppressImeCaretProc, 1, 0);
    // 引擎在窗口创建早期就可能已建立 IME 上下文，这里兜底清一次
    ::DestroyCaret();
  }

  // Dart 侧同步"是否正在编辑文本"，用于控制上面那个系统光标
  const flutter::MethodChannel<> osk_channel(
      flutter_controller_->engine()->messenger(), "venera/osk",
      &flutter::StandardMethodCodec::GetInstance());
  osk_channel.SetMethodCallHandler(
      [](const flutter::MethodCall<>& call,
         const std::unique_ptr<flutter::MethodResult<>>& result) {
        if (call.method_name() == "setEditing") {
          const auto* editing = std::get_if<bool>(call.arguments());
          if (editing == nullptr) {
            result->Error("bad_args", "setEditing expects a bool");
            return;
          }
          g_text_editing = *editing;
          SetImeCaret(*editing);
          result->Success();
          return;
        }
        result->Success();
      });

  flutter_controller_->engine()->SetNextFrameCallback([&]() {
    // this->Show();
  });

  // Flutter can complete the first frame before the "show window" callback is
  // registered. The following call ensures a frame is pending to ensure the
  // window is shown. It is a no-op if the first frame hasn't completed yet.
  flutter_controller_->ForceRedraw();

  return true;
}

void FlutterWindow::OnDestroy() {
  if (flutter_controller_) {
    flutter_controller_ = nullptr;
  }

  Win32Window::OnDestroy();
  if (monitorThread != nullptr) {
      mainThreadAlive = false;
      monitorThread->join();
  }
}

void mouse_side_button_listener(unsigned int input)
{
    if(mouseEvents != nullptr)
    {
        mouseEvents->Success(static_cast<int>(input));
    }
}

LRESULT
FlutterWindow::MessageHandler(HWND hwnd, UINT const message,
                              WPARAM const wparam,
                              LPARAM const lparam) noexcept {
  // Give Flutter, including plugins, an opportunity to handle window messages.
    UINT button = GET_XBUTTON_WPARAM(wparam);
    if (button == XBUTTON1 && message == 528)
    {
        mouse_side_button_listener(0);
    }
    else if (button == XBUTTON2 && message == 528)
    {
        mouse_side_button_listener(1);
    }
    if (flutter_controller_) {
        std::optional<LRESULT> result =
            flutter_controller_->HandleTopLevelWindowProc(hwnd, message, wparam,
                                                      lparam);
        if (result) {
          return *result;
        }
    }

    switch (message) {
      case WM_FONTCHANGE:
          flutter_controller_->engine()->ReloadSystemFonts();
          break;
    }

  return Win32Window::MessageHandler(hwnd, message, wparam, lparam);
}
