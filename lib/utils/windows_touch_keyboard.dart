import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:venera/foundation/app.dart';

/// Windows 平板（触屏）上"点击窗口任意位置都会弹出触摸键盘"的修复 —— Dart 侧。
///
/// 成因见 `windows/runner/flutter_window.cpp` 的说明：
/// Flutter 引擎会在窗口上创建一个系统光标（`CreateCaret`），而只有输入法组字结束时才会销毁它。
/// Windows 的触摸键盘自动唤起逻辑看到这个光标，就把整个窗口当成文本编辑器，
/// 于是任何位置的触摸都会弹出触摸键盘。
///
/// 这里负责两件事：
///  1. [install]：把"当前是否正在编辑文本"同步给 runner，让它只在编辑时保留系统光标；
///  2. [wrap]：Windows 上输入框默认**不会**因为点击别处而失焦，会导致上面的状态一直停在
///     "编辑中"（触摸键盘继续乱弹）。这里在窗口根部监听指针按下，点到输入框以外就让其失焦
///     —— 这也符合桌面/平板上"点别处收起键盘"的惯例。
class WindowsTouchKeyboard {
  WindowsTouchKeyboard._();

  static const _channel = MethodChannel('venera/osk');

  static bool _installed = false;
  static bool _lastEditing = false;

  /// 当前焦点是否落在文本输入框上。
  static bool get isEditing {
    final context = FocusManager.instance.primaryFocus?.context;
    if (context == null) return false;
    // TextField/EditableText 内部的 Focus 是 EditableText 的子孙节点，
    // 所以从焦点节点往上找 EditableText（或它的 State）即可判定。
    return context.findAncestorWidgetOfExactType<EditableText>() != null ||
        context.findAncestorStateOfType<EditableTextState>() != null;
  }

  /// 在 App 初始化时调用一次。
  static void install() {
    if (!App.isWindows || _installed) return;
    _installed = true;
    FocusManager.instance.addListener(_sync);
    _sync();
  }

  /// 包在窗口根部，负责"点别处让输入框失焦"。非 Windows 平台原样返回。
  static Widget wrap(Widget child) {
    if (!App.isWindows) return child;
    return _TapOutsideGuard(child: child);
  }

  static void _sync() {
    final editing = isEditing;
    if (editing == _lastEditing) return;
    _lastEditing = editing;
    _channel.invokeMethod('setEditing', editing).catchError((Object _) {});
  }
}

class _TapOutsideGuard extends StatelessWidget {
  const _TapOutsideGuard({required this.child});

  final Widget child;

  @override
  Widget build(BuildContext context) {
    return Listener(
      behavior: HitTestBehavior.translucent,
      onPointerDown: (event) => _handleTap(context, event),
      child: child,
    );
  }

  void _handleTap(BuildContext context, PointerDownEvent event) {
    // 焦点不在输入框时什么都不做，避免干扰阅读器的键盘快捷键、菜单等焦点逻辑
    if (!WindowsTouchKeyboard.isEditing) return;
    // 点在输入框上（含光标/选区）就交给输入框自己处理
    if (_hitsEditable(context, event)) return;
    FocusManager.instance.primaryFocus?.unfocus();
  }

  /// 用命中测试判断这次点击是否落在 `RenderEditable`（即输入框）上。
  /// 比依赖手势竞争更可靠：无论上层控件是否消费了这次点击都能判断。
  static bool _hitsEditable(BuildContext context, PointerDownEvent event) {
    final view = View.maybeOf(context);
    if (view == null) return false;
    final result = HitTestResult();
    WidgetsBinding.instance.hitTestInView(result, event.position, view.viewId);
    for (final entry in result.path) {
      if (entry.target is RenderEditable) return true;
    }
    return false;
  }
}
