import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:venera/foundation/app.dart';

/// Windows 平板（触屏）上"点击窗口任意位置都会弹出触摸键盘"的修复 —— Dart 侧。
///
/// 成因见 `windows/runner/flutter_window.cpp` 的说明：
/// Flutter 引擎会在窗口上创建一个系统光标（`CreateCaret`），而只有输入法组字结束时才会销毁它。
/// Windows 的触摸键盘自动唤起逻辑看到这个光标，就把整个窗口当成文本编辑器，
/// 于是任何位置的触摸都会弹出触摸键盘。
///
/// 这里负责把"当前是否正在编辑文本"同步给 runner：
///  - 正在编辑（焦点在 [EditableText] 上）→ 允许系统光标存在，触摸键盘照常可用；
///  - 没有编辑 → runner 销毁系统光标，触摸键盘不再乱弹。
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

  static void _sync() {
    final editing = isEditing;
    if (editing == _lastEditing) return;
    _lastEditing = editing;
    _channel.invokeMethod('setEditing', editing).catchError((Object _) {});
  }
}
