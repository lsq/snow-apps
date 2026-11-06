# Snow Apps 移植统信 UOS / Linux 完整方案

> 目标平台：统信 UOS（Deepin 系，Debian 衍生），**arm64 + amd64 双架构**
> 硬约束：**必须兼容 Qt 5 及以下**
> **决策已定（见 §0.4）**：维持 Qt5 路线，但改造形态改为「**一套源码，Qt 5.15 / Qt 6.12 双版本皆可编译**」，
> 并**保留 Windows 分支**用于本地验证；具体执行见 **[`docs-qt5-downgrade-plan.md`](docs-qt5-downgrade-plan.md)**
> 基线版本：`SNOW_SHOT_VERSION = 1.2.8`（根 `CMakeLists.txt:18`）
> 文档定位：与 `docs-macos-build.md`、`docs-windows-arm64-build.md` 同级的移植设计文档

---

## 0. 结论摘要与可行性判定

### 0.1 一句话结论

工程具备**清晰的三平台抽象骨架**（Windows / macOS 两条腿已实现），但 **Linux 支线几乎为零**；
真正的移植难点不在 Linux，而在于**硬性 Qt6 → Qt5 降级**——现有代码不是"停留在 Qt5 老 API"，
而是**全面拥抱 Qt 6 专有 API**，且 `snow_image_viewer` 直接依赖 Qt 6 才公开的 RHI 渲染抽象。

### 0.2 可行性分级

| 模块 | Linux 可移植性 | Qt5 可降级性 | 综合判定 | 主要阻塞 |
|---|---|---|---|---|
| `snow_draw_engine_qt`（Rust 部分） | 高（纯 Rust，无平台 crate） | — | **可直接复用** | 仅需 aarch64 性能补强 |
| `snow_image` | 高 | 中 | **小改** | `_M_X64` 内联汇编/SSE → NEON |
| `snow_memory` | 高（`mmap` 已实现） | — | **可直接复用** | `pages.rs:123-137` 已就绪 |
| `ant_design_qt` | 中 | 中高 | **中改** | 4 个 `.mm`、`enterEvent`、`position()` |
| `snow_shot`（主程序） | 低（平台层全缺） | 中 | **大改** | 需从零实现全部 Linux 平台后端。**已起步**：物理光标走 X11，并建立了 Rust↔C++ 平台桥（见 9.4.8） |
| `snow_image_viewer` | 低 | **极低** | **重写或裁剪** | `QRhi*` 为 Qt6 专有公开 API |
| Rust `snow-capture` / `snow-cursor` | 低 | — | **X11 完成，Wayland 半程** | 采集与光标走 X11（见 9.4.3），经 MIT-SHM 零拷贝读像素（见 9.4.7）、Damage 跳帧与多屏区域脏矩形（见 9.4.19 / 9.4.21）；Wayland 侧 portal 握手已落地（见 9.4.20），**PipeWire 帧消费仍缺**，故 Wayland 会话明确拒绝采集而不是交出错图 |
| Rust 输入钩子 / 键帽渲染 | 低 | — | **已完成 X11 部分** | XInput2 观测（见 9.4.4）+ `ab_glyph` 光栅化（见 9.4.5）；Wayland / 无 XInput2 退路仍缺 |
| Rust 元素拾取 | 低 | — | **已完成窗口级** | X11 窗口树 + `_NET_*`（见 9.4.6）；控件级仍需 AT-SPI2 |
| Rust 音频 | 低（全为 `UnsupportedBackend`） | — | **范围外（决策 4）** | 不做：只在 Win/mac 提供录制音轨，Linux 上诚实上报不可用 |
| Rust OCR（`snow-ocr-process`） | 中 | — | **中改** | 默认 `directml-provider` 为 Windows 专有 |

### 0.3 必须向决策方澄清的前置问题

1. **Qt5 的下限版本**：建议锁定 **Qt 5.15 LTS**。UOS 20 出厂为 Qt 5.11，`deepin-shared-libs` 另提供
   Qt 5.15.10 GLES 供 UOS v20 amd64/aarch64 使用；Deepin 23 基线为 Qt 5.15 + 6.x。
   若被迫降至 5.12/5.11，`QStringDecoder` / `QLatin1StringView` / `QByteArrayView` / `qsizetype` /
   `QColorSpace` / `QImageReader::allocationLimit` 等将再增一大层替换工作。
2. **`snow_image_viewer` 是否在移植范围内**：它是唯一使用 `QRhi` 的模块，Qt5 下无公开等价 API。
   建议 **Linux 首版不交付查看器**，或将其渲染后端重写为 `QOpenGLWidget`。
3. ~~**Qt5 约束的来源**~~ → **已定，见 §0.4。**

### 0.4 决策记录（已确认）

**决策 1：维持 Qt5 路线。** 不做 AppImage + Qt6 的绕行（该路线作为备选保留在附录 A，已否决）。

**决策 2：保留 Windows 分支，先本机验证再上 UOS。** 即先在 Windows 用 Qt5 打通编译与冒烟测试，
确认降级本身没引入回归，再进 UOS 做平台后端适配。

**决策 3（由决策 2 推导出的改造形态）：不删除 Qt6 支持，改为双版本兼容。**
依据是 Qt 官方的 versionless 机制——`Qt::Core` 这类无版本前缀的目标**在 Qt 5.15 与 Qt 6 中同时存在**，
因此 `Qt6::X` → `Qt::X` 是一次双向生效的官方重命名，而非单向降级。
这带来两个直接收益：现有 Qt6 构建全程保持可用（可作二分对照基线，快速区分"降级引入的 bug"与"原有 bug"），
且回滚成本极低。

**降级工作量实测**（当前 `main` 快照 grep 统计）：需改造 **112 个 C++ 文件** + **882 行 CMake**，
占非测试 C++ 文件总量的 8.6%。完整清单、兼容层代码、阶段计划与 Windows 环境前置
全部落在 **[`docs-qt5-downgrade-plan.md`](docs-qt5-downgrade-plan.md)**。

**决策 4：Linux 不做音频采集。** 录屏在 UOS 上只录画面。理由不是"做不了"，而是代价与收益不成比例：
PulseAudio / PipeWire 的客户端库全是 C 绑定，一旦引入，「平台后端只用纯 Rust」这条铁律就破了，
此后音频路径**没有任何能在 Windows 上跑的门禁**，只能靠真机反复试。而 `snow-audio-recorder`
在 Linux 上本来就已经是 `UnsupportedBackend`，保持"如实上报不可用"比给一个无法验证的实现更诚实。

> ⚠️ **新增前置阻塞**：经实测，本机**未安装 Qt / CMake / MSVC / Ninja / Rust / vcpkg，且仓库从未在本机构建过**。
> 因此 Windows 验证前必须先完成环境 bootstrap（约 10–12 GB 下载、数小时），这是不可绕过的门槛。

---

## 1. 工程整体架构

### 1.1 顶层构成

```
snow-apps/                             (cmake_minimum_required 4.2, project snow_apps 0.2.0)
├── CMakeLists.txt                     顶层编排：模块开关、IPO/LTO、CPack(NSIS)
├── CMakePresets.json                  16 个 configure preset：windows-msvc-* / macos-*
├── vcpkg.json                         清单依赖：snow-shot / full-codecs / image-viewer features
├── vcpkg-configuration.json           builtin registry, baseline 4497409a
│
├── snow_shot/           (GPL-3.0)     主程序：截图/贴图/OCR/录屏；1084 个 C++ 文件
├── ant_design_qt/       (Apache-2.0)  Qt Widgets 组件库；369 个 C++ 文件 + 4 个 .mm
├── snow_draw_engine_qt/ (Apache-2.0)  绘图引擎：C++ 薄壳 + 9 个 Rust crate
├── snow_image/          (GPL-3.0)     图像编解码库（PNG/JPEG/GIF/HEIF/JXL/WEBP/AVIF...）
├── snow_image_viewer/   (GPL-3.0)     看图器：QRhi 渲染（Qt6 专有）
├── snow_memory/                       显式 OS 页所有权的大缓冲
├── snow-crates/         (Apache-2.0)  Rust workspace，31 个 crate
├── snow_rust_ffi/       (Apache-2.0)  汇总 C ABI staticlib，供 CMake 链接
└── cmake/ scripts/                    共享 CMake 模块、vcpkg overlay、PowerShell/sh 入口
```

### 1.2 依赖分层（自下而上）

| 层 | 组件 | 平台耦合 |
|---|---|---|
| L0 内存/基础 | `snow-memory`、`snow-core`、`snow-media`、`snow-diagnostics` | 低（Linux `mmap` 已有） |
| L1 图像 | `snow_image`、`snow-stitch-images`、`visual-region-detector`、`snow_draw_engine_qt`(Rust) | 低-中（SIMD 为 x86） |
| L2 平台后端 | `snow-capture`、`snow-cursor`、`snow-audio-recorder`、`snow-ui-selector`、`snow-selected-text`、`snow-recording-effects` | **极高**（Win/macOS only） |
| L3 录制/导出 | `snow-recording-runtime`、`snow-recording-export`、`snow-screen-recorder`、`snow-pipeline` | 高（GPU 路径 Windows only） |
| L4 OCR | `rapid-ocr-rs`、`snow-ocr-process`、onnxruntime | 中（DirectML Windows only） |
| L5 C ABI | `snow-*-c` × 8 → `snow-shot-rust-ffi`(staticlib) | — |
| L6 C++ UI | `ant_design_qt` → `snow_shot` | 高（平台层、.mm、Win32） |

### 1.3 构建与打包链路

- **Qt 供给**：**不使用 aqtinstall / vcpkg**。由 `scripts/qt-toolchain.json` 单一真源锁定 `6.12.0`，
  `cmake/SnowQt.cmake:29` 执行 `find_package(Qt6 <ver> EXACT REQUIRED)`，版本不符即 `FATAL_ERROR`；
  `SnowQt.cmake:39` 注入 `QT_DISABLE_DEPRECATED_UP_TO=0x060c00`（Qt 6.5+ 专有宏）。
  静态 Qt 由 `scripts/build-static-qt.sh:143-160` 从官方 tarball 自建，子模块仅
  `qtbase,qtsvg,qttools,qttranslations`，并 `-skip qtdeclarative`。
- **原生依赖**：vcpkg manifest 模式，triplet 仅 `{x64,arm64}-{windows,osx}-*`（**无 Linux triplet**）。
- **Rust 集成**：`cmake/RustStaticLibrary.cmake` 生成 `cargo build --locked --package ... --target ...` 自定义目标，
  产物以 `STATIC IMPORTED` 挂到 CMake；`snow_rust_ffi` 单库聚合全部 C ABI。
- **打包**：Windows `NSIS`（根 `CMakeLists.txt:426-485`），macOS `DragNDrop`
  （`cmake/SnowShotMacOSPackage.cmake`）。**无 Linux/deb/AppImage 配置。**

---

## 2. 跨平台兼容性问题清单

### 2.1 Linux 现状：几乎为零

全仓库源码级 `Q_OS_LINUX` 仅 **2 处**（`snow_image_viewer/src/ui/viewer_window.cpp:94,126`）。
X11 / Wayland / PipeWire / portal / D-Bus / PulseAudio / ALSA / EGL 在应用代码中 **0 处**。
缺平台实现时当前行为是「静默降级为桩」：

```cpp
// snow_shot/src/platform/globalmousebackend.cpp:35-37
#else
    return std::make_unique<UnsupportedGlobalMouseBackend>();   // 权限状态 Unavailable
#endif
```

```rust
// snow-crates/crates/snow-capture/src/platform/mod.rs:17-22
#[cfg(not(any(target_os = "windows", target_os = "macos")))]
fn unsupported_error() -> CaptureError {
    CaptureError::platform(anyhow::anyhow!("screen capture is only supported on Windows"))
}
```

> 风险：若不改造，Linux 版会**编译通过但核心功能全部失效**（截不到屏、录不了音、抓不到光标）。
> 这与用户对"误导性误报"的敏感点直接冲突（不允许出现"工具已安装但无法获取屏幕"这类误导提示），
> 因此在接口层必须显式上报 `Unsupported`，而非伪造成功。

### 2.2 Windows 专属代码（必须替换）

| 类别 | 代表位置 | Linux 对应 |
|---|---|---|
| DXGI Desktop Duplication / WGC | `snow-capture/src/platform/windows/{duplication.rs,wgc.rs,duplication_gpu.rs}` | PipeWire / X11 XShm |
| D3D11 / GPU tonemap | `snow-crates/crates/snow-d3d11/*`；`snow-capture/.../gpu_tonemap.rs`（≈180 处） | Vulkan/EGL 或纯 CPU |
| GDI 回退 | `snow-capture/src/platform/windows/gdi.rs` | X11 `GetImage` —— **已落地**：`snow-linux/src/x11.rs` + `snow-capture/src/platform/linux.rs`（见 9.4.3） |
| WASAPI 音频 | `snow-audio-recorder/src/platform/windows/*` | PulseAudio / PipeWire |
| UIA/MSAA 元素拾取 | `snow-ui-selector/src/windows/*`、`snow-selected-text/src/platform/windows/*` | **X11 窗口树 —— 已落地窗口级**：`snow-linux/src/window.rs`（纯逻辑）+ `snow-linux/src/x11/window.rs`（见 9.4.6）。**跨进程取选中文字已落地 PRIMARY selection**（见 9.4.22）；**AT-SPI2 已接进 `snow-selected-text`**（`snow-atspi` crate，见 9.4.25/9.4.26）：控件级文本 + 选中矩形 + 按 `_NET_WM_PID` 定位应用，PRIMARY 作退路。**控件级拾取已接进 `snow-ui-selector`**（见 9.4.27）：`Component.GetAccessibleAtPoint` 点命中下探，返回「控件→…→窗口」路径，读不到时回退窗口矩形 |
| 全局键鼠钩子 | `snow-recording-effects/src/{keyboard_hook.rs:124, mouse_hook.rs:90}` | **XInput2 raw events —— 已落地**：`snow-linux/src/input/`（见 9.4.4）。待补：无 XInput2 时的 XRecord / evdev 退路、Wayland portal |
| DirectWrite + Direct2D 键帽光栅化 | `snow-recording-effects/src/keyboard_rasterizer.rs:8`（macOS 走 CoreText + Quartz） | **`ab_glyph` + `ttf-parser` —— 已落地**：`snow-linux/src/text/`（见 9.4.5）。字体发现自建，**刻意不链 fontconfig** |
| Win32 窗口层 | `snow_shot/src/platform/windows/*`（30 文件）：`windowchrome.cpp:255` `SetWindowDisplayAffinity`、`pinnedwindownative.cpp` `WS_EX_*` | X11 属性 / 无对应 |
| Win32 物理光标 | `platform/physicalcursor.cpp:26-39`（`GetPhysicalCursorPos` / `SetPhysicalCursorPos`）、macOS 侧 `CGEventGetLocation` / `CGWarpMouseCursorPosition` | **XQueryPointer / XWarpPointer —— 已落地**（见 9.4.8）：`snow_linux::x11::pointer` + C ABI `snow_linux.h`。**刻意不用 XFixes**，因为这两个请求是核心协议，缺扩展的服务器也能用 |
| 注册表自启动 | `platform/windows/autostartregistration.cpp:14` | `~/.config/autostart/*.desktop` |
| ShellExecute 提权 | `platform/windows/administratorlaunch.cpp:25-31` | `pkexec` / polkit |
| 资源管理器右键菜单 | `platform/windows/shellcontextmenuregistration.cpp:20-21` | `~/.local/share/applications/*.desktop` |
| 命名管道 | `app/main.cpp:379`、`app/mcp/screenshotmcpserver.cpp:31` | QLocalServer abstract socket |
| crashpad | `diagnostics/crashcollector.cpp`、`app/main.cpp:440-453` | 移除 / Breakpad-Linux |
| DirectML | `rapid-ocr-rs/src/runtime/{provider.rs,session.rs}` | CPU EP / OpenVINO / CUDA(amd64) |
| WIC/D3D11 打印 | `platform/windows/nativeprintdocument.cpp`、`snow_image_viewer/src/platform/windows_print.cpp` | CUPS / QPrinter |
| `winres` 无条件 build-dep | `snow-ocr-process/build.rs:35-36` | 需 target-gate |

### 2.3 macOS 专属代码（移植时可整目录排除）

- 19 个 `.mm`：`snow_shot/src/platform/macos/*`（`NSWindow`/`CGEvent`/`NSPasteboard`/`AVFoundation`...）；
- 4 个 `.mm`：`ant_design_qt/.../widgets/{context_menu_mac,modal_mac,detail/*_mac}.mm`；
- 整 crate `snow-crates/crates/snow-macos/`（22 文件，依赖 16 个 `objc2-*`）；
- `snow-capture/src/platform/macos.rs`、`snow-audio-recorder/src/platform/macos.rs` 等。

> 这些在 Linux 构建中可以 `if(APPLE)` 排除，但 **`snow_shot/CMakeLists.txt:1-8` 目前在 `APPLE` 下
> 全局启用 `LANGUAGE OBJCXX`**，需确保 Linux 分支不触发。

### 2.4 硬件 / 架构差异

| 项 | 现状 | arm64 影响 |
|---|---|---|
| SIMD 内核 | 全部 AVX2/SSE（`snow-capture/src/convert/simd_x86.rs` ≈600 处；`snow-stitch-images/{orb,region,estimator}.rs`；`rapid-ocr-rs/src/det/{pre,post}process.rs`；`snow-recording-export/src/{resize,editing}.rs`；C++ 侧 `snow_canvas_filter_avx2.cpp` ≈188 处 `_mm256_*`、`snow_image/src/raster.cpp:135,168`） | **均有标量回退可编译，但性能显著下降**，需补 NEON |
| 已有 NEON 范式 | `snow-media/src/convert.rs:46-76` | 可作为移植模板 |
| CMake SIMD | `cmake/SnowSimd.cmake:7-23`（MSVC `/arch:AVX2`、GCC/Clang `-mavx2`） | 需加 aarch64 分支（`-march=armv8-a+simd`） |
| 内存大页 | `snow-memory/src/pages.rs:30-44` `VirtualAlloc`+`MEM_LARGE_PAGES` | Linux 需 `mmap(MAP_HUGETLB)`，现有分支未用大页 |
| 指针宽度 | 已强制 64 位单架构（根 `CMakeLists.txt:55-86`） | — |

### 2.5 Qt 6 专有特性（**本方案的最大阻塞**）

> 全仓库**没有一处 `#if QT_VERSION_CHECK` 兼容分支**，即无法"一套代码双版本编译"，必须新增兼容层或分叉。

| 类别 | 代表 API / 文件 | Qt5 替代 |
|---|---|---|
| **RHI 渲染** | `snow_image_viewer/src/render/rhi_image_window.{h:23-48,222,cpp}`（`QRhi*`/`QVulkanInstance`/`QOffscreenSurface`） | **Qt5 无公开 RHI** → `QOpenGLWidget` 重写 |
| 键序列 | `QKeyCombination`：`shortcutbinding.h:4`、`shortcutbinding.cpp:375,432,623,650`、`windowshortcutmanager.cpp:46-47,508-596` 等 15+ 文件 | Qt5 `QKeySequence` 用 `int` |
| 原生接口 | `QNativeInterface::QWindowsScreen/QCocoaScreen`：`monitorgeometry.cpp:18`、`focusedfullscreenwindow.cpp:61`、`screenshotgeometry.cpp:373,873` | Qt5 `QPlatformNativeInterface` |
| 进入事件 | `enterEvent(QEnterEvent*)`：`titlebarwidget.cpp:20,97,154`、`tag.h:13,220` 等 | Qt5 `enterEvent(QEvent*)` |
| 指针事件 | `QMouseEvent::position()/globalPosition()`：`screenshotoverlaywindow.cpp:1100-1220`、`screenshotpinnedwindow.cpp` 等 15+ 文件 | `localPos()/globalPos()` |
| 字符串视图 | `QStringView`（11 文件）、`QLatin1StringView`(`configurationarchive.cpp:132,403-408`)、`QByteArrayView`(`worker_protocol.cpp:61,137`) | `QString`/`QLatin1String`/`QByteArray` |
| 编解码 | `QStringDecoder`（`screenshotclipboardcontent.cpp:38,549-551`） | `QTextCodec` |
| 元类型 | `QMetaType::fromType` / `QVariant::metaType()`：`mcpsettingsadapter_p.h:47-76`、`image.cpp:443`、`select_option_utils.cpp:18,37` | `QVariant::type()` / `QMetaType::Type` |
| 容器 | `QList::removeIf`：`settingsadapters.cpp:112`、`configurationschema.cpp:37-2273` | 手写 `std::remove_if`+`erase` |
| 协程 | `QPromise`（`mcpasyncwork_p.h:8,25`，Qt6 移入 QtCore） | Qt5 在 QtConcurrent 中 |
| 色彩 | `QImage::convertedToColorSpace`、`QColorSpace::Primaries::*`：`snowimageqtcodec.cpp:264-312` | Qt5.14 部分可用，命名不同 |
| 图像限制 | `QImageReader::allocationLimit()/setAllocationLimit()`：`snowimageqtcodec.cpp:598,619` | Qt5 无此静态 API |

---

## 3. Qt5 降级专项方案

### 3.1 Qt 模块可用性对照

| 现用 Qt6 模块 | Qt5 对应 | 结论 |
|---|---|---|
| Core / Gui / Widgets / Network / Svg / Test | `Qt5::*` | 直接可用 |
| Concurrent | `Qt5::Concurrent` | 可用（注意 `QPromise` 位置） |
| LinguistTools | `Qt5::LinguistTools` | 可用，但构建期 API 不同（见 3.3） |
| GuiPrivate | `Qt5::GuiPrivate` | 可用但私有 ABI，需逐点核对 |
| **未使用**：Quick / Qml / Multimedia / DBus / Sql / PrintSupport / OpenGL(Widgets) | — | 无需处理 |

> 有利面：**无 QML / Qt Quick、无 `.pro`、无 `.ui`**，`CMAKE_AUTOMOC/AUTORCC` 已启用。
> 这消除了 Qt5↔Qt6 差异中最大的两块（QML 引擎与 qmake）。

### 3.2 CMake 改写表

| Qt6 专有 | 出现位置 | Qt5 改写 |
|---|---|---|
| `find_package(Qt6 EXACT)` | `cmake/SnowQt.cmake:29` | `find_package(Qt5 5.15 EXACT)` |
| `QT_DISABLE_DEPRECATED_UP_TO` | `cmake/SnowQt.cmake:39` | `QT_DISABLE_DEPRECATED_BEFORE` |
| `Qt6::qtpaths` | `SnowQt.cmake:61` | `Qt5::qmake` / `Qt5Core_VERSION` |
| `qt_standard_project_setup()` | `snow_draw_engine_qt/CMakeLists.txt:36` | 手工设 `CMAKE_AUTOMOC` 等 |
| `qt_add_library()` | `snow_draw_engine_qt/CMakeLists.txt:64` | `add_library()` |
| `qt_add_executable()` | `snow_shot/CMakeLists.txt:1596`、`SnowShotMini.cmake:113` | `add_executable()` |
| `qt_add_resources(PREFIX/BASE/FILES)` | `snow_shot/CMakeLists.txt:1153,1664,1685`、`SnowShotTranslations.cmake:78` | `qt5_add_resources(<qrc>)` |
| `qt_add_translations/lupdate/lrelease` | `ant_design_qt/CMakeLists.txt:153`、`SnowShotTranslations.cmake:96`、`SnowShotTranslationRelease.cmake:18-31` | 直接调 `lupdate`/`lrelease`，手工实现合并 |
| `qt_import_plugins()` | `SnowQtOffscreenTest.cmake:31`、`snow_shot/CMakeLists.txt:1792,2975` | `Q_IMPORT_PLUGIN(...)` 宏 |
| `qt_generate_deploy_app_script()` / `qt_finalize_executable()` | `SnowShotMini.cmake:276,283` | Linux 无对应（用 CPack/手工） |
| 静态插件名 `Qt6::QSvgPlugin` 等 | `snow_shot/CMakeLists.txt:153-172` | Qt5 静态插件需 `Q_IMPORT_PLUGIN` + 手工链接 |

### 3.3 i18n 构建管线

运行时（`QTranslator` + `.qm` 资源）Qt5/6 一致，**可原样保留**。构建期需改写：
`MERGE_QT_TRANSLATIONS` / `QT_TRANSLATION_CATALOGS` / `QT_INTERNAL_ABSOLUTE_TRANSLATIONS_DIR`
（Qt6 专有）→ 改为脚本显式调用 `lrelease`；Qt 自带翻译文件名由 `qtbase_*.qm` 改为 `qt_*.qm`
（`SnowShotTranslationRelease.cmake:8-14`）。`lrelease -fail-on-unfinished` 策略保留。

### 3.4 RHI 重写（查看器）

`rhi_image_window.{h,cpp}` 使用 `QRhiBuffer/QRhiGraphicsPipeline/QRhiSwapChain/QVulkanInstance`。
Qt5 仅有私有 RHI，**公开 API 无法编译**。三条路径：

| 方案 | 成本 | 说明 |
|---|---|---|
| **A. QOpenGLWidget 重写**（推荐） | 中 | 复用现有 GLSL 着色器（`snow_image_viewer/resources/shaders/*.frag|vert`），改写为 GL 3.3 Core |
| B. QPainter/QImage 软渲染 | 低 | 舍弃 GPU 加速，仅保证功能 |
| C. Linux 首版不交付查看器 | 最低 | 从 preset 关闭 `SNOW_APPS_BUILD_IMAGE_VIEWER` |

### 3.5 兼容层设计（推荐落地形态）

新增 `snow_shot/include/snow_shot/compat/qt5compat.h`（+ 同名 `.cpp` 可选），集中收敛差异：

```cpp
// 设计示意（非最终代码）
#pragma once
#include <QtGlobal>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#  include <QKeyCombination>
#  include <QStringDecoder>
#  define SNOW_HAS_QT6 1
#else
#  define SNOW_HAS_QT6 0
#endif

namespace snow_shot::compat {
#if SNOW_HAS_QT6
inline int keySequenceKey(const QKeySequence& s) { return s[0].toCombined(); }   // 占位
#else
inline int keySequenceKey(const QKeySequence& s) { return s[0]; }
#endif
} // namespace
```

约定：**所有平台差异集中在 `compat/`，业务代码只调用 `compat::` 前缀 API**，避免把
`#if QT_VERSION` 散落到 1000+ 文件。

---

## 4. 需要修改 / 新增的模块清单

### 4.1 C++ 侧

| 动作 | 路径 | 说明 |
|---|---|---|
| **新增** | `snow_shot/src/platform/linux/` | 对应 `windows/` 全部能力：全局快捷键、全局鼠标、物理光标、全屏窗口检测、窗口捕获排除、窗口 chrome、托盘、单实例、自启动、提权、剪贴板、系统通知、滚动注入。**例外**：物理光标没有落在这里 —— `physicalcursor.cpp` 的 Linux 分支直接调 `snow-linux-c`，因为那块逻辑的价值在于可离线单测，放进纯转发目录反而失去意义（见 9.4.8） |
| **修改** | `snow_shot/src/platform/focusedfullscreenwindow.cpp:39`、`globalmousebackend.cpp:5-38`、`physicalcursor.cpp:13-97` | 增加 `Q_OS_LINUX` 分支。**`physicalcursor.cpp` 已完成**（见 9.4.8）；其余两个仍是通用桩（全屏检测恒 false、全局鼠标 `Unavailable`） |
| **修改** | `snow_shot/src/presentation/{globalcanvas/globalcanvasplatform.cpp, pinned/pinnedwindowplatform.cpp}` | Win/Mac 分发 + 桩，需 Linux 实现 |
| **重构** | `snow_shot/include/snow_shot/platform/screenshotnative.h:3` | **无条件 include `windows/scrollinput.h`**，须拆出平台中立接口 |
| **修改** | `ant_design_qt/.../widgets/{context_menu,modal}.cpp` + `detail/*` | Win32/`.mm` 分发，需 X11 对应 |
| **重写/裁剪** | `snow_image_viewer/src/render/rhi_image_window.*` | 见 3.4 |
| **新增** | `snow_shot/resources/linux/*.desktop`、图标 | 应用入口、右键菜单。**已落**（见 9.5.1/9.5.2）：入口放在 `snow_shot/packaging/linux/`；图标复用 `resources/app-icon.{svg,ico}`，PNG 尺寸由 `scripts/gen-linux-icons.py` 从 ico 抽出 |
| **新增** | `cmake/SnowShotLinuxPackage.cmake`、`cmake/SnowShotAppImage.cmake` | CPack DEB / AppDir + `appimagetool` |

### 4.2 Rust 侧

| 动作 | 路径 | 说明 |
|---|---|---|
| **新增** | `snow-crates/crates/snow-linux/` | 替代 `snow-macos` 的平台 crate：X11/Wayland 屏幕、音频、光标、输入、文字。**已建**：`geometry`/`pixels`/`session`/`capture`/`damage`/`window`/`pointer`（纯逻辑，任何平台可单测）+ `input::{keys,tracking}`（纯逻辑：keysym→虚拟键映射、键码表、点击/区域状态机）+ `text::{font,raster}`（纯逻辑：字体发现 + 键帽光栅化，`ab_glyph`/`ttf-parser`）+ `portal::{deadline,...}`（纯逻辑：portal 请求路径、版本门控、应答判定、流列表解析、握手顺序）+ `x11`/`display`/`cursor`/`input::x11`/`x11::{window,shm,damage,pointer}`（X11，`x11rb`）+ `portal::dbus`（session bus，`zbus`）。**AT-SPI2 不在这里**：9.4.25 曾落在 `a11y`，9.4.26 抽成独立 crate `snow-atspi`（两个 C archive 都是它的消费者，其中 `snow-selected-text` 刻意不依赖本 crate）。待补：PipeWire 帧消费、evdev 退路。**音频按决策 4 不做** |
| **新增** | `snow-crates/crates/snow-atspi/` | AT-SPI2 访问层，只依赖 `zbus`（且仅 Linux）：`lib.rs` 纯逻辑（`ObjectRef`/空 bus name 继承、state 值、字符↔字节、`choose_selection`、`Bounds`、有界且有期限的 BFS、按 pid 找应用；单测在 Windows 上跑）+ `bus.rs` 传输（`org.a11y.Bus.GetAddress` → 第二条连接 → `GetChildren`/`GetState`/`GetText`/`GetRangeExtents`）。见 9.4.25（能力）与 9.4.26（抽离与接线）。**独立成 crate 的理由**：`snow-selected-text-c` 与 `snow-ui-selector-c` 是两个 archive，前者刻意不依赖 `snow-linux`，一份实现才不会漂移 |
| **新增** | `snow-crates/crates/snow-linux-c/`（`include/snow_linux.h`） | Rust↔C++ 平台桥：C ABI 头 + `staticlib`。**已建**（见 9.4.8）：`create`/`destroy` + 指针读写。刻意与 `snow_rust_ffi` **分成两个 archive** —— 后者经 `zstd-sys` 撞 C 源码，跨目标检查在依赖图里就断了，走不到这段 Linux-only 代码；这个 archive 只依赖纯 Rust 的 `snow-linux`，因此每次 `bash scripts/linux-cross-check.sh` 都覆盖它 |
| **新增** | `snow-crates/crates/snow-capture/src/platform/linux.rs` | **X11 已落地**（`LinuxBackend` + `X11Capturer`，见 9.4.3）：显示器枚举 / 主屏 / `MonitorLayout` / `inspect_target` + 全屏与区域采集；XShm 零拷贝（9.4.7）；Damage 跳帧与脏矩形（9.4.19）；区域采集只读子矩形（9.4.23）；**Wayland 会话按 `capture_plan` 明确拒绝**（9.4.20）。待补：PipeWire 帧消费（portal 会话协商已落地） |
| **新增** | `snow-crates/crates/snow-audio-recorder/src/platform/linux.rs` | ~~PulseAudio / PipeWire~~ **不做（决策 4）**：保持 `UnsupportedBackend`，Linux 录制无音轨 |
| **新增** | `snow-crates/crates/snow-cursor/src/platform/linux.rs` | XFixes `GetCursorImage` + 核心 `QueryPointer`。**已完成**（含 image 缓存的瞬时失败保护，与 Windows 后端同策略） |
| **新增** | `snow-crates/crates/snow-ui-selector/src/linux.rs` | **窗口级已落地**（见 9.4.6）：X11 窗口树 + `_NET_CLIENT_LIST`，`ElementRegionService` 在 Linux 上导出，`snow-ui-selector-c` 恢复可编译。**控件级已落地**（见 9.4.27）：`HitTestMode::UiElement` 经 `snow-atspi` 的点命中下降返回「控件→…→窗口」；读不到时回退窗口矩形（`HitTestMode::Window` 语义） |
| **新增** | `snow-crates/crates/snow-selected-text/src/platform/linux/` | **X11 PRIMARY selection 已落地**（见 9.4.22）：`selection.rs`（纯逻辑：目标优先级、`INCR` 判据、解码、上限）+ `platform/linux/{selection,source}.rs`（传输与活动窗口识别）。被动读、零注入 ⇒ `clipboard_status` 恒 `Unchanged`。**AT-SPI2 已接线**（见 9.4.26）：先读焦点控件的 `Text` 选中（带矩形），没有才退回 PRIMARY；`method` 报真正答话的那条路。待补：真机验证 |
| **修改** | `snow-recording-effects/src/{keyboard_hook,mouse_hook}.rs` 的 `#[cfg(not(any(windows,macos)))]` 空实现 | **已落地 X11 实现**（见 9.4.4）：`#[cfg(target_os="linux")] mod platform` 把 `snow_linux::input` 的观测翻译成 `KeyObservation` / `MouseClickObservation` / `MouseMovement` |
| **修改** | `snow-recording-effects/src/keyboard_rasterizer.rs:547` 的 `#[cfg(not(any(windows,macos)))]` 桩 | **已落地**（见 9.4.5）。**注意**：该文件不是纯逻辑 —— 每个平台各自实现 `mod platform`（Windows 是 DirectWrite+D2D，macOS 是 CoreText+Quartz），Linux 分支原先是硬返回 Err ⇒ 按键能采到但画不出来。现已接 `snow_linux::text` |
| **修改** | `snow_rust_ffi/{Cargo.toml,src/lib.rs}` | `snow-ui-selector-c` 的依赖与 `pub mod ui_selector` 门控从 `cfg(any(windows, target_os="macos"))` 放宽到 `cfg(any(windows, unix))`。不改的话，Linux 上 C++ 那头 `snow_ui_selector_service_*` 会在链接期全部 undefined。**本机无法验证**：该 archive 在 Linux 目标上连 `cargo check` 都过不去（见 9.4.2 A 类），只能到真机验 |
| **修改** | `snow-recording-runtime/src/{capture_policy.rs:18-90,direct_capture.rs:4-90}` | 放开 Windows-only 门控，接入 CPU 合成路径 |
| **修改** | `rapid-ocr-rs/src/model_store.rs:94-102` | `LOCALAPPDATA` → XDG (`$XDG_DATA_HOME`) |
| **修改** | `snow-ocr-process/build.rs:35-36` | `winres` 改为 `[target.'cfg(windows)'.build-dependencies]` |
| **重写/排除** | `snow_shot/rust/snow-shot-updater`、`snow-shot-mcp` | 纯 Windows crate（注册表/服务/`LoadLibrary`） |
| **修改** | `rust-toolchain.toml` | `targets` 增加 `x86_64-unknown-linux-gnu`、`aarch64-unknown-linux-gnu` |

---

## 5. 依赖库替换 / 调整

### 5.1 vcpkg → UOS 系统包 / 自建

| 依赖 | 现状 | Linux 方案 |
|---|---|---|
| Qt | 源码自建 6.12.0 | **系统 Qt 5.15**（deepin-shared-libs Qt5.15.10 / apt `qtbase5-dev`） |
| FFmpeg | vcpkg overlay port 9.0 | UOS 源 `libavcodec-dev` 等，或自建（Deepin23 自带 6.1.1） |
| OpenCV | vcpkg `opencv4`(contrib/dnn/fs/intrinsics) | **自建 4.12.0**（opencv + opencv_contrib，与 overlay port 同源同补丁，见 9.6.20）；发行版的 4.6 不可用 |
| onnxruntime | vcpkg overlay + DirectML | 官方预编译 `libonnxruntime.so`（CPU EP）或 OpenVINO |
| libpng/libjpeg-turbo/libwebp/giflib | vcpkg | UOS 源同名 `-dev` |
| libheif / libjxl / openexr / lunasvg | vcpkg | UOS 源或自建（注意 aom/hevc 特性） |
| tinyxml2 / minizip-ng / flatbuffers / abseil | vcpkg | UOS 源 |
| **crashpad** | `platform: windows \| osx` | Linux **移除**，或替换为 Breakpad/Breakpad-Linux |
| **DirectML** | `ort/directml` | **移除**（Windows 专有） |
| **Vulkan** | `vulkan-headers`(image-viewer) | `libvulkan-dev`（若走 RHI→GL 则不需要） |

### 5.2 需新增的 Rust 依赖

| 用途 | crate |
|---|---|
| 屏幕采集（Wayland） | `zbus`（xdg-desktop-portal ScreenCast，**已引入：v6，协商层见 9.4.20**）、`pipewire-rs`（`libspa`，帧消费，未引入） |
| 屏幕采集（X11） | `x11rb` 或 `xcb`、`xfixes`、`xrecord` |
| 音频 | `libpulse-binding` / `pipewire-rs` |
| 输入钩子 | `evdev`、`input-linux`、或 `x11rb` XRecord |
| 底层系统 | `nix`、`libc` |
| 可访问性 | `atspi` / `zbus`（AT-SPI2 over D-Bus） |
| 硬件编码（amd64） | FFmpeg `h264_vaapi`/`h264_nvenc` 经由 `ffmpeg-next` 绑定 |

> 已有：`x11rb`（X11 全栈）、`zbus`（portal 协商）、`ab_glyph`/`ttf-parser`（文字）、`memmap2`（MIT-SHM）。
> 仍缺：`pipewire-rs`（Wayland 帧消费）、`nix`/`libc`、`evdev`、`atspi`。
> 注意 `zbus` 6 的两处破坏性变化：`zvariant` 并入 `zbus::wire`（`Value` → `Variant`），blocking API 被移除，
> 同步接送只能 `zbus::block_on` 驱动 future。

---

## 6. 系统调用 / 硬件差异适配对照表

| 能力 | Windows 实现 | macOS 实现 | **Linux 目标实现** |
|---|---|---|---|
| 屏幕采集 | DXGI Duplication / WGC | ScreenCaptureKit | **X11 `GetImage` + MIT-SHM —— 已落地**（见 9.4.3 / 9.4.7）；Wayland 的 portal **协商**已落地（见 9.4.20），**PipeWire 帧消费仍缺** ⇒ Wayland 明确拒绝采集 |
| 窗口采集 | WGC per-window | SCK per-window | PipeWire `restore_token` / X11 Composite |
| 防截屏排除 | `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)` | `NSWindowSharingNone` | **无等价**，X11 用 shape/`XComposite` 近似；建议降级说明 |
| 全局快捷键 | `RegisterHotKey` | Carbon/CGEventTap | **X11 `XGrabKey`** / Wayland `GlobalShortcuts` portal |
| 全局鼠标 | `WH_MOUSE_LL` | `CGEventTap` | **XInput2 raw events —— 已落地**（see 9.4.4）；无 XInput2 时诚实上报不可用 |
| 窗口 / 元素拾取 | UIA / MSAA | `AXUIElement` + `CGWindowList` | **X11 窗口树 + `_NET_CLIENT_LIST` —— 已落地窗口级**（见 9.4.6）。控件级需 AT-SPI2，仍缺 |
| 键帽文字渲染 | DirectWrite + Direct2D | CoreText + Quartz | **`ab_glyph` + `ttf-parser` —— 已落地**（见 9.4.5）。字体发现自建枚举（族名 → 文件），**刻意不链 fontconfig / FreeType**，以保住跨目标类型检查 |
| 物理光标位置 | `GetPhysicalCursorPos` | `CGEventGetLocation` | **XQueryPointer / XWarpPointer —— 已落地**（见 9.4.8）。不用 XFixes：这两个是核心协议请求，缺扩展的服务器也能用 |
| 音频采集 | WASAPI | CoreAudio | ~~PulseAudio / PipeWire~~ **不做（决策 4）**：Linux 录制无音轨 |
| 自启动 | 注册表 Run 键 | SMAppService | **`~/.config/autostart/*.desktop`** |
| 提权 | `ShellExecute(runas)` | Authorization Services | **`pkexec`**（polkit） |
| 剪贴板图像 | DIB / CF_DIBV5 | `NSPasteboard` | **X11 `image/png` 选择** / Wayland `wl_data_device` |
| 托盘 | `Shell_NotifyIcon` | NSStatusItem | **StatusNotifierItem**（D-Bus） |
| 系统通知 | Windows Toast | UNUserNotification | **`org.freedesktop.Notifications`** |
| 单实例 | 命名管道 | 命名端口 | **QLocalServer abstract socket**（Qt 抽象，改名字即可） |
| 内存大页 | `VirtualAlloc(MEM_LARGE_PAGES)` | 无 | **`mmap(MAP_HUGETLB)`** |
| 进程 QoS | `SetProcessInformation` | `pthread_set_qos_class_self_np` | **无等价**（返回 0，`snow-core/src/qos.rs:92-134` 已有分支） |
| 打印 | WIC + D3D11 | NSPrintOperation | **CUPS / `QPrinter`** |
| 崩溃捕获 | crashpad | crashpad | 移除或 Breakpad |
| 滚动注入 | `WS_EX_TRANSPARENT` + wheel | CGEvent | **XTest** / `libei`（Wayland） |

---

## 7. arm64 / amd64 双架构方案

### 7.1 架构支持矩阵

| 架构 | UOS 典型终端 | 构建方式 | Rust target |
|---|---|---|---|
| amd64 (x86_64) | 主流 PC | 原生 | `x86_64-unknown-linux-gnu` |
| arm64 (aarch64) | 飞腾/鲲鹏/麒麟 990 | 原生优先，交叉为备 | `aarch64-unknown-linux-gnu` |

`cmake/RustStaticLibrary.cmake:120-125` **已内置** Linux 架构解析，无需改动即可识别上表。

### 7.2 SIMD 补强优先级

| 优先级 | 位置 | 说明 |
|---|---|---|
| P0 | `snow-capture/src/convert/simd_x86.rs` ↔ aarch64 `simd_arm.rs` | 像素格式转换热路径，影响采集帧率。**已完成**：上游 `392a75a9` 的 `simd_arm.rs` 覆盖 BGRA + f16 + HDR，取代了本移植的 `simd_neon.rs`（后者已在 2026-10-30 删除，见 9.4.18 后续修正）；AArch64 不提供 NT store，`_nt_*` 回落普通内核 |
| P1 | `snow_image/src/raster.cpp:135,168`、`snow_canvas_filter_avx2.cpp` | 绘制/滤镜 |
| P1 | `snow-stitch-images/src/{orb,region,estimator}.rs` | 滚动拼接（可先容忍标量） |
| P2 | `snow-recording-export/src/{resize,editing}.rs` | 录制缩放 |
| P2 | `rapid-ocr-rs/src/det/{pre,post}process.rs` | OCR 后处理 |

`cmake/SnowSimd.cmake` 需新增 aarch64 分支（Neon 默认开启，`-march=armv8-a+simd`）。

### 7.3 交叉编译工具链

```bash
# arm64 交叉（在 amd64 上构建 arm64 包）
sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu
# Rust: rustup target add aarch64-unknown-linux-gnu
# .cargo/config.toml:
# [target.aarch64-unknown-linux-gnu]
# linker = "aarch64-linux-gnu-gcc"
```

> 注意：ARM64 交叉构建**必须在真机（飞腾/鲲鹏）做终验**，尤其是 PipeWire/portal/GPU 路径。

---

## 8. 分步骤实施计划

| 阶段 | 目标 | 关键产出 | 验收标准 |
|---|---|---|---|
| **P0 预研基线** | 验证 Qt5.15 能否编译纯逻辑模块 | 在 UOS 上构建 `snow_image` + `snow_draw_engine_qt` + `ant_design_qt`（暂关平台功能） | 三库链接通过；`snow_image` 单元测试在 x86_64+arm64 通过 |
| **P1 Qt5 兼容层** | 建立 `compat/` + CMake 双版本开关 | `snow_shot/include/snow_shot/compat/qt5compat.h`；`SnowQt.cmake` 增加 `SNOW_QT_MAJOR` 分支 | 一个不含平台代码的目标可在 Qt5/Qt6 下分别编译 |
| **P2 构建系统 Linux 化** | preset / 依赖 / 工具链 | `CMakePresets.json` 增 `linux-*`；`cmake/vcpkg-overlay-triplets/{x64,arm64}-linux.cmake`；`rust-toolchain.toml` 补 target | `cmake --preset linux-x64-debug` 配置成功 |
| **P3 Rust 平台后端** *(进行中)* | 补齐 L2/L3 平台能力 | `snow-linux` crate；capture(X11→PipeWire)、cursor、audio、input hooks、text、window picking | `cargo test -p snow-capture` 在 UOS 通过；能截到主屏。**已达成（slice 1–15）**：`snow-linux`（X11 会话 / RandR / 光标 / `GetImage` 解码 / XInput2 输入观测 / `ab_glyph` 键帽光栅化 / X11 窗口树拾取 / MIT-SHM 零拷贝采集 / Damage 跳帧与脏矩形 / portal ScreenCast 协商）+ `snow-capture` 多屏区域脏矩形（9.4.21）+ `snow-cursor` Linux 后端 + `snow-capture` X11 后端 + `snow-recording-effects` 键鼠观测与键帽渲染 + `snow-ui-selector` Linux 窗口级 provider + `snow-selected-text` X11 PRIMARY selection（9.4.22）+ X11 区域采集只读子矩形（9.4.23）+ X11 采集的 `record_stage_timings` 真正产出（9.4.24）+ AT-SPI2 访问层（9.4.25）+ AT-SPI2 抽成 `snow-atspi` 并接进 `snow-selected-text`（9.4.26）+ AT-SPI2 点命中接进 `snow-ui-selector` 的控件级拾取（9.4.27），双架构跨目标类型检查全绿、纯逻辑单测在 Windows 上跑。**剩余**：PipeWire 帧消费（Wayland 的 portal 协商已落地，见 9.4.20）（**音频按决策 4 不做**） |
| **P4 C++ 平台层** *(代码面完成，待真机)* | 主程序平台层 | `snow_shot/src/platform/linux/*`；3 个 dispatcher 增分支；`screenshotnative.h` 重构 | 截图→标注→贴图闭环在 X11 会话可用。**全部 slice 已落代码（slice 1–9）**：物理光标（9.4.8）、全局快捷键（9.4.9）、全屏窗口检测（9.4.10）、全局鼠标手势（9.4.11）、自启动 XDG autostart（9.4.12）、桌面通知（9.4.13）、托盘 StatusNotifierItem（9.4.14）、单实例端点（9.4.15）、`screenshotnative.h` 三分（9.4.16）、窗口级捕获目标（9.4.17）。**剩余全部是只能在 UOS 上回答的问题**：各片「本片未覆盖」段罗列的真机项、首页卡片拖拽在 X11 上明确不实现（9.4.11 第 7 条） |
| **P5 双架构 + SIMD** *(进行中)* | arm64 可用可跑 | NEON 内核；`SnowSimd.cmake` aarch64 分支 | 飞腾真机截屏成功，帧率 ≥ 目标准。**slice 1 已由上游取代**：BGRA→RGBA 的 NEON 内核现由上游 `simd_arm.rs` 提供（含 f16/HDR），本移植的 `simd_neon.rs` 已删除（9.4.18 后续修正），`--all-features` 在非 Windows 上恢复可编译。**剩余**：§7.2 的 P1/P2 项（`raster.cpp` CRC32C、`snow_canvas_filter_*_avx2`、拼接/录制/OCR 标量容忍）、真机性能数字 |
| **P6 打包与系统集成** *(进行中)* | 可交付安装包 | `cmake/SnowShotLinuxPackage.cmake`；`.desktop`；deb + AppImage | 双架构 deb 在 UOS 20/Deepin 23 安装并可启动。**slice 1 已落代码**：deb（CPack DEB + `dpkg-shlibdeps` 依赖推导 + 五架构映射）、`.desktop`、AppStream metainfo、hicolor 安装规则、门禁 43 项（9.5.1）。**slice 2 已落代码**：7 个尺寸的 hicolor PNG（从 `app-icon.ico` 抽取）、AppImage（`cmake/SnowShotAppImage.cmake` + `AppRun` + `snow-shot-appimage` 目标）、门禁扩到 101 项（9.5.2）。**剩余**：真机 `dpkg -i` 与两个 freedesktop 校验器、AppImage 真机 `--appimage-extract` 与 FUSE 验证、真机性能数字 |
| **P7 硬化与 CI** *(进行中)* | 稳定与回归 | Wayland 会话适配；CI 增 Linux job | 双会话（X11/Wayland）功能对等；关键用例回归通过。**slice 1 已落代码**：`.github/workflows/snow-shot-linux.yml`（`gates` 无工具链必过 + `build` 双架构真编译/deb/校验器，9.6）。**slice 2 已让 workflow 真的跑起来**：GitHub 镜像 + `github` 远程（9.6.8），三次运行把 `gates` 跑绿、把 `build` 推到第 4 个红灯。**第三方库来源已定案并逐个落地**：9.6.9（zlib-ng / libjpeg-turbo / lunasvg / minizip-ng / onnxruntime 等）、9.6.12–9.6.13（OpenEXR+Imath / libwebp / highway / libjxl / libheif）、**9.6.20（OpenCV 4.12 + contrib，自建、与 overlay port 同源同补丁）**。**AppImage 已进 CI**（9.6.25：appimagetool + type-2 runtime 固定版本与校验和，验产物不验工具）。**编译侧已清零**：run 27 的 608/608 个编译单元全过，红落到链接 —— Rust 静态库的档案名与 cargo 实际写出的不一致（9.6.28，已加门禁）；紧接着链接又报出"两个 Rust 静态库 = 两份运行时"与"OpenCV 的 NEON 分发单元从未生成"（9.6.29）。**AppImage 路径的前两处"从未执行过的代码"已修**：`Fetch appimagetool` 调用没传必填参数（9.6.30），暂存的两个源文件因函数体里的 `CMAKE_CURRENT_SOURCE_DIR` 指向仓库根（9.6.31）。**私有库随包带的三件套已落代码**（闭包复制 + RPATH + `SHLIBDEPS_PRIVATE_DIRS`，9.6.33），其中两处"CPack 才会说话"的错在 run 37 跑到 CPack 之前就被静态读码拦下：`file(RPATH_SET)` 的关键字签名、私有库必须按 soname 而非 `REALPATH` 命名（9.6.34）。**deb 路径已全绿**（run 43：`Build the Debian package`、`Validate the packaged metadata`、`Upload the package` 全过；9.6.41/9.6.42）。**AppImage 三步已开始真跑**：`Fetch appimagetool` 过，`Build the AppImage` 的 `cmake -E chmod` 与依赖闭包两处已修（9.6.42）；run 44 里两个架构的 AppImage 都**打出来了**，`Validate the AppImage` 红在门禁脚本自己 `chdir` 后用了相对路径（9.6.44）；run 45 里解包、AppRun、rdns、可执行位、依赖库、AppRun 一致性全过，只剩图标检查把 `Icon=` 当成文件名（9.6.46）。**AppImage 链路已全绿**（run 46：`Build`/`Validate`/`Upload the AppImage` 两个架构全过，9.6.47），其 Qt 依赖定案为**发行版 Qt 5.15**（产物里不含任何 `libQt5*`，AppRun 条件导出 + 两处门禁守住，9.6.45；真正自包含需静态 Qt 或 linuxdeploy-plugin-qt，未决）；`Unit tests` 在 run 46 红于 configure —— 375 处 `$<TARGET_FILE_DIR:Qt::Core>` 在 Qt5 上是 INTERFACE 目标（9.6.47，已修 + 已加门禁）；PipeWire 帧消费 |

**建议节奏**：P0→P2 可并行（构建立即受阻于 Qt5 决策，应最先定论）；P3 与 P4 是最大工作量，建议
**先打通 X11 路径**（技术成熟、调试简单），**再补 Wayland/PipeWire**（UOS 20 默认 X11，Deepin 23 起 Wayland 占比上升）。

---

## 9. 编译构建建议

### 9.1 依赖安装（UOS 20 / Deepin 23 通用）

```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build pkg-config git
# Qt5 开发环境（qttools5-dev 提供 Qt5LinguistTools 的 CMake 配置；
# 只有 qttools5-dev-tools 时 lrelease 存在但 find_package 找不到。
# qttranslations5-l10n 提供 /usr/share/qt5/translations 下的 qtbase_*.qm，
# 缺了它 configure 会在「stock-dialog 目录」那一步直接失败。
# qtbase5-private-dev 提供 <include>/QtGui/<ver>/{private,qpa}：源码真的 include
# 了 <qpa/qwindowsysteminterface.h>、<private/qhighdpiscaling_p.h>，缺它时
# Qt5GuiConfig.cmake 不会建 Qt5::GuiPrivate，19 处链接在配置期就报错，见 9.6.11）
sudo apt install -y qtbase5-dev qtbase5-private-dev qt5-qmake qttools5-dev qttools5-dev-tools \
    qttranslations5-l10n libqt5svg5-dev
# 图形/输入/媒体
sudo apt install -y libgl1-mesa-dev libxcb-xinerama0-dev libxkbcommon-dev libxkbcommon-x11-dev \
    libfontconfig1-dev libfreetype6-dev libharfbuzz-dev \
    libx11-dev libxext-dev libxfixes-dev libxrandr-dev libxi-dev libxtst-dev \
    libpipewire-0.3-dev libpulse-dev libasound2-dev libdbus-1-dev libegl1-mesa-dev
# 图像/编解码
# 注意：**不装** libheif-dev / libjxl-dev。这两个编解码器由 scripts/build-linux-deps.sh
# 带固定版本自编（见 9.6.13），发行版版本比代码老一代；两个来源并存 = 自编失败时静默回落到
# 老版本，唯一正确的做法是让 find_package 大声失败。libde265/x265/aom/brotli/lcms2 要装，
# 它们是自编 HEIF / JXL 的后端（aom 就是 AVIF 编码器，缺它时设置里能选 AVIF、保存时报
# "No encoder is available"，见 9.6.13 的修订）。
sudo apt install -y libpng-dev libjpeg-dev libwebp-dev libgif-dev libtiff-dev \
    libde265-dev libx265-dev libaom-dev libbrotli-dev liblcms2-dev zlib1g-dev libtinyxml2-dev
# FFmpeg（录制/导出链的绑定，§5.1 决定 Linux 用发行版包）
# Rust 侧 ffmpeg-next -> ffmpeg-sys-next 的 build 脚本按 pkg-config 找这六个模块；
# C++ 侧链接同一组。libavfilter 不是可选项：ffmpeg-next 的 "filter" feature
# 对应 CARGO_FEATURE_AVFILTER，缺它时 build 脚本直接 exit(1)。
# 24.04 是 FFmpeg 6.1.1（libavcodec 60.31），ffmpeg-next 9.0.0 支持：探测不到
# ffmpeg_7_0 时会自动走 7.0 之前的 channel-layout 分支（legacy_channel_layout.rs）。
sudo apt install -y libavcodec-dev libavfilter-dev libavformat-dev libavutil-dev \
    libswresample-dev libswscale-dev
# 可选：OCR
sudo apt install -y libopencv-dev
```

**CMake 不能用发行版自带的那个。** 根 `CMakeLists.txt` 是 `cmake_minimum_required(VERSION 4.2)`，
`CMakePresets.json` 里也写着同一个下限；而 Ubuntu 24.04 自带 3.28.3、UOS 20（Debian 10）自带 3.13，
**两者的 preset 读取器都会直接拒绝这个文件**：

```
CMake Error: Could not read presets from ...:
Error: @4,14: "cmakeMinimumRequired" major version 4 must be less than 3
```

注意这条错误发生在 **`cmake --preset` 读文件的时候**，一行项目代码都还没被解析，所以换
`-S/-B` 手动传参也一样过不去（`CMakeLists.txt` 自己也会在 `cmake_minimum_required` 处失败）。
装法（不碰系统包管理器，两种架构通用）：

```bash
python3 -m venv ~/.local/cmake && ~/.local/cmake/bin/pip install "cmake>=4.2"
export PATH="$HOME/.local/cmake/bin:$PATH"   # 先于 /usr/bin/cmake
cmake --version                              # 必须是 4.2+
```

### 9.2 CMake preset（已落地，不再是草案）

以下预设已写入仓库根 `CMakePresets.json`（对应上面 §8 的 **P2**，可直接用）：

| 预设 | 用途 |
|---|---|
| `linux-base`（hidden） | 所有 Linux 预设的公共父级：Ninja、`SNOW_QT_MAJOR=5`、无 `toolchainFile`、`VCPKG_MANIFEST_INSTALL=OFF`、`SNOW_APPS_BUILD_IMAGE_VIEWER=OFF`、移植期 `SNOW_APPS_STRICT_COMPILE=OFF` |
| `snow-shot-linux-x64-debug` / `-release` | UOS 20 / Deepin 23 x86_64 原生构建 |
| `snow-shot-linux-arm64-debug` / `-release` | 飞腾 / 鲲鹏 / 麒麟 990 原生构建 |
| `build-snow-shot-linux-{x64,arm64}-{debug,release}` | 对应的 build preset |

```bash
# 在 UOS 机器上
bash scripts/uos-preflight.sh --probe          # 先跑前置探测（§6.1 的决策点）
cmake --preset snow-shot-linux-x64-debug
cmake --build --preset build-snow-shot-linux-x64-debug          # 或 --target snow_image
ctest --preset test-...                                         # 测试目标见下
```

设计上有三处和常规 preset 不同，都是为了不把 Windows/macOS 主线的假设带进 Linux：

1. **不设 `toolchainFile`**（与 `windows-qt5-base` 同思路）。不挂 vcpkg 工具链 ⇒ manifest 步骤
   根本不运行 ⇒ Linux 用发行版包（§9.1），Windows 用 vcpkg，两边互不干扰。
2. **不写死 `CMAKE_PREFIX_PATH`**。Debian 系（含 UOS）把 Qt5 的 CMake 包放在
   `/usr/lib/<triplet>/cmake/Qt5`，本来就在 CMake 默认搜索路径里；若 UOS 装在自定义前缀，
   再显式 `-DCMAKE_PREFIX_PATH=<prefix>`。
3. **`SNOW_APPS_BUILD_IMAGE_VIEWER=OFF`**，直到 §6.1 的甲/乙路线拍板（见
   `docs-qt5-downgrade-plan.md` §6.1，探测脚本 `scripts/uos-preflight.sh`）。

`SNOW_APPS_STRICT_COMPILE=OFF` 是**移植期**设定：新平台代码加上 Qt5 自带头的告警会在 `-Werror`
下把真正的错误淹掉。Linux 目标全绿后应逐目录打开，最终与 Windows 侧对齐。

#### 9.2.1 源码侧的配套改动（P2 一并完成）

| 文件 | 改动 | 为什么必需 |
|---|---|---|
| `rust-toolchain.toml` | `targets` 增加 `x86_64-unknown-linux-gnu`、`aarch64-unknown-linux-gnu` | 让**任何 Windows 机器**都能对 Linux 目标做静态验证：`cargo check --target x86_64-unknown-linux-gnu` 只需要 std 不需要链接器，等于把跨平台语法/cfg/API 检查前移到写代码的当下 |
| `cmake/SnowWorkspace.cmake` | 补 `Linux` 分支，默认 triplet `x64-linux` / `arm64-linux` | 原来非 Windows/非 macOS 一律落到 `x64-windows`，会把 `SNOW_VCPKG_INSTALLED_DIR` / `SNOW_FFMPEG_ROOT` 指到 Windows 树 |
| `snow_shot/CMakeLists.txt` | `OpenCV_DIR` 的强制赋值加 `if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)` 守卫 | 原来无条件指向 `vcpkg_installed/.../share/opencv4`，在 Linux 上会让 `find_package(OpenCV CONFIG REQUIRED)` 直接失败 |
| `cmake/vcpkg-overlay-triplets/x64-linux.cmake`、`arm64-linux.cmake` | 新增 | 为「Linux 上仍需 vcpkg 补几个发行版没有的库」这条可选路线准备；`arm64` 那份显式带 `-march=armv8-a+simd` |
| `scripts/uos-preflight.sh` | 新增（§6.1 的可执行形式） | 见下 |

`cmake/SnowDiagnostics.cmake` 首行是 `if(WIN32 OR APPLE)`，`snow_shot/CMakeLists.txt` 里 DirectML
的相关段落也都在 `if(WIN32)` 内，因此 Linux 天然不会去找 crashpad / DirectML —— 这两处**无需改动**，
写在这里是为了避免下次重新排查一遍。

### 9.3 需改动的 CMakeLists 关键点

1. `cmake/SnowQt.cmake`：`SNOW_QT_MAJOR` 分支；`find_package(Qt${major})`；Qt5 无 `EXACT` 语义时放宽为 `>=5.15`。
2. 根 `CMakeLists.txt:55-86`：现有 macOS 专用校验需补 Linux 分支；`_snow_mini_default` 对 Linux 置 OFF。
3. `snow_shot/CMakeLists.txt:1-8`：`OBJCXX` 仅在 `APPLE` 生效（确认 Linux 不触发）。
4. `snow_shot/CMakeLists.txt:404-451`：OCR 目标移除 `directml-provider`，Linux 用 `dynamic-onnx-runtime`。
5. `cmake/SnowDiagnostics.cmake`：Linux 下跳过 crashpad。
6. `cmake/SnowSimd.cmake`：新增 aarch64 分支。

### 9.4 Rust 构建

```bash
cd snow-crates
cargo build -p snow-capture -p snow-cursor --target x86_64-unknown-linux-gnu
cargo build -p snow-capture -p snow-cursor --target aarch64-unknown-linux-gnu
cargo test  -p snow-capture      # 按 AGENTS.md：只跑相关 crate 测试
```

> 注意 `.cargo/config.toml` 的 **`-Dwarnings`**：Linux 新平台代码的任何 warning 都会直接 build failure，
> 移植期建议临时对该目录放开或严格清零。

#### 9.4.1 在 Windows 上预先检查 Linux 目标（已实测，不必等 UOS 机器）

`rust-toolchain.toml` 已声明 `x86_64-unknown-linux-gnu` 与 `aarch64-unknown-linux-gnu`，
`cargo check` 只需要目标 std、不需要链接器，所以 Windows 工作站可以直接做跨目标检查：

```bash
cargo check -p snow-cursor -p snow-memory --target x86_64-unknown-linux-gnu
cargo check --workspace --target aarch64-unknown-linux-gnu \
    --config 'build.rustflags=["-Awarnings"]'      # 先看结构错误，别被 -Dwarnings 淹没
```

实测边界（2026-10-15，Windows + rustc 1.97.1）：

| 范围 | 结果 |
|---|---|
| 纯 Rust crate（`snow-core` / `snow-media` / `snow-memory` / `snow-cursor`） | ✅ 23.8s 全绿，可直接当 Linux/arm64 的语法 + cfg + 类型门禁 |
| 依赖树里含 **C 源码** 的 crate | ❌ 卡在 `cc-rs`：`failed to find tool "x86_64-linux-gnu-gcc"`（实测触发者是 `ring`，由 rustls 链条引入）。这类 crate 必须上 UOS 或装交叉 C 工具链才能验 |
| 整个 workspace 一把梭 | 不适用，会被上一条挡住 |

用法建议：新写 Linux 平台代码时先对**目标 crate** 跑跨目标 `cargo check`（秒级反馈），
再在 UOS 上跑真正的 `cargo build` / `cargo test`。这样能把"Linux 语法/cfg 错误"和
"Linux 环境缺失"两类问题分开，后者在真机上一眼可见，前者在真机上代价高得多。

由于"整个 workspace 一把梭"会被 `ring` 挡住，跨目标体检改成**逐个 crate** 跑：

```bash
bash scripts/linux-cross-check.sh                                  # x86_64
SNOW_CHECK_TARGETS="x86_64-unknown-linux-gnu aarch64-unknown-linux-gnu" \
    bash scripts/linux-cross-check.sh                              # 双架构
SNOW_CHECK_FILTER=capture bash scripts/linux-cross-check.sh        # 只跑名字含 capture 的
```

报告写入 `.workbuddy/qt5-port/logs/linux-cross-check.txt`，每个 crate 一份明细日志。
逐 crate 跑的价值第一次就兑现了：它把 `snow-capture` 一个**既有的** Linux 阻塞点
揪了出来（`MonitorId::raw_handle` 在非 Win/mac 下是 dead code，撞 `-Dwarnings`），
这个问题在 Windows 与 macOS 构建里完全不可见。

> **已知的跨目标不可验边界**：依赖树含 C 源码的 crate（`ring` ← rustls 链）需要
> `x86_64-linux-gnu-gcc`。要彻底解锁整个 workspace 的 Linux 类型检查，可以在
> Windows 上装 Zig 并设 `CC_x86_64_unknown_linux_gnu="zig cc -target x86_64-linux-gnu"`
> （`AR_*`/`CXX_*` 同理）。FFmpeg 绑定（`ffmpeg-next` 走 `pkg-config`）即使有 C 编译器
> 也仍需真机，除非手工给 `FFMPEG_*` 环境变量。

#### 9.4.2 逐 crate 体检现状（2026-10-15 实测，x86_64 + aarch64）

31 个 workspace 成员 × 2 架构 = 62 次检查，**首跑 46 通过（每架构 23/30）、当前 50 通过（每架构 25/31）**，9.4.1 的脚本已固化为
`bash scripts/linux-cross-check.sh`。缺口分两类，性质完全不同：

> **2026-10-17 复跑（slice 3 之后）**：仍是 **46/60**，失败项与分类**逐条未变**。差别在覆盖范围：
> `snow-recording-effects` 这一项现在真正编译了新增的 Linux 键鼠适配代码（`#[cfg(target_os="linux")]`
> 在该目标下是激活的），所以输入后端从"只在真机验"变成了"Windows 上每次都能类型检查"。
> 失败原因在 A 类内部会漂移（`zstd-sys` / `ffmpeg-sys-next` 谁先撞取决于并行调度），不是新问题。
>
> **2026-10-17 复跑（slice 4 之后）**：依旧 **46/60**，失败清单与上面第二次复跑**逐条相同**。
> 本片新增的 `snow-linux::text`（`ab_glyph` + `ttf-parser`）**没有给任何 crate 增加新的失败项**，
> 因为它只往依赖树里加了两个纯 Rust crate —— 这正是选它们而不选 fontconfig / FreeType 的原因：
> 若链了 C 字体栈，`snow-linux` 自己就会掉进 A 类，本片代码将不再有任何自动门禁。
>
> **2026-10-17 复跑（slice 5 之后）**：**48/60**（每架构 24/30），**B 类清零**。新增通过的两项就是
> `snow-ui-selector` 与 `snow-ui-selector-c` —— 它们此前是同一个根因的两面（provider 未导出 /
> C 层用不到）。A 类仍是每架构 6 个、逐条未变（`snow-ocr-process` 在两个架构上撞的是不同依赖，
> 见下表）。**这就是本片选 X11 窗口树而不是直接上 AT-SPI2 的实际收益：一次把 2 个门禁项转绿，
> 且没有新增任何 C 依赖。**
>
> **2026-10-17 复跑（slice 6 之后）**：仍是 **48/60**，失败清单逐条未变。新增的两项依赖
> （`x11rb` 的 `shm` 特性、`memmap2`）都不含 C 源码，所以门禁范围没有任何变化 ——
> 这是选 `CreateSegment` + `mmap` 而不是 SysV `shmget` 的直接后果。

> **2026-10-17 复跑（P4 slice 1 之后）**：**50/62**（每架构 25/31）。分母从 60 变成 62 是因为
> `snow-linux-c` 进了 workspace —— 它自己在两个架构上都 PASS，所以这是一次**纯增量**的覆盖，
> 不是通过率被稀释。A 类仍是每架构 6 个、逐条未变（`snow-ocr-process` 在两个架构上撞的都是
> `clipper-sys` 要 `*-linux-gnu-g++`）。**新增的门禁价值**：这段 C++ 直接调用的代码以前根本
> 不存在，现在它每次都在 Windows 上被类型检查。

> **2026-10-20 复跑（P4 slice 2 之后）**：仍是 **50/62**（每架构 25/31），失败清单逐条未变。
> 本片新增的是 `snow-linux::hotkey`（纯逻辑）与 `snow-linux::x11::hotkey`（`x11rb` 的
> `grab_key` / `get_modifier_mapping` / `query_keymap`，全在已启用的 `xproto` 里），
> 以及 `snow-linux-c` 的六个新导出 —— 没有新增 workspace 成员，也没有新增 C 依赖，
> 所以分母不动、通过项不动，属于**纯代码增量**。

> **2026-10-20 复跑（P4 slice 3 之后）**：仍是 **50/62**（每架构 25/31），失败清单逐条未变
> （仍是那 6 个 A 类 crate × 2 架构）。本片新增的是 `snow-linux::fullscreen`（纯逻辑）与
> `snow-linux::x11::fullscreen`（`get_property` / `query_tree` / `get_geometry` /
> `translate_coordinates` / `get_input_focus`，全在已启用的 `xproto` 里）——
> 没有新增 workspace 成员，也没有新增依赖，分母与通过项都不动。

> **2026-10-20 复跑（P4 slice 4 之后）**：仍是 **50/62**（每架构 25/31），失败清单逐条未变
> （仍是那 6 个 A 类 crate × 2 架构）。本片新增的是 `snow-linux::globalmouse`（纯逻辑）、
> `snow-linux::x11::keymap`（从 `x11/hotkey` 提出来的表，没有引入新请求）、
> `snow-linux::x11::globalmouse`（`grab_button` / `ungrab_button`，同样在已启用的 `xproto` 里）
> 与 `snow-linux-c` 的五个新导出 —— 没有新增 workspace 成员，也没有新增依赖。

**(A) 需要交叉 C 编译器 —— 不是我们的代码问题（每架构 6 个）**

| crate | 卡住的 C 依赖 | 说明 |
|---|---|---|
| `rapid-ocr-rs` | `ring` | rustls 链引入 |
| `snow-ocr-process` | `clipper-sys` | 要 `*-linux-gnu-g++`（两个架构都一样） |
| `snow-recording-c` / `snow-recording-export` / `snow-recording-runtime` / `snow-screen-recorder` | `ffmpeg-sys-next` | FFmpeg 绑定；真机上还要 `pkg-config` 找 `libav*-dev` |

这一类的失败**与 Linux 移植无关**：同一份代码在 UOS 真机上只要有 `gcc` + `libav*-dev` 就能过。

**(B) 真实缺口 —— ~~必须在 P3 里补（每架构 1 个）~~ 已于 slice 5 清零（每架构 0 个）**

| crate | 报错 | 根因 | 处置 |
|---|---|---|---|
| ~~`snow-ui-selector-c`~~ | ~~`E0432 unresolved import snow_ui_selector::ElementRegionService`~~ | 该类型只在 `#[cfg(windows)]` / `#[cfg(target_os="macos")]` 下导出；`snow-ui-selector` 尚无 Linux provider | **已修（slice 5，见 9.4.6）**：新增 `snow-ui-selector/src/linux.rs` 导出 `ElementRegionService`，走 X11 窗口树。原判断「不放假桩」保留 —— 当时塞一个"返回空元素集"的实现会让 C 层以为无障碍"可用但没结果"，比编译失败更危险；本片给的是**真实现**（窗口级），不是空元素集 |

**(C) 顺手修掉的两个既有 Linux 阻塞（本来编译不过，与本次改动无关）**

| 位置 | 问题 | 修法 |
|---|---|---|
| `snow-capture/src/monitor.rs` | `MonitorId::raw_handle()` 只被 Win/mac 后端读 ⇒ Linux 下 dead code ⇒ 撞 `-Dwarnings` | `#[cfg_attr(not(any(windows, macos)), allow(dead_code))]` |
| `snow-ui-selector/src/{lib.rs,query.rs}` | `ElementRect::new` 与 `QueryControl` 的字段同样只在 Win/mac 被消费 | 同上，平台范围内 `allow(dead_code)` |

这类问题在 Windows 与 macOS 构建里**完全不可见**，正是 9.4.1 那套跨目标检查第一次跑就抓出来的。

> 检查报告：`.workbuddy/qt5-port/logs/linux-cross-check.txt`；逐 crate 明细
> `.workbuddy/qt5-port/logs/linux-check-<target>-<crate>.log`。
> 复跑：`bash scripts/linux-cross-check.sh`（加 `SNOW_CHECK_TARGETS` 可扩到 arm64）。

#### 9.4.3 P3 slice 2：X11 采集后端落地（2026-10-15）

`platform/mod.rs` 里的 `UnsupportedBackend` 在 Linux 上已有真实现：`platform/linux.rs`
（`LinuxBackend` + `X11Capturer`）经 `snow-linux` 的 X11 会话读根窗口 `GetImage`。

**分层照 8.1 的铁律切**：协议与像素全在 `snow-linux`，`snow-capture` 只做类型翻译。

| 层 | 位置 | 平台 | 内容 |
|---|---|---|---|
| 协议 | `snow-linux/src/x11.rs` | linux | `capture_image(Rect)`：裁剪到根窗口 → `GetImage(ZPixmap, AllPlanes)` → 根 visual + 该深度的 pixmap format |
| 解码 | `snow-linux/src/capture.rs` | **全平台** | `ImageLayout` / `ChannelMask` / `decode_into` / `decode_window_into`：掩码驱动的通用解码 + 原生 BGRX 快路径 + 按目标 stride 写子窗口（9.4.23）；单测**在 Windows 上跑** |
| 接线 | `snow-capture/src/platform/linux.rs` | linux | `CaptureBackend`（枚举 / 主屏 / 布局 / inspect）+ `MonitorCapturer`（`capture`、`capture_region_into`、`set_output_pixel_format`） |

三个必须写下来的判断（都是跨目标类型检查逼出来的）：

1. **`bits_per_pixel` 不在 visual 里**。X11 的 `Visualtype` 只有 `red_mask/green_mask/blue_mask`；
   每个深度的位宽与扫描线填充在 `Setup.pixmap_formats`。所以位宽按**回复的 `depth`** 去查
   `pixmap_formats`，而不是从 visual 猜 32 —— 猜错就是整幅画面通道错位。
   同理 `GetImage` 回复**没有** `bytes_per_line` 字段：优先用回复长度反推（`reply_stride`），
   退路才是按 `Format.scanline_pad` 现算（`scanline_bytes`）。
2. **X11 像素没有 alpha**。32bpp 的第 4 字节是填充（常见 0，也可能残留脏数据），
   解码一律写成不透明的 `0xff` —— 直接透传会得到带洞的截图。
3. **根窗口读不到指针，也读不到别的窗口**。指针由 X 合成在根窗口"之上"而非"之中"，
   所以 `create_target_capturer` 保持默认（不提供原生采集器），光标交给会话的通用路径
   （`attach_cursor_to_frame` → `snow_cursor` 的 X11 采样器）来画；窗口目标直接
   `BackendUnavailable` —— 读别的窗口像素需要 Composite 重定向，这是硬缺口而非待办。

`CaptureBackendKind` 新增 `X11` 变体（`as_str() == "x11"`），C ABI 里是 `5`
（`snow_capture.h` 同步）。穷尽 match 的更新点：Windows 侧两个 `create_*_by_kind`
（**显式拒绝而非替换**）、三个 benchmark example、`snow-capture-c` / `snow-recording-c`
的 `capture_backend_value`。

**性能**：同步往返 + 服务端拷贝的形态已在 slice 6 改掉——像素经 MIT-SHM 直接写进双方
共享的内存，回复里只剩下深度与字节数（见 9.4.7）。**仍缺**的是脏矩形：`is_duplicate` 恒为 false，
Windows 侧靠 DXGI 脏矩形跳过编码的优化在这里没有对应物；X11 的等价物是 Damage 扩展，属后续 slice。

#### 9.4.4 P3 slice 3：X11 输入观测后端落地（2026-10-17）

P3 slice 1/2 让 Linux 能「看上屏」，这一片让它能「看见手」：`snow-recording-effects`
的键盘/鼠标观测在 Linux 上不再是空实现。分层仍照 8.1 的铁律切，且**新增的纯逻辑面比 X11 面还大**
（这也是它能在 Windows 上被完整单测的原因）。

| 层 | 位置 | 平台 | 内容 |
|---|---|---|---|
| 翻译 | `snow-linux/src/input/keys.rs` | **全平台** | `KeyMap`（键码→keysym，含 shift/AltGr 层选择与 `NoSymbol` 回退）、`virtual_key`（keysym→虚拟键）、`label`、`pressed_from_keymap`。**20 个单测在 Windows 上跑** |
| 规则 | `snow-linux/src/input/tracking.rs` | **全平台** | `Region` / `MotionTracker`（连续性）/ `ClickTracker`（按下命中区域、抬起跟随）/ `PointerButton` / `modifiers_from_pressed`。**16 个单测** |
| 对话 | `snow-linux/src/input/x11.rs` | linux | `X11InputObserver`：XI2 原始事件订阅 + 事件泵 + `QueryPointer`/`QueryKeymap`/`GetKeyboardMapping` 往返 |
| 接线 | `snow-recording-effects/src/{keyboard_hook,mouse_hook}.rs` | linux | `#[cfg(target_os = "linux")] mod platform`，翻译成既有的 `KeyObservation` / `MouseClickObservation` / `MouseMovement` |

**先修掉一个既有 Linux 硬阻塞**：mouse/hook 的空实现里 `start()` 返回 `Ok`（一个永远不上报的观察器），
只有 `start_with_movement()` 返回 `Err`。而 `deferred.rs`（Linux 目标下确实参与编译）走的正是后者 ⇒
**Linux 上 `InputRecorder::new` 必然失败，录制根本起不来**。空实现里"假装成功"的那一半正是问题所在，
两处都已换成真实现，`not(any(windows, macos))` 门控随之改为 `not(any(windows, macos, linux))`。

四个必须写下来的判断（都是跨目标类型检查/交叉比对逼出来的）：

1. **观测值必须说 Windows 虚拟键的空间**。overlay 的 `KeyObservation::event()` 与
   `keyboard_overlay::modifier()` 用**硬编码**的 `0xa2/0xa3/0xa4/0xa5/0xa0/0xa1/0x5b/0x5c` 索引
   `pressed` 表，F1–F24 靠 `0x70..=0x87`、小键盘靠 `0x60..=0x69` 直接格式化。所以 Linux 后端不能透传
   X11 键码，必须做 keysym→虚拟键映射（`keys.rs`）。macOS 走的是 `0x100+原生键码` 的第三空间，Linux 只在
   **映射不到**时才退回 `0x100+键码`（见第 4 点）。
2. **keysym 描述的是"字符"，不是"键"**。US 布局下同一个物理键给出 `0x31`(`1`) 与 `0x21`(`!`)，
   而 Windows 虚拟键只有一个身份。故映射表建在**字符**上：`!` 与 `1` 归一、`?` 与 `/` 归一、`;`/`:` 归一，
   与 Windows 的 `VK_OEM_*` 语义一致。钥匙帽显示什么则由 `label(keysym)` 决定 —— 它返回**按下的那个字符**
   （`!`、`Д`），因此比"虚拟键→字符"反查更准（后者拿不到 Shift 层）。
3. **不用原始事件的坐标，用 `QueryPointer`**。RawMotion 的位置在设备 valuator 里，`axisvalues` 与
   `axisvalues_raw` 哪个含加速值、相对设备按 FP1616 还是 FP3232 上报，**没有真机就无法证伪**，
   而猜错的结果是"看起来合理但整体偏移"的光标轨迹 —— 最坏的一类错误。`QueryPointer` 在任何服务器/设备上
   都无歧义。代价是每次采样一次往返，故运动采样上限 `MOTION_SAMPLE_INTERVAL = 8ms`（125 Hz）并在其间合并
   ——消费端本来就只保留最新一条 movement，合并零损失。
4. **观测器自己持有 pressed 表和键盘映射**，于是按键事件**零往返**：shift/AltGr 层选择直接用本地
   `pressed` 表算，不需要每次查询修饰键。映射在 `MappingNotify` 时重建（换布局后标签才不会继续描述旧布局）；
   通道溢出后 `generation` 自增，并用 `QueryKeymap` 重建 pressed 表 —— 丢掉的批次里可能含 keyup，
   不修就会让一个修饰键**永久画在屏幕上**。

**其余刻意的取舍**：

- **poll 而不是阻塞 `wait_for_event`**：连接的 socket 归工作线程独占，`Drop` 没有任何办法打断阻塞读，
  而录制收尾必须能随时结束观测器。改成 `poll_for_event` + 空闲 10ms 休眠（约 100 次/秒唤醒，比采集路径
  自身的帧定时器低一个量级），代价是**空闲时首事件最多在 socket 缓冲里等 10ms** —— 抖动上限，远小于一帧。
- **`alt_gr` 恒为 false**：X11 的 AltGr 是**独立的键**（映射到 `0xa5`，标签直接就是 `AltGr`），
  不是 Windows 那种"Ctrl+Alt 合成"的伪修饰。该标志的作用是隐藏伪装的 Ctrl 芯片，这里不存在。
- **无 XInput2 时诚实失败**：`LinuxError::UnsupportedInputBackend`（分类 `InvalidConfig`，不重试），
  而不是塞一个"返回空事件流"的实现 —— 后者会让录制器承诺一个永远不出现的键帽。
- **`scan` 字段在 Linux 上存 keysym**：X11 没有硬件扫描码可报，而 keysym 正是 `printable_label`
  需要的（`scan` 的语义本来就是每平台不同：Windows 是扫描码，macOS 是 `keyboard_type`）。
  `layout` 字段在 Linux 上未使用（映射由观测器单点持有）。

**验证（2026-10-17，Windows + rustc 1.97.1）**：

| 检查 | 结果 |
|---|---|
| `cargo test -p snow-linux` | **65 通过**（其中 47 个是本次新增：keys 20 + tracking 16 + error 1 + …） |
| `cargo test -p snow-recording-effects` | **74 通过**，1 ignored —— 与改动前逐条一致，无回归 |
| `cargo check -p snow-linux --target {x86_64,aarch64}-unknown-linux-gnu` | 双架构全绿（`-Dwarnings` 生效） |
| `cargo check -p snow-recording-effects --target {x86_64,aarch64}-unknown-linux-gnu` | 双架构全绿 —— 该 crate 依赖树**无 C 源码**，所以整条适配路径都能在 Windows 上类型检查 |

**本片未覆盖（诚实记账）**：Wayland（无特权客户端拿不到全局输入，只能走 `GlobalShortcuts` / 远程桌面
portal）、无 XInput2 的 XRecord/evdev 退路、音频采集。**真机待验**：XInput2 原始事件在 UOS 20 的
Xorg 与 XWayland 下的实际投递行为、以及 `QueryPointer` 采样在 125 Hz 上限下的实测开销。

#### 9.4.5 P3 slice 4：键帽光栅化落地（2026-10-17）

slice 3 让 Linux「采得到键」，这一片让它「画得出键帽」：`snow-recording-effects` 的
`keyboard_rasterizer::create` 在 Linux 上不再返回 `Err`，录制时的按键叠加终于能合成进画面。

**先纠正上一片文档的一处错误断言。** §4.2 原先写着「`keyboard_rasterizer.rs` 无需改动（纯逻辑）」，
这是错的：该文件是 `#[cfg(...)] mod platform` 结构，Windows 走 DirectWrite + Direct2D、macOS 走
CoreText + Quartz、其余平台是**硬返回 Err** 的桩。**判据**：凡是每个平台都各自实现一份 `mod platform`
的模块，就不是纯逻辑，第三方平台的适配必须在清单里，不能因为「文件名不含平台字样」就跳过。
本片的起点正是这个被漏掉的桩 —— 功能上它让 Linux 的按键叠加整条链路缺最后一环。

| 层 | 位置 | 平台 | 内容 |
|---|---|---|---|
| 字体发现 | `snow-linux/src/text/font.rs` | **全平台** | `scan_faces`（递归枚举 + `ttf-parser` 读 `name` 表）、`select_face`（族名归一 + 字重最近邻）、`resolve`（主族 + CJK 回退）。**10 个单测** |
| 光栅化 | `snow-linux/src/text/raster.rs` | **全平台** | `KeycapRenderer`：`ab_glyph` 取轮廓 → 灰度覆盖；自写圆角矩形 4×4 超采样；premultiplied RGBA 合成。**15 个单测** |
| 接线 | `snow-recording-effects/src/keyboard_rasterizer.rs` | linux | `#[cfg(target_os = "linux")] mod platform`，包成既有的 `KeycapRasterizer` |

五个必须写下来的判断：

1. **不调 fontconfig**。这是本片最重要的约束，与 x11rb 的选型同源：`cargo check --target
   <linux-triple>` 是本项目**唯一**能在 Windows 上验证 Linux 代码的门禁，一旦链接 C 库（fontconfig /
   FreeType），该门禁立刻需要 C 交叉工具链与 target 头文件，机制整个失效。所以族名解析改成
   **枚举字体文件 + 直接读 `name` 表**（`ttf-parser`，纯 Rust），光栅化用 `ab_glyph`。代价是
   fontconfig 的 alias / 替换规则（`~/.config/fontconfig/fonts.conf`）**不生效**，只做族名精确匹配。
2. **只实现 `rasterize`，`rasterize_glyph` 沿用 trait 默认实现** —— 与 macOS 分支一致（macOS 也只实现
   前者）。默认实现把 `font_pixels / 32` 当作 scale，`playback.rs` 的 `TIMER_FONT_SIZE = 24` ⇒ scale
   0.75。**这个不能自己另算**：`playback` 靠扫描返回位图的 alpha 通道来定位字形边界并推出基线
   （`glyph_top` / `digit_top`），换一套度量就会让计时器数字的基线飘。
3. **产物契约对齐 macOS 而不是 Windows**。两边都能画成「带边框的圆角键帽」，但构造不同：macOS 是
   **border 铺满整图 + background 内缩 `scale` 像素**（圆角 `10 × scale`），Windows 是矩形内缩 1.0 +
   圆角 12 + 2px 描边。选 macOS 的形态，因为自绘一遍 fill 两遍更简单，且 Linux 与 macOS 同属
   `not(windows)` 分支。尺寸公式与两边一致：`ceil(fit(measured).0 × scale)` by `round(64 × scale)`。
4. **文本度量用 advance 累加，不做 kerning**。主字体与 CJK 回退字体是两套坐标，跨字体的一对
   kern 没有意义；同字体下对一两个字符的键帽图例影响不到 1px。macOS 的 `CTLineGetTypographicBounds`
   本质也是 advance 宽度，所以这仍与 macOS 同源。
5. **缺字回退的判据是 `glyph_id(c) == 0`**。`ab_glyph` 对无轮廓的字符返回 `.notdef`（id 0），
   这正是 DirectWrite / CoreText 触发 cascade 的同一个条件。**主字体缺失且没有回退字体时不报错**，
   退化成 `.notdef` 方框 —— 少一个汉字不该让整帧合成失败。但**请求了一个未安装的族名要报错**
   （`TextError::FontUnavailable`）而不是换成别的字体：录出来的水印字体必须与应用程序其它地方一致。

**其余刻意的取舍**：

- **圆角与边缘用 4×4 超采样覆盖率**（16 样本/像素）而不是解析式 SDF：确定性、能写进断言、
  没有浮点近似的争议。一个 64×144 的键帽约 15 万次 `inside` 测试，微秒量级，而 overlay 侧本来就有
  256 项、32 MB 上限的键帽缓存。
- **不做 hinting / 子像素定位**：灰度抗锯齿与系统库同一档次，键帽是小字号大字重，差异不可见。
- **默认族名候选表**（`Noto Sans CJK SC` → `Source Han Sans SC` → `WenQuanYi Micro Hei` → `Noto Sans`
  → `DejaVu Sans` → `Liberation Sans`）只在请求未命名族名时使用；UOS / Deepin 默认 UI 字体是前两个。
- **扫描跳过符号链接并限深 6 层**，否则字体目录里一个指回父目录的链接会把一次枚举变成无边界遍历。
  文件按路径排序后解析，保证同一台机器上字重平局的判定每次一样。

**验证（2026-10-17，Windows + rustc 1.97.1）**：

| 检查 | 结果 |
|---|---|
| `cargo test -p snow-linux` | **91 通过**（slice 3 后是 65，本片 +26：`text::font` 10 + `text::raster` 16） |
| `cargo test -p snow-recording-effects` | **74 通过**，1 ignored —— 与改动前逐条一致，无回归 |
| `cargo check -p snow-recording-effects --all-targets` × {windows, x86_64-linux, aarch64-linux} | 三目标全绿（`-Dwarnings` 生效） |
| `cargo check -p snow-linux --target {x86_64,aarch64}-unknown-linux-gnu` | 双架构全绿 |
| `bash scripts/linux-cross-check.sh`（双架构，slice 4 后复跑） | **46/60**，失败清单与 slice 3 复跑逐条相同；`snow-linux` 与 `snow-recording-effects` 均 PASS |

单测**不是**在真机上跑的：验证素材是 `test-support/fonts` 里那三套刻意造的测试字体。它们的设计正好
支撑本片的断言 —— Latin 家族**故意不含汉字轮廓**（"fallback 测试不能靠基础字体蒙混过关"）、
Bold 的笔画是 Regular 的两倍多（能区分「真加载了粗体」与「又用了一次常规体」）、每个非 ASCII 字形的
advance 都比 Latin 的 `M` 宽（能区分「量了回退字体」与「没量」）。

测试字体是 fontTools 生成的**单 face TTF**，与真实安装仍有距离（集合 `.ttc`、CFF 轮廓、本地化 name
记录），所以另有一个 `#[cfg(windows)]` 的冒烟测试直接枚举宿主 `C:/Windows/Fonts`（约 3.8 秒），
用 **Arial 画拉丁图例、微软雅黑做汉字回退**，两者都要求真有墨迹落地；目录不存在时不执行。
这条测试是「发现层不是只对生成字体成立」的实证。

**本片未覆盖（诚实记账）**：fontconfig 的 alias / 替换规则与按语言选字形（`lang=` 匹配）；
颜色字体（COLR / CBDT emoji）；可变字体的 `wght` 轴插值（只按静态 face 选字重）；
`Xft` / X core 位图字体。**真机待验**：UOS 20 上实际的默认族名（候选表按 Noto / 文泉驿写的，
若镜像用的是别的族名，`create` 会在录制启动时报 `FontUnavailable` 而不是静默画错），
以及汉字键帽在真实字体下的墨迹居中效果。

#### 9.4.6 P3 slice 5：X11 窗口级拾取落地（2026-10-17）

slice 1–4 让 Linux「看上屏、看见手、画得出键帽」，这一片补的是**指针指向谁**：
`snow-ui-selector` 在 Linux 上第一次有了 `ElementRegionService`，`snow-ui-selector-c` 随之恢复可编译。
这就是 9.4.2 里唯一一个 B 类门禁失败的根因，本片把它清零（46/60 → **48/60**）。

**范围是窗口级，不是控件级。** 控件级要 AT-SPI2（另一个协议、另一条总线），本片没做，
也不假装做了 —— 见下面第 5 条判断。

| 层 | 位置 | 平台 | 内容 |
|---|---|---|---|
| 规则 | `snow-linux/src/window.rs` | **全平台** | `WindowKind` / `classify`（`_NET_WM_WINDOW_TYPE` 折叠）/ `selectable`（可选中判据）/ `DesktopWindows::hit_test`（叠放序命中）。**10 个单测在 Windows 上跑** |
| 对话 | `snow-linux/src/x11/window.rs` | linux | `desktop_windows_for`：`_NET_CLIENT_LIST` → 逐窗口 `GetWindowAttributes` + `_NET_WM_WINDOW_TYPE` + `GetGeometry` + `TranslateCoordinates` → 根坐标 frame 矩形 |
| 接线 | `snow-ui-selector/src/linux.rs` | linux | `ElementRegionService`：与 macOS provider 同形的 API（`new` / `with_backend*` / `refresh*` / `release_cache` / `window_snapshot` / `from_snapshot` / `query`），`!Send`/`!Sync` |

五个必须写下来的判断：

1. **`_NET_CLIENT_LIST` 是自下而上，命中测试必须倒着找**。这是本片最容易写错、且**错了不会崩**的地方：
   正序遍历命中第一个，拿到的是最底层窗口 —— 表现为"选择器总指向后面那个应用"，看起来像
   业务逻辑 bug 而不是遍历方向 bug。所以规则模块的文档注释和单测都把这条写在最前面
   （`the_topmost_window_wins` 用两个**完全重合**的矩形来断言：叠放序相同则无法靠几何区分）。
2. **`TranslateCoordinates` 给的是内区原点，边框要自己补回来**。X11 的 `GetGeometry` 里
   `width`/`height` 是内区尺寸，`border_width` 单独给出；而用户看到的、他要截的那块是**含边框**的外框。
   两者不一致时框会整体向内缩一圈，且窗口越有边框偏得越多。故 `bounds = (dst − border, w + 2·border)`。
   `translate_coordinates` 的 `same_screen == false` 一律丢弃候选：那表示窗口在另一个 screen 上，
   在本 screen 的根坐标里没有位置。
3. **属性的三种状态必须分开**：「读失败（窗口没了）」「属性不存在（`type_ == NONE`）」「存在但为空」。
   - `GetWindowAttributes` / `GetGeometry` / `TranslateCoordinates` 失败 ⇒ **丢弃这一个候选**，不让整次
     枚举失败。桌面在任何时刻都有应用被关掉，一次瞬时竞态不该让选择器整体报错。
   - `_NET_CLIENT_LIST` **属性不存在**才回退到 `QueryTree` 遍历根窗口子窗口（reparenting WM 下的
     frame 窗口）；**存在但为空**是窗口管理器在说"没有受管窗口"，此时回退会把 frame 当成窗口，是反的。
   - `_NET_WM_WINDOW_TYPE` 不存在 ⇒ `Normal`。该属性是**可选的**，绝大多数应用根本不设；
     若把"没写类型"当成"不可选中"，等于把几乎所有程序排除在外。
4. **`_NET_WM_WINDOW_TYPE` 与 `override_redirect` 两道过滤都要**，因为它们描述的是不同的东西。
   菜单/工具提示通常 `override_redirect`，于是**根本不会出现在 `_NET_CLIENT_LIST` 里**（受管窗口才进）；
   但回退路径读的是根窗口子窗口，那时它们就在。反过来，dock/desktop 是受管窗口、会进列表。
   单测 `only_plain_application_windows_are_selectable` 把七种"看似窗口但不可选"的形态列了一遍
   （不可见 / override-redirect / `InputOnly` / 零面积 / 桌面 / 面板 / 瞬态），逐条断言被拒。
5. **`HitTestMode::UiElement` 与 `Window` 返回同一个窗口矩形 —— 这是有意的，且写进了模块文档**。
   没有控件级 provider 时，把窗口矩形当控件矩形返回会让调用方以为拿到了控件；
   而返回"空元素集"会让 C 层以为无障碍**可用但没结果**（比编译失败更危险，与 9.4.2 原来的判断一致）。
   所以只承诺窗口级，并在 `query` 的文档里写明"需要控件时应把本 provider 当窗口级退路"。

**其余刻意的取舍**：

- **点落在窗口之间的空隙时报根窗口矩形**，不返回空 path。空 path 在 C ABI 里是 `path: None`，
  上层当作"这次没选中"；而 macOS provider 在同样情形返回显示器矩形。两者行为保持一致，
  用户在裸桌面上点一下也能框到一块区域。
- **`Point::window_id` 优先**：UI 线程已经用 `QueryPointer` 解析过命中窗口时，以它的答案收窄搜索范围。
  两次遍历的时差可能让重叠窗口的结论不一致，让先解析的一方说了算，比让两边各自猜更稳。
- **属性读取上限 `MAX_PROPERTY_ITEMS = 1024`**：一次 hover 里读几千项的窗口列表不是任何真实桌面，
  上限只是防止一个畸形属性把一次 hover 变成大分配。
- **`release_cache` 只清快照**：X11 provider 不持有长生命周期的服务器对象，连接随快照一起丢掉；
  这与 macOS provider 需要显式释放 AX 对象不同，因此"释放"在这里就是丢弃 `WindowSnapshot`。

**验证（2026-10-17，Windows + rustc 1.97.1）**：

| 检查 | 结果 |
|---|---|
| `cargo test -p snow-linux` | **101 通过**（slice 4 后是 91，本片 +10 = `window` 全部） |
| `cargo test -p snow-ui-selector` | **50 通过**，无回归 |
| `cargo check -p snow-ui-selector -p snow-ui-selector-c --all-targets` × {windows, x86_64-linux, aarch64-linux} | 三目标全绿（`-Dwarnings` 生效） |
| **反向验证**：往 `linux.rs` 末尾注入 `compile_error!` 后重跑 linux 目标检查 | 构建**如期失败** —— 证明该文件在 linux 目标下确实参与编译，而不是被 cfg 掉后"检查通过" |
| `bash scripts/linux-cross-check.sh`（双架构） | **48/60**（每架构 24/30）；`snow-ui-selector` 与 `snow-ui-selector-c` 均 **PASS**，B 类清零 |

反向验证这一步不是形式主义：`snow-ui-selector` 的 Linux 面全部在 `#[cfg(target_os = "linux")]` 之后，
如果 cfg 名写错（例如写成 `target_os = "linux "`），跨目标检查会**静默通过**，而真机上照样编译不过。
注入一个必然失败的语句，是把"这条路径真的被编译了"从假设变成事实的最便宜办法。

**本片未覆盖（诚实记账）**：

- **控件级拾取（AT-SPI2）**：`HitTestMode::UiElement` 拿不到按钮/输入框。已完成可行性探针：
  `zbus 6.0.0`（`--no-default-features --features default-rt,proxy`）能在 Windows 上为
  `x86_64-unknown-linux-gnu` 交叉编译通过、依赖树无 C 源码 ⇒ **不会破坏纯 Rust 铁律**。
  但代价是 1000+ 行协议代码 + 「在 Windows 上无法验证行为」—— 属于独立切片。
- **非矩形窗口的形状**：`Shape` 扩展裁剪过的窗口，这里按外接矩形处理，会多框一圈透明区域。
- **多 screen 的老式 X 配置**（`Xinerama` 之前的 `screen 0/1`）：跨 screen 的窗口被直接丢弃，不折算坐标。
- **窗口的最小化状态**：靠 `IsViewable` 判断（最小化窗口通常已 unmap）；不额外读
  `_NET_WM_STATE_HIDDEN`。
- **XWayland 下的缩放**：XWayland 会把逻辑坐标与物理像素的关系交给合成器，本层拿到的仍是
  X 空间坐标；Wayland 会话下的实际映射需真机验证。
- **真机待验**：UOS 20 的 `dde-daemon` / `dde-dock` 是否导出 `_NET_CLIENT_LIST`（不导出则走
  `QueryTree` 回退，选中粒度变成 frame 窗口）、以及 `_NET_WM_WINDOW_TYPE` 在国产应用上的实际设置率。

#### 9.4.7 P3 slice 6：MIT-SHM 零拷贝采集（2026-10-17）

slice 2 的 X11 采集能出画面，但每帧的像素要穿过整条连接：服务端分配回复、拷进去，客户端再拷出来。
1920×1080 一帧 ≈ 8 MB，三份拷贝，按录制帧率重复。本片把这条路换成 MIT-SHM：
服务端直接写进双方共享的内存，回复里只剩深度与字节数。

| 层 | 位置 | 平台 | 内容 |
|---|---|---|---|
| 尺寸规则 | `snow-linux/src/capture.rs::shm_segment_size` | **全平台** | 按服务端的 `scanline_pad` 规则算段大小，溢出/退化返回 `None`。**4 个单测在 Windows 上跑** |
| 对话 | `snow-linux/src/x11/shm.rs` | linux | `Segment`（`CreateSegment` 取 fd + `mmap`）、`prepare_shm`（分配/按需扩大）、`read_shm_image`（`ShmGetImage` + 用回复的 `size` 反推 stride） |
| 接线 | `snow-linux/src/x11.rs::capture_image` | linux | 先 `prepare_shm`（`&mut`）再 `read_image`（`&self`）；后者优先读共享段，不可用则 `GetImage` |

**为什么这仍然是"纯 Rust"且不缩门禁**：`CreateSegment`（MIT-SHM 1.2）由服务端分配段并把 fd 传回来，
客户端只用 `memmap2` 映射（它本身也不含 C 源码）。换成经典的 SysV `shmget` + `Attach` 也能做，
但那要直接调 libc，且段的生命周期要自己用 `shmctl(IPC_RMID)` 管；`CreateSegment` 的段随连接消失。

五个判断：

1. **`ShmGetImage` 没有回复数据，所以必须先分配再知道大小**。这是与 `GetImage` 最根本的差别：
   后者可以「读回复 → 从长度反推 stride」，前者必须在发请求前就算出字节数，
   算小了服务端就写越界。故用 `shm_segment_size`（与服务端同一套 padding 规则）估算，
   再用回复里的 `size` 反推真实 stride（复用 slice 2 的 `reply_stride`）——**不信任自己的估算**。
2. **失败是连接级属性，不是瞬时故障**。远程 TCP 显示、无 MIT-SHM 的服务器、拒绝映射 fd 的沙箱，
   这三种情况都不会因为下一帧而改变。所以第一次失败就把会话标成 `Unavailable` 并按 `GetImage` 走到底；
   若每帧重试，等于每帧多一次往返去确认同一件事。
3. **越界一律不读**。服务端已经在回复到达前写完了，所以 `size > 段长` 时数据可能已经越界；
   该帧直接交给 `GetImage`（它带回长度可校验）。另外 `mmap` 按页映射，小幅越界通常落在同一页里
   不会崩 —— 但这是"没崩"，不是"正确"，不能拿来当保证。
4. **准备与读取必须拆成两个函数**。分配需要 `&mut self`，而读取要把映射里的切片借出去；
   一个方法同时做两件事，就会出现「一条路径返回自身借用、另一条路径继续用自身」——
   可变借用的生命周期被返回类型钉死到整个函数体，编译器直接拒绝。这个拆分是编译器逼出来的，
   不是风格选择。
5. **`capture_access_active` 仍为 `false`**：共享段不是"采集访问句柄"，释放它并不会让别的程序
   重新获得采集能力。语义没变，就没有理由跟着改。

**验证（2026-10-17，Windows + rustc 1.97.1）**：

| 检查 | 结果 |
|---|---|
| `cargo test -p snow-linux` | **105 通过**（slice 5 后是 101，本片 +4 = `shm_segment_size`） |
| `cargo check -p snow-capture --target {x86_64,aarch64}-unknown-linux-gnu` | 双架构全绿（`-Dwarnings` 生效） |
| `bash scripts/check-rust.sh snow-linux`（fmt + clippy `-D warnings`） | 绿。**顺带修掉前几片遗留的 3 个 clippy 与全 crate 的 fmt 漂移**（单独一个 `style` 提交） |
| `bash scripts/linux-cross-check.sh`（双架构，slice 6 后复跑） | **48/60**，失败清单逐条未变；`snow-linux`、`snow-capture` 均 PASS |

**必须说清楚的一点**：这一片**在 Windows 上无法验证行为，只能说类型正确**。零拷贝是否真的更快、
`ShmGetImage` 在 UOS 的 Xorg 上是否按 `size` 报告长度、以及 `CreateSegment` 在 dde 会话下是否被允许，
都必须在真机上测。手册层面的依据是 MIT-SHM 规范与 Xorg 的实现，不是本机的观测。

**本片未覆盖（诚实记账）**：

- **Damage 脏矩形**：`is_duplicate` 仍恒为 false，录制器无法跳过未变化的帧。Damage 是纯协议
  （`x11rb` 的 `damage` 特性，零 C），但"根窗口的 Damage 在无 Composite 重定向时是否覆盖
  遮挡变化"必须真机确认，属独立 slice。**—— 已由 9.4.19 落地（含合成器前置判据与心跳兜底）。**
- **窗口采集**：仍 `BackendUnavailable`。读别的窗口像素需要 Composite 重定向，而**有合成器运行时
  重定向会被拒（BadAccess）**，届时要走合成器的截图 portal 或从根图像裁剪。这是设计决策，不是补丁。
  **—— Wayland 侧已由 9.4.20 打通到"拿到流描述"为止，帧消费与窗口源选择仍缺。**
- **合成器的 `XCompositeNameWindowPixmap` 路径**、`XShmPutImage`（本工程不往屏幕写像素，用不到）。
- **真机待验**：UOS 20 / Deepin 23 上 `CreateSegment` 的可用率、1440p 下每帧耗时与 CPU 占用
  相对 `GetImage` 的实际差距。

#### 9.4.8 P4 slice 1：Rust↔C++ 平台桥 + 物理光标（2026-10-17）

P4 的第一个问题不是"写什么"，而是**写在哪里才能被验证**。C++ 侧在这个仓库里**一行都没被编译过**
（本机没有 Linux C++ 交叉工具链，也没有 Qt5 的 Linux 头），所以如果直接把 Win32 调用逐条翻译成
Xlib 调用，产出的是一批**只能靠眼睛读的代码**。本片的取舍是：把逻辑压到纯 Rust 里（可跨目标类型检查
+ 可在 Windows 上跑单测），C++ 只剩转发，并**先用一个最小能力把桥建立起来**，后续 P4 各项复用同一座桥。

| 层 | 位置 | 平台 | 内容 |
|---|---|---|---|
| 坐标规则 | `snow-linux/src/pointer.rs` | **全平台** | `RootPosition::new`（i32 → i16 越界即 `None`）、`PointerSample::position`（`same_screen == false` ⇒ 无位置）。**6 个单测在 Windows 上跑** |
| 对话 | `snow-linux/src/x11/pointer.rs` | linux | `pointer_sample`（`QueryPointer`）、`warp_pointer_to`（`WarpPointer` + 显式 `flush`） |
| C ABI | `snow-crates/crates/snow-linux-c/`（`include/snow_linux.h`） | linux | 进程级连接 + 空句柄；`create`/`destroy`/`pointer_position`/`set_pointer_position`。**整 crate 在 `#![cfg(target_os = "linux")]` 之后**，非 Linux 上导出为空 |
| 接线 | `snow_shot/src/platform/physicalcursor.cpp` | linux | `#elif defined(Q_OS_LINUX)` 分支，形状与 macOS 分支同构；CMake 用遍历把所有编译该文件的 target 都链上 `snow_shot_linux_platform` |

**为什么非要另开一个 archive**（实测依据，不是偏好）：把这段代码塞进 `snow_rust_ffi` 是更"自然"的做法，
但本机实测那个 archive 的 Linux 目标 `cargo check` 在 `zstd-sys` 就断了（A 类，需 C 交叉编译器），
**永远走不到我们的代码**。也就是说：塞进去 = 写了但没有门禁。新 archive 只依赖纯 Rust 的 `snow-linux`，
每次 `bash scripts/linux-cross-check.sh` 都会真的编译它。代价是 C++ 侧多链一个静态库。

六个判断：

1. **句柄不持有连接**。Qt 在热路径上构造临时 `PhysicalCursor`（`globalcanvascontroller.cpp:68` 与 `:387`
   都是 `platform::PhysicalCursor().logicalPosition()`），若一实例一条连接，就是**每次鼠标移动做一次
   X11 握手**。所以连接放进 `OnceLock<Option<Mutex<X11Session>>>`，句柄只是一个能力令牌 +
   以后挂 per-caller 状态的地方；`create` 返回 NULL 就等于"这个会话不支持"。
2. **第一次尝试决定终身**。会话打不开要么是永久的（纯 Wayland 无 XWayland），要么是暂时的
   （合成器还没起来）。每次调用重试等于每次花一次握手去确认同一个答案；而应用本身没有 X 就跑不起来，
   所以这个 latch 的代价可以接受 —— 但**必须写在文档里**，不能让人以为是随机失败。
3. **`WarpPointer` 没有回复，所以必须显式 `flush`**。否则请求躺在外发缓冲里，直到**下一次需要回复的
   请求**顺手把它冲出去，光标会在一个不可预测的时刻跳。这类 bug 不报错、不复现，只是"有时候不灵"。
4. **越界要拒绝，不要 clamp、更不要 `as` 转**。root 坐标是 `i16`，而 C ABI 收的是 `int32_t`；
   `as i16` 会把 32768 变成 -32768，把光标送到**对面角落**。`RootPosition::new` 返回 `Option`，
   C 侧拿 0（失败）。同样地，也不 clamp 到屏幕边缘 —— 那会把调用方的 bug 藏在一个"只是位置不对"的
   现象后面。
5. **`same_screen == false` 必须当成"没有位置"**。`QueryPointer` 的坐标是相对**持有指针的那个 root**；
   本后端只连一个 screen，把别的 root 的坐标当本 root 的读出来，结果是**光标跳到左上角**，而不是读取失败。
6. **不提供 `readLogicalPosition`，也不合成鼠标事件**。X11 的 root 坐标是设备像素，而 logical 坐标
   依赖屏幕缩放 —— 那是 `QCursor::pos()` 已经算过的事，在这里再算一遍是"用更少信息给出第二个答案"。
   反过来，X11 的 warp 对服务器而言是**真实的 motion 事件**（与 macOS 的 `CGWarpMouseCursorPosition` 相反），
   所以 `generatesMouseMoveEvents` 保持 `true`，不需要 `QWindowSystemInterface` 合成。

**两个附带判断**：

- **C++ 分支要先看 `QGuiApplication::platformName() == "xcb"`**。Wayland 会话下即使有 XWayland，
  Qt 的 platform 仍是 `wayland`；此时 `XQueryPointer` 读的是 XWayland 自己那份指针副本，
  **指针离开 X 窗口后就不再跟随**。那是一个"过期的位置"，比"不支持"更糟。
- **CMake 的链接不能只写一处，也要防匹配器静默失效**。静态库可以留 undefined 符号，最终可执行文件不行；
  而仓库里多个测试 target 会**各自重编** `physicalcursor.cpp`。所以沿用本文件中已有的
  `canvas_status_readout` 遍历写法，但加一条**配置期自检**：一个 target 都没匹配到就 `FATAL_ERROR`。
  否则匹配器写错的表现会是链接期的 undefined symbol，出现在离原因很远的日志里。

**验证（2026-10-17，Windows + rustc 1.97.1）**：

| 检查 | 结果 |
|---|---|
| `cargo test -p snow-linux` | **111 通过**（slice 6 后是 105，本片 +6 = `pointer`） |
| `cargo check -p snow-linux-c` 三目标（host / x86_64-linux / aarch64-linux） | 全绿。host 上是**空 crate**（整 crate 在 `#![cfg(target_os="linux")]` 之后），Linux 上是真实现 |
| `cargo fmt --check` + `cargo clippy --all-targets`（host 与 linux target） | 双绿，`-Dwarnings` 生效 |
| **反向验证** | 往 `snow-linux-c/src/lib.rs` 末尾注入 `compile_error!` 后重跑 Linux 目标检查，**如期失败**。理由同 slice 5：整 crate 都在 crate 级 `cfg` 之后，cfg 写错会让门禁**静默通过**而真机编译出空 archive |
| `bash scripts/linux-cross-check.sh`（双架构） | **50/62**（每架构 25/31），新增的 `snow-linux-c` 双架构 PASS，A 类失败逐条未变 |

**必须说清楚的一点**：**C++ 那一侧一行都没有被编译过**。本片能保证的是
(a) 它调用的 C ABI 在两个 Linux 架构上类型正确，(b) 逻辑与坐标规则有单测，
(c) CMake 的匹配错误会在 configure 阶段大声失败，而不是在链接期。
但 `#elif defined(Q_OS_LINUX)` 分支本身是否有语法/类型错误，**只有 UOS 上的第一次构建能回答**。
这是 P4 全阶段的共同约束，不是本片的疏漏。

**本片未覆盖（诚实记账）**：

- **全屏窗口检测**、**全局鼠标 backend**、**全局快捷键（`XGrabKey`）**、托盘 / 自启动 / 单实例 / 通知、
  `screenshotnative.h` 的平台中立化 —— 都属 P4 后续 slice，桥已就位。
- **`snow_rust_ffi` 的 selector 门控放宽**（`cfg(any(windows, unix))`）：slice 5 让
  `snow-ui-selector-c` 能在 Linux 编译，但那个 archive 仍把它排除在 Linux 之外，C++ 侧
  `snow_ui_selector_service_*` 会在链接期全部 undefined。本片一并改掉，**但本机无法验证**
  （该 archive 的 Linux 目标连 `cargo check` 都过不去），只能到真机验。
- **真机待验**：`XWarpPointer` 在 dde 下是否被合成的鼠标抓取拦截（拖动中 warp 的行为）、
  多屏负坐标布局下的读数、以及 `QCursor::pos()` 与 root 坐标在 `QT_SCALE_FACTOR` 非 1 时的对应关系。

#### 9.4.9 P4 slice 2：全局快捷键（`XGrabKey`）落地（2026-10-20）

Windows 用 `RegisterHotKey`，macOS 用 Carbon 的 `RegisterEventHotKey`；X11 的等价物是**被动键盘抓取**
（`XGrabKey`）：在本连接持握期间，该 keycode + 该修饰键状态的按键只发给本客户端，别的应用收不到。
没有它，截图快捷键只在窗口有焦点时有效 —— 那等于没有。

**分层照旧：翻译规则压在纯 Rust 里，X11 交互在 `#[cfg(target_os = "linux")]` 之后，C++ 只做转发。**

| 层 | 位置 | 平台 | 内容 |
|---|---|---|---|
| 翻译规则 | `snow-linux/src/hotkey.rs` | **全平台** | Qt 键 → keysym；keysym → 可用的 keycode（含隐含 Shift）；修饰键组合 → 抓取掩码集合；修饰键映射 → Alt/Super 的 X11 mask。**18 个单测在 Windows 上跑** |
| 抓取服务 | `snow-linux/src/x11/hotkey.rs` | linux | 独立连接 + 工作线程；`register` / `unregister` / `key_down`；`KeyPress` 匹配后回调 |
| C ABI | `snow-linux-c`（`include/snow_linux.h`） | linux | `snow_linux_hotkeys_{create,destroy,register,unregister,key_down,validate_binding}` |
| 接线 | `snow_shot/src/platform/linux/globalshortcutbackend.cpp` | linux | `GlobalShortcutBackend` 的 Linux 实现；工厂 `createPlatformGlobalShortcutBackend()` 增 `#elif defined(Q_OS_LINUX)` |
| CMake | `snow_shot/CMakeLists.txt` | linux | `SNOW_SHOT_GLOBAL_SHORTCUT_SOURCES` 增 `elseif(UNIX)` 分支；文末链接遍历增一条匹配器 + **自检 `FATAL_ERROR`** |

八个判断（每条都对应一个会静默出错的坑）：

1. **Qt 键值 → keysym 必须自己建表，不能凭记忆**。Qt 没有公开的 keysym 反查表。本片把 Qt 侧取值
   从 `qnamespace.h`（5.15.2 与 6.12.0 实测一致）读出、X11 侧从 xorgproto 的 `keysymdef.h` /
   `XF86keysym.h` 读出后逐条写死。**凭记忆写的初稿错了 6 处**（`Hyper_L` 写成 `Meta_R`、
   `MediaRecord` 指向了 `XF86Mail`、`PowerOff`/`WakeUp`/`Eject`/`ScreenSaver`/`Calculator` 全偏），
   而这类错误的表现是「按了快捷键弹出邮件客户端」—— 不像 bug，像灵异。
2. **Qt 5.15 把两个媒体键的常量写少了一位**：`Key_MediaPause = 0x1000085`、`Key_MediaTogglePlayPause
   = 0x1000086`（Qt 6 已修正为 `0x01000085/86`）。Linux 发的是 Qt5，所以表里**两种拼写都接受**。
3. **字母必须做大小写折叠**。Qt 的 `Key_A` 是 `0x41`（= `XK_A`），但 X 键盘映射把 `XK_a` 放在
   **未按 Shift 的那一层**。只查 `0x41` 会命中 shifted 层，于是 `Ctrl+A` 被悄悄注册成
   `Ctrl+Shift+A` —— 能注册、永远不触发。折叠到小写 keysym 才能命中 unshifted 层；
   同理 `+` 这类字符确实在 shifted 层，此时**必须把 Shift 加进抓取掩码**。
4. **抓取要覆盖所有锁定键组合**。`XGrabKey` 的修饰键匹配是**精确匹配**，只抓 `Control` 的话，
   CapsLock 一开快捷键就失效。按键盘上实际存在的锁定修饰键（`Lock` + 实测到的 NumLock /
   ScrollLock 掩码）做幂集，最多 8 个抓取。
5. **Alt / Super 的 X11 mask 要读回来**。协议只定义 `Mod1..Mod5`，谁是谁由布局决定。从
   `GetModifierMapping` + 键盘映射反查 `Alt_L/R`、`Super_L/R`，查不到才退回 `Mod1`/`Mod4`。
   写死 `Mod1` 在某些布局上会「注册成功但永不触发」。
6. **一个组合只能属于一个注册**。同一客户端对同一组合抓两次 X 是允许的，但那样一次按键会触发
   两个动作；Windows 的第二次 `RegisterHotKey` 会返回「已被占用」，所以这里也回
   `ALREADY_IN_USE`，让两端的提示一致。
7. **失败必须整条回滚**。多个 keycode（多布局）时若第 3 个抓取失败而前两个留着，绑定会变成
   「CapsLock 关着能触发、开着不能」这种「偶发」现象。失败即 ungrab 已发的全部。
8. **回调不能直接摸 Qt**。`KeyPress` 在 Rust 工作线程上，C++ 侧用 `QMetaObject::invokeMethod`
   投递到 Qt 线程，且以 `ActivationDispatcher` 作 context 对象 —— QObject 析构时 Qt 会丢弃
   尚未执行的 metacall，这正是「已经投递但后端刚被销毁」这一类崩溃的解药。同理
   `snow_linux_hotkeys_destroy` 会 join 工作线程后才返回，所以 `user_data` 不会在析构后被动到。

**另外补了一道本机门禁（这是本片真正的可验证性来源）**：`src/platform/linux/*.cpp` 在 Windows 构建里
一行都不会被编译，但它的 Qt 用法是平台中立的。于是
`bash scripts/syntax-check-linux-cpp.sh` 用 `cl -Zs`（只做前端，不产出目标文件）
加 `-DQ_OS_LINUX` 把这批文件过一遍 `-W4 -permissive-` 语法/类型检查。
**反向验证过**：注入一个改名的函数调用后如期报 `C3861`，删掉即恢复 `SYNTAX OK`。
这让「C++ 侧只能靠眼睛看」的范围缩小到只剩 ABI 与运行期行为。

**验证（2026-10-20，Windows + rustc 1.97.1）**：

| 检查 | 结果 |
|---|---|
| `cargo test -p snow-linux --lib hotkey` | **18 通过**（新增，全部纯逻辑） |
| `cargo check -p snow-linux -p snow-linux-c` 双架构 | 全绿，`-Dwarnings` 生效 |
| `cargo clippy --all-targets` / `cargo fmt --check` | 双绿 |
| `bash scripts/syntax-check-linux-cpp.sh` | `SYNTAX OK`（+ 反向验证会红） |
| `bash scripts/linux-cross-check.sh` 双架构 | 见 9.4.2 的复跑行，分母再次 +0（本片没有新增 workspace 成员） |

**本片未覆盖（诚实记账）**：

- **Wayland 会话下不支持**：`QGuiApplication::platformName() != "xcb"` 时后端整体降级为
  「平台不支持」，与物理光标分支同一判据。等 P6/P7 的 portal（`org.freedesktop.portal.GlobalShortcuts`）
  再接。
- **真机待验**：dde 是否把某些组合先截走（`XGrabKey` 会返回 `BadAccess`，此时 UI 上应显示
  「已被占用」）；多布局下同一键产生多个 keycode 的实际行为；长按重复触发频率与 Windows 的差异；
  `QT_SCALE_FACTOR` 非 1 时 `inputState` 的读数（与缩放无关，但需实测确认）。
- **全局鼠标 backend / 全屏窗口检测 / 托盘 / 自启动 / 单实例 / 通知** 仍是 P4 后续 slice。

#### 9.4.10 P4 slice 3：全屏窗口检测（2026-10-20）

两个产品行为依赖它：**全屏应用获焦时暂停全局快捷键**（`GlobalShortcutManager`），**全屏时收起悬浮工具条**
（`FloatingToolbarController`）。Windows 用「前台窗口 + `DWMWA_EXTENDED_FRAME_BOUNDS` 等于某台显示器矩形」，
macOS 用 AX 的窗口列表；X11 的等价物是「`_NET_ACTIVE_WINDOW` 的顶层框架矩形等于某台 RandR 显示器矩形」。

**分层照旧，本片新增的都是「查什么 + 怎么比」的规则：**

| 层 | 位置 | 平台 | 内容 |
|---|---|---|---|
| 判定规则 | `snow-linux/src/fullscreen.rs` | **全平台** | `covers_display`（±1px 容差）、`focused_is_fullscreen`（种类/可视图/空矩形过滤）、`logical_to_native`（Qt 逻辑坐标 × dpr）。**16 个单测在 Windows 上跑** |
| X11 读取 | `snow-linux/src/x11/fullscreen.rs` | linux | `_NET_ACTIVE_WINDOW`（回退 `GetInputFocus`）→ 爬到 parent 为 root 的顶层 → 顶层几何 + `_NET_WM_WINDOW_TYPE` |
| 原子复用 | `snow-linux/src/x11/window.rs` | linux | 把 `Atoms` 拆出 `TypeAtoms`（一次批量 intern），X11 拾取与全屏检测共用同一套类型原子 |
| C ABI | `snow-linux-c` | linux | `snow_linux_platform_focused_fullscreen` / `..._on_display(x,y,w,h,scale)` |
| 接线 | `snow_shot/src/platform/linux/focusedfullscreenwindow.cpp` | linux | 实现 `snow_shot::platform::{focusedFullscreenWindowExists, OnScreen}` |
| CMake | `snow_shot/CMakeLists.txt` | linux | `SNOW_SHOT_GLOBAL_SHORTCUT_SOURCES` 的 `UNIX` 分支加第二源文件；文末链接遍历加**第三条匹配器 + 自检** |

九个判断（前三条任意一条错，症状都是「产品行为静默错位」而不是崩溃）：

1. **桌面窗口精确覆盖显示器，所以几何相等不足以判定全屏**。`_NET_WM_WINDOW_TYPE_DESKTOP` / `DOCK`
   平时就跟显示器一样大。若不先按类型排除，**空桌面上所有快捷键会失效**，而且屏幕上没有任何东西
   能解释为什么 —— 这是本片最贵的一条，已写成单测 `an_empty_desktop_is_not_fullscreen`。
2. **必须读顶层（parent == root 的祖先）的几何，不是客户区几何**。重定向窗口管理器（mutter/kwin/
   xfwm/openbox 都是）把客户窗口嵌进自己画的框架里，框架才是用户看到的边界；读客户区会**少一圈标题栏**，
   于是一个「最大化」窗口看起来比显示器矮一截、一个真正的全屏窗口看起来也差一点 —— 两侧都判错。
3. **类型属性要沿祖先链往上找**。`_NET_WM_WINDOW_TYPE` 由应用设在**客户窗口**上，框架窗口没有这个属性；
   只看顶层会把每个应用都当成 `Normal`（桌面窗口的 `DESKTOP` 也就丢了）。反过来只读焦点窗口则会在
   焦点落在子窗口时漏掉。
4. **比对的是「某一台显示器」，不是整个桌面**。跨两块屏铺满的窗口是最大化窗口，不是全屏；这条同时
   让两端判据一致（Windows 也是 `MONITOR_DEFAULTTONULL` 那台的 `rcMonitor`）。
5. **不要求 `_NET_WM_STATE_FULLSCREEN`**。Wine 下的游戏、自带尺寸的视频播放器很多不设这个属性；
   而真正设了它的窗口管理器也会把窗口改到显示器大小 —— 也就是说几何判据已经涵盖了它。
6. **焦点要有回退**。没有窗口管理器时 `_NET_ACTIVE_WINDOW` 可能根本不存在（属性 `type == NONE`），
   或者存在但为 0。此时读 `GetInputFocus`，并把 `None`(0) / `PointerRoot`(1) 视为「没有窗口」——
   这两个值是协议里的哨兵，不是窗口 id。
7. **Qt 逻辑坐标 → 根像素的换算由 Rust 做，dpr 由调用方传**。X11 根坐标是设备像素，Qt 的逻辑坐标在缩放会话下
   是它的一部分（9.4.8 的物理光标已经确立这个分工）。把乘法和舍入放进 `logical_to_native` 才能给
   1.25 / 1.5 这类分数比例写单测；放在 C++ 里就只剩真机手测。
8. **判不出来时的答案必须是「不是全屏」**。窗口可能在「读属性」和「读几何」之间被销毁（这是常态竞态），
   此时逐请求允许失败、整体返回 `false`，而不是把错误抛给调用方去猜。答错的代价是「快捷键照常工作、
   工具条照常显示」—— 功能仍在，只是没被抑制；反过来会是「快捷键在最需要的时候失灵」。
9. **Wayland 会话降级为 `false`**，判据与 9.4.9 相同（`platformName() != "xcb"`）。XWayland 下 X 服务器
   只看得见 X 客户端，焦点窗口不是用户正在看的那个应用，宁可让它答「没有全屏」。
   *（另外两处易漏的接线：共享的 `src/platform/focusedfullscreenwindow.cpp` 里的兜底桩必须同时排除
   `Q_OS_LINUX`，否则 Linux 上是**重复符号**；`SNOW_SHOT_GLOBAL_SHORTCUT_SOURCES` 加文件之后，
   文末那段「谁编译了它就得链 X11 归档」的遍历也要加匹配器，否则是**未定义符号**。）*

**顺手修掉的一个真 bug（由本片自己的极端坐标测试抓到）**：`covers_display` 初版写的是
`(lhs - rhs).abs() <= 1`。`i32::MIN` 与 `0` 相减会溢出，在 release 下绕回一个恰好在容差内的差值 ——
两个「绝不可能相等」的矩形被判为相等。改成 `abs_diff`（`u32` 语义，永不溢出）。
这类算术错误在正常多屏坐标下**永远跑不出来**，只有表格驱动的单测能抓到。

**验证（2026-10-20，Windows + rustc 1.97.1）**：

| 检查 | 结果 |
|---|---|
| `cargo test -p snow-linux --lib` | **147 通过**（slice 2 是 131，本片 +16，全部纯逻辑） |
| `cargo check` / `clippy --all-targets` 双架构 | 全绿，`-Dwarnings` 生效 |
| `bash scripts/linux-cross-check.sh` 双架构 | 见 9.4.2 的复跑行 |
| `bash scripts/syntax-check-linux-cpp.sh` | `SYNTAX OK`（默认覆盖面从 1 个文件扩到 2 个；改名注入仍如期 `C3861`） |
| 共享桩文件的 Windows 分支 | `windows-qt5-smoke` **不编译** `snow_shot` 的源文件（它只开 `ant_design_qt` + `snow_image`），所以本片用等价的 `cl -Zs -W4 -permissive-` 单独过了 `src/platform/focusedfullscreenwindow.cpp` 的 Windows 分支：0 error 0 warning |

**本片未覆盖（诚实记账）**：

- **未在 X11 真机跑过**：`_NET_ACTIVE_WINDOW` 在 dde/kwin 上的刷新时机、全屏但未获焦点的窗口
  （多屏时很常见：一块屏全屏播放、另一块屏在打字）、`--scale` 参与的布局下 Qt 逻辑坐标与根像素的
  对应关系，都只能在 UOS 上确认。
- **本片没有任何「按显示器」的真机证据**：`focusedFullscreenWindowOnScreen` 的容差是按单屏场景设计的，
  跨屏混合 dpr 的落点需要真机。
- **全局鼠标 backend / 托盘 / 自启动 / 单实例 / 通知 / `screenshotnative.h` 拆分** 仍是 P4 后续 slice。

#### 9.4.11 P4 slice 4：全局鼠标手势（`XGrabButton`）落地（2026-10-20）

产品行为：**按住修饰键组合 + 拖鼠标按钮，就在桌面任意位置开始截图**（`GlobalMouseManager` → `dragEvent`
→ `ScreenshotController`）。Windows 用 `WH_MOUSE_LL` 低级钩子，macOS 用 `CGEventTap`；X11 的等价物是
**被动按钮抓取 `GrabButton`**。

**本片最核心的一条结论：这个功能必须由 X 服务器做匹配，而不是由我们观测后决定。**

| 方案 | 能不能「吞掉」按下 | 结论 |
|---|---|---|
| XInput2 raw events（`XI_RawButtonPress`） | **不能**。raw 事件按定义对投递是中立的，它只是「旁听」；应用照样收到按下并开始自己的拖拽 | 与 Windows 钩子语义不同，弃用 |
| 记录 `XI2` 普通事件 / 轮询按钮状态 | 同上，且漏掉「按下到轮询之间」的窗口 | 弃用 |
| **被动 `XGrabButton`（root 上，精确修饰键掩码）** | **能**。抓取期间该组合的按下只投递给本连接，其他客户端一个字节都收不到 | **采用** |

代价是「匹配权」交给了服务器：**落到我们事件循环里的按下，必然是某个已配置的组合**，于是 Rust 侧不需要
「这个按下是不是我的」这种运行期判断，也不需要 `XAllowEvents(ReplayPointer)` 回放 —— 这一片因此比
Windows/macOS 的后端**更简单**，代价是 C++ 侧共享的 `GlobalMouseGesture` 状态机在 X11 上不参与判断
（Windows/macOS 仍然用它）。

分层：

| 层 | 位置 | 平台 | 内容 |
|---|---|---|---|
| 规则 | `snow-linux/src/globalmouse.rs` | **全平台** | Qt 按钮 → X 按钮号、组合展开成抓取集合、歧义组合整条丢弃。**12 个单测在 Windows 上跑** |
| 键盘表复用 | `snow-linux/src/x11/keymap.rs` | linux | 从 `x11/hotkey.rs` 提出来的 `Keymap`（键码表 + Alt/Super 掩码 + 锁掩码），快捷键与鼠标抓取共用 |
| X11 抓取 | `snow-linux/src/x11/globalmouse.rs` | linux | `GlobalMouseService`：独立连接 + 工作线程，按下/移动/松开 → `Begin/Update/Finish`，`cancel` 只让拖拽静音 |
| C ABI | `snow-linux-c` | linux | `snow_linux_global_mouse_{create,destroy,set_bindings,set_enabled,cancel}` |
| 接线 | `snow_shot/src/platform/linux/globalmousebackend.cpp` | linux | `GlobalMouseBackend` 的 Linux 实现；Wayland 会话报 `Unavailable` |
| 共享桩 | `snow_shot/src/platform/globalmousebackend.cpp` | 全平台 | `#elif defined(Q_OS_LINUX)` 分支（原本只有 Win/macOS/兜底） |
| CMake | `snow_shot/CMakeLists.txt` | linux | `snow_shot_global_mouse` 的 `UNIX` 分支加源文件；文末 X11 链接遍历**重构成清单驱动**（见下） |

七条判断（前三条错，症状同样是「行为静默错位」）：

1. **Qt 的按钮枚举和 X11 的按钮号不是一回事，而且顺序相反**。Qt 是位标志 `1/2/4/8/16`（左/右/中/后退/前进），
   X11 按协议分配顺序编号：左 1、**中 2**、**右 3**、滚轮 4–7、侧键 8/9。按位置翻译会把「右键拖拽」变成
   「中键拖拽」，且在只有左右键的配置下完全看不出来。
2. **`captureAvailable == false` 时必须撤掉抓取，而不是「忽略按下」**。忽略仍然吞掉了那一次点击
   （抓取已经激活），要还回去就得回放；而撤掉抓取是零成本的。C++ 侧 `setCaptureAvailable(false)`
   （另一次截图正在进行）就走到这条路径上。
3. **两个动作配了同一组合时，整条丢弃**。共享的 `GlobalMouseGesture` 对歧义按下的处理是「不匹配」，
   Windows/macOS 上应用照常收到点击；X11 若把抓取给了其中一个，同一份配置在两个平台行为不同。
4. **锁状态变体照旧要展开**（`grab_masks`，与快捷键同一个函数）。X11 的抓取匹配是**精确**的，
   CapsLock/NumLock 开着时 `Ctrl+左键` 的状态位多一位 —— 不展开就是「一开大写锁定，手势就失灵」。
5. **抓取失败是正常答案，不是错误**。窗口管理器会自己抓 `Super`+拖拽之类（移动窗口），
   `BadAccess` 在那里是预期结果；`set_bindings` 返回**成功装上的条数**，让调用方能看出「有几条没抢到」，
   而不是让一条冲突把其余全废掉。
6. **拖拽期间指针归本进程，指针不动的是「应用」，不是光标**。`GrabButton` 用 `pointer_mode: ASYNC`：
   光标继续跟随鼠标移动，事件进本连接；`owner_events: false` 保证事件不回流给指针下的窗口。
   于是拖拽过程中下层应用收不到任何鼠标事件（Windows 上它还能收到移动，只是没有拖拽语义）——
   这是 X11 语义带来的**更强**隔离，也是它「不会意外拖到下层窗口」的原因。
7. **`beginButtonDrag` 在 X11 上是空实现，并且写清了为什么**。这个入口服务于首页卡片的
   「按住卡片再拖到桌面」：按下**已经**投递给本程序自己的 Qt 窗口，Qt 连接持有隐式抓取，
   此时 `XGrabPointer` 会 `AlreadyGrabbed` 失败，移动事件继续进 Qt 窗口而不是本服务 ——
   结果是「报一个 Begin 然后什么都没有」。正确做法是走 Qt 自己的拖拽 + `QCursor::pos()` 跟随，
   那是另一套设计；在它落地前，卡片拖拽在 Linux 上是**明确不可用**，而不是半工作状态。

**顺手做的两处结构整理**（都在本片被真实踩到）：

- `x11/hotkey.rs` 里的 `Tables` 提成 `x11/keymap.rs` 的 `Keymap`。鼠标抓取需要**同一套** Alt/Super 掩码
  与锁掩码，复制一份意味着以后修「Alt 到底在 Mod1 还是 Mod3」要改两处。
- `snow_shot/CMakeLists.txt` 文末的「谁编译了平台源，谁就链 X11 归档」从**三条重复的 if** 重构成
  一份清单 + 两个循环：加文件现在是一行，而「没人编译它」的 `FATAL_ERROR` 自检保留。这段逻辑在
  Windows 上不执行（`if(UNIX AND NOT APPLE)`），所以本片用一个等价小工程做了**双向验证**：
  四条齐 → configure 通过；抽掉一条 → `FATAL_ERROR`。

**验证（2026-10-20，Windows + rustc 1.97.1）**：

| 检查 | 结果 |
|---|---|
| `cargo test -p snow-linux --lib` | **159 通过**（slice 3 是 147，本片 +12，全部纯逻辑） |
| `cargo check` / `clippy --all-targets` 双架构 | 全绿，`-Dwarnings` 生效 |
| `cargo fmt --check` / clang-format `--dry-run -Werror` | 干净 |
| `bash scripts/syntax-check-linux-cpp.sh` | `SYNTAX OK`（默认覆盖面从 2 个文件扩到 3 个；把 `snow_linux_global_mouse_cancel` 改名注入后如期 `C3861` + `SYNTAX FAILED`） |
| `windows-qt5-full` configure + `snow_shot_global_mouse` | configure `EXIT=0`；该静态库 16 步全绿（共享桩的 `#elif` 分支参与编译） |
| `bash scripts/linux-cross-check.sh` 双架构 | 见 9.4.2 的复跑行 |

**本片未覆盖（诚实记账）**：

- **未在 X11 真机跑过**：`GrabButton` 在 dde/kwin 上的实际冲突情况（哪些组合已被 WM 占用）、
  侧键（8/9）在真实鼠标上的映射、拖拽期间合成器对光标的重绘。
- **没有「按下归属」的真机证据**：抓取确实让下层应用收不到按下的直接证据（例如在文本编辑器里
  用同类组合拖拽），只能在 UOS 上验证。
- **布局切换（`MappingNotify`）不刷新掩码**：与快捷键服务同一个取舍，重建服务时才重读。
- **托盘 / 自启动 / 单实例 / 通知 / `screenshotnative.h` 拆分** 仍是 P4 后续 slice。

#### 9.4.12 P4 slice 5：自启动走 XDG autostart 落地（2026-10-20）

**起点是一处假成功。** `platform::windows::AutoStartRegistration` 的每个函数都有非 Windows 兜底：
`isSupported()` 返回 false，于是设置页的开关**灰掉**；而 `reconcileStartupMode()` 的 `#else` 分支返回
`{true, false, {}}` —— 「成功」但一个字节都没写。后者是本项目反复强调的那种缺陷：调用方看到成功，
用户看到开关是灰的，中间没有任何一处说实话。

**XDG autostart 是这里唯一可用的基线。** 一个 `.desktop` 文件放在 `$XDG_CONFIG_HOME/autostart/`
（默认 `~/.config/autostart/`），桌面会话登录时读它。系统级对应 `$XDG_CONFIG_DIRS/autostart`
（默认 `/etc/xdg/autostart`），**同名时用户级优先**。这里没有 Windows 那种「一个注册表值就是全部状态」
的单一位置：用户级文件、系统级文件、`Hidden` 键三者共同决定实际行为，所以判断「是否启用」必须三处一起看。

分层：

| 层 | 位置 | 平台 | 内容 |
|---|---|---|---|
| 纯逻辑 | `snow_shot/src/platform/linux/desktopentry.cpp` | **全平台** | `Exec` 转义与反解、条目生成、键读取、启用判定。**零文件系统访问**，因此在 Windows 上跑单测 |
| 文件与目录 | `snow_shot/src/platform/linux/autostartregistration.cpp` | linux | `$XDG_CONFIG_HOME` 解析、系统级扫描、`QSaveFile` 原子写、快照/回滚、`reconcile` |
| C++ 接线 | `main.cpp` / `settingsbackend.cpp` / `settingscatalog.cpp` | linux | 启动对账、开关读写、条目文案与可见性 |
| 测试 | `snow_shot/tests/linux_autostart_tests.cpp` | 全平台 | 纯逻辑的 20 余条断言，含「写→读」闭环 |

六条判断（前四条错了都是静默错位）：

1. **`Exec` 有两层转义，顺序不能反**。规范说 string 层的转义「在 quoting 规则之前应用」，于是：
   字面反斜杠在引号参数里要写 **四个**；字面 `$` 要写 `\$`。写少一层，路径里的反斜杠就会被
   shell 层吃掉一位。
2. **`%%` 是字面百分号，而且引号内也成立**。本条是本片测试自己抓出来的：初版按「引号内不展开
   field code，所以引号内的 `%` 不必双写」处理，结果往返失败 —— 读回来多一个 `%`。
   `%%` 属于**转义**而不是**展开**，规范对转义没有引号内例外。
3. **禁用时不能只删自己的文件**。如果打包时装了系统级条目，删掉用户级文件等于让系统级那条重新生效 ——
   用户点了「关」，重启后照样自启。规范的解法是写一个 `Hidden=true` 的存根把同名条目遮住。
4. **`X-GNOME-Autostart-enabled=false` 也必须认**。GNOME 的「启动应用程序」面板禁用时写这个键而**不**写
   `Hidden`。不认它，用户在面板里关掉、我们的开关还显示「开」。
5. **文件名跟 `productId()` 走，不跟显示名走**。文件名同时是「覆盖哪条系统级条目」的身份，
   用翻译过的显示名意味着切换语言就换了要遮的文件。
6. **比较「参数列表」而不是「文本」**。`matchesExpectedEntry()` 先用 `parseExec()` 反解再比 argv：
   打包者用不同但等价的引号写法时，不会每次启动都被判不匹配而重写。

**顺手修的两处 Linux 产品缺陷**（都是本片接线时才暴露的）：

- 设置项文案原样写着「Start Snow Shot in the background when **Windows starts**」——`autoStartItem()` 的
  `#else` 分支是 Windows 专用，被 Linux 借用。现改为三分支，Linux 用「登录桌面后启动」。
- 「Launch as administrator」与「Restart as administrator」两项在 Linux 上仍会构建出来：桌面会话没有提权
  概念，它们只能永远灰着，附带一句「需要 Administrators 组成员资格」的 Windows 解释。现改为只在
  Windows 构建；`switchEnabled` 同时对该 binding 返回 false，避免陈旧调用落到无意义的操作上。

**验证（2026-10-20，Windows + Qt 5.15.2 / MSVC 14.44）**：

| 检查 | 结果 |
|---|---|
| `snow-shot-linux-autostart-tests`（手工 `cl` 编译链接后运行） | **passed**；`-W4 -permissive-` 0 warning（STL4043 系 Qt5 头噪声除外） |
| 测试抓到的真 bug | 往返用例 `/opt/100%/snow_shot` 读回 `/opt/100%%/snow_shot` ⇒ 补 `parseExec` 的 `%%` 反解 + 引号内双写 |
| `bash scripts/syntax-check-linux-cpp.sh` | `SYNTAX OK`（覆盖面 3 → **5** 个文件）；把 `desktopentry::parseExec` 改名注入后如期 `C2039` + `C3861` + `SYNTAX FAILED` |
| `windows-qt5-full` configure（tests 保持 OFF） | `EXIT=0`，`-- Generating done` —— 加进 `snow_shot_administrator` 的两个源文件没有破坏既有配置 |
| `windows-qt5-full` build `snow_shot_administrator` | 5 步全绿；两个新文件在 Windows 上编的是 `#else` 兜底分支，静态库链接成功 |
| Qt5 路线开 `SNOW_APPS_BUILD_TESTS=ON` | **仍不可用**，见下 |

**验证时拆掉的一层既有阻塞（并没能拆完）**：想在 Windows 上真正配置出 `snow-shot-linux-autostart-tests`
这个 CTest 目标，必须开 `SNOW_APPS_BUILD_TESTS=ON`，而 Qt5 路线在这个开关上是**从来没被走通过**的
（三档 `windows-qt5-*` 全部 `BUILD_TESTS=OFF`）。依次撞到两层：

1. `qt_import_plugins(... NO_DEFAULT ...)` —— `cmake/SnowQtVersionShims.cmake` 里那条垫片用的是
   `if(NOT COMMAND qt_import_plugins)` 守卫，而 **Qt 5.15 自带 versionless 的 `qt_import_plugins`**，
   守卫恒假、垫片根本没装；Qt 自带那版原样转发 `NO_DEFAULT` 给 `qt5_import_plugins`，后者
   `FATAL_ERROR`。已改为**无条件覆盖**（有意的例外，理由写在文件里），因为那一版对 `NO_DEFAULT`
   的处理是错的而不是风格差异；Qt5 的插件本来就是运行时加载，没有默认插件集可关，丢掉这个关键字
   是语义已满足而非降级。
2. `ant_design_qt/CMakeLists.txt` 的测试用 `$<TARGET_FILE_DIR:Qt::Core>` 拼 PATH —— Qt5 的 `Qt::Core`
   是 INTERFACE 目标而不是 alias，生成阶段直接报 "not an executable or library"（`add_test` 共 5 处）。
   **这一层没修**：它属于 Qt5 降级路线的测试基础设施，与本片无关，修它要改 5 处生成表达式并逐个验证。

因此本片的测试目标**没有被 CMake 配置验证过**：验证分成「源文件能被 MSVC 编译并链接成静态库」
（已做）与「测试逻辑正确」（手工 `cl` 编译链接后运行，已做）两段，CTest 接线留待 Qt5 测试基建打通后一并确认。

**本片未覆盖（诚实记账）**：

- **未在真实桌面会话验证**：dde（UOS 默认桌面）读 `~/.config/autostart` 的实际行为、以及它是否会在
  用户改设置后重写该文件；GNOME/KDE 的差异。
- **Flatpak / Snap 沙箱未检测**：沙箱里的 `$XDG_CONFIG_HOME` 被重定向，写进去的条目在宿主侧不生效，
  而 `isSupported()` 恒 true。打包阶段（P6）如果走 Flatpak，这里需要一条显式判据。
- **`OnlyShowIn` / `NotShowIn` / `TryExec` 不参与判定**：它们决定「这个桌面是否启动」，不是「用户是否
  要求启动」。判成「关」会让开关和我们写下的条目自相矛盾。
- **托盘 / 单实例 / 通知 / `screenshotnative.h` 拆分** 仍是 P4 后续 slice。单实例经查是纯 Qt
  （`QLocalServer` + `QLockFile`，`src/app/singleinstancecoordinator.cpp` 全文件无平台条件编译），
  在 Linux 上应当直接可用，但尚未在真机确认 socket 路径可用。

#### 9.4.13 P4 slice 6：桌面通知走 `org.freedesktop.Notifications` 落地（2026-10-20）

**起点与 9.4.12 同型：Linux 一直在用 Windows 那条 `#else` 分支。** `SystemTrayController` 的通知投递在
非 macOS 上统一是 `QSystemTrayIcon::showMessage()`，也就是**经 StatusNotifier 项转手**。这有两个后果：
桌面没有 SNI 宿主时（纯 X11 会话、部分轻量 WM）**什么都不显示**，而回调仍然 `completed({})` 报 Accepted；
`supportsMessages()` 在 Linux 上取决于 SNI 后端自报，不是「能不能显示」的真实判据。

**通知 daemon 是会话服务，与显示协议无关。** 所以这一片不需要 Wayland 分支（与采集、快捷键、全屏检测
都不同）：`org.freedesktop.Notifications` 在 X11 和 Wayland 会话里都由同一个 daemon 提供，
Deepin/UOS 上是 `dde-daemon` 的 notification 服务。判据只有「会话总线连得上」与「这个名字已注册」。

分层（与 9.4.12 同一套切法）：

| 层 | 位置 | 平台 | 内容 |
|---|---|---|---|
| 纯逻辑 | `snow_shot/src/platform/linux/notificationpayload.cpp` | **全平台** | 转义、urgency 字节、图标名、过期时长、`actions` 数组、`Notify` 八参数的顺序与类型。**不碰总线**，因此在 Windows 上跑单测 |
| 总线 | `snow_shot/src/platform/linux/systemnotificationservice.cpp` | linux | `QDBusConnection::sessionBus()`、`GetCapabilities` 探测、异步 `Notify`、`ActionInvoked` / `NotificationClosed` 回执 |
| C++ 接线 | `systemtraycontroller.cpp` 的 `#elif defined(Q_OS_LINUX)` | linux | 投递改用 daemon；仅在「后端缺失」时退回托盘 balloon |
| 测试 | `snow_shot/tests/linux_notification_tests.cpp` | 全平台 | 纯逻辑的 30 余条断言，含参数顺序与变体类型 |

七条判断（前四条错了都是**运行期**才暴露的静默失败）：

1. **`Notify` 的八个参数有类型要求，写错是整条消息被拒，不是「显示得不理想」**。`replaces_id` 必须是
   `uint`（写成 `int` 会以 `i` 上线，daemon 按内省签名 `u` 校验 ⇒ `InvalidArgs`），`expire_timeout`
   必须是 `int`，而 `urgency` 在 `a{sv}` 里必须是**字节**（`y`），不是 int。测试因此断言的是
   `snowMetaTypeOf()` 的类型名，而不只是值。
2. **body 是 HTML 子集，路径里的 `&` / `<` / `>` 必须转义** ——否则 `/home/u/a&b<c>.mp4` 被当成标签碎片
   吞掉或断行。但反转也要认：daemon 不支持 `body-markup` 时**原样发**，否则用户看到的是 `&amp;`。
   判据来自 `GetCapabilities`，异步取一次并缓存；取不到就按「支持」处理（现网 daemon 全都支持，
   猜错的代价只是某一处 `&` 多显示几个字符）。
3. **只发 `default` 键、标签留空**。这既保住「点气泡本体就触发」的交互，又不必给按钮文案新增 msgid ——
   本片一行 `tr()` 都没加，翻译目录（`catalog-dir` 那套 split/merge）不需要动。
4. **`replaces_id` 按 action 维度记上一枚 id**：同一类通知重复出现时替换旧气泡而不是叠第二枚；
   收到 `NotificationClosed` 就把这枚 id 忘掉（daemon 可能回收 id，继续拿它当 `replaces_id` 是错的）。
5. **只把「后端缺失」降级到托盘**。`AccessDenied` 来自 daemon 策略或用户的勿扰状态，穿透到托盘 balloon
   等于绕开用户意图。
6. **托盘开关不再屏蔽通知**。`enabled` 在 Windows 上同时管「图标」和「气球」，在 Linux 上现在只管图标；
   `activateMessage()` / `showBalloon()` 里那两道 `if (enabled)` 与 macOS 一样排除 Linux，
   否则「关了托盘图标就收不到导出完成提示」。
7. **`desktop-entry` hint 指向 `productId()`（`snow-shot`）**，供 daemon 找 `.desktop` 拿标题与图标。
   该文件由 P6 安装；未安装时 daemon 退到 `app_icon`（`dialog-information` 等标准主题名），不会报错。

**验证（2026-10-20，Windows + Qt 5.15.2 / Qt 6.12.0，MSVC 14.44）**：

| 检查 | 结果 |
|---|---|
| `snow-shot-linux-notification-tests`（手工 `cl` 编译链接后运行，Qt **5.15.2**） | **passed** |
| 同一份源与测试，手工 `cl` + Qt **6.12.0** | **passed**（需 `/Zc:__cplusplus`，Qt6 头文件硬要求） |
| 测试抓到的真 bug | `QMetaType::name()` 在 Qt 5.15 返回**按值的 `QByteArray`**、在 Qt6 返回 `const char*`；按 `const char*` 接临时对象 ⇒ 悬垂读取，实际读到 `"Q\0\0…"`。测试改用 `SNOW_QT6` 分流 |
| `bash scripts/syntax-check-linux-cpp.sh` | `SYNTAX OK`（覆盖面 5 → **6** 个文件，新增 `systemnotificationservice.cpp`，并给门禁补了 `QtDBus` 头目录） |
| 托盘投递 lambda 的语法 | 单独写探针 TU（不引 ant_design）过 `-Zs`，`EXIT=0` —— `systemtraycontroller.cpp` 因 ant_design 依赖进不了门禁 |
| `windows-qt5-full` configure | `EXIT=0`（`-- Generating done`）：新库与四处链接改动没有破坏既有配置 |
| `windows-qt5-full` build `snow_shot_notification_payload` | 3 步全绿（MOC + 编译 + 静态库） |
| CTest 目标本身 | **仍未经 CMake 配置验证**，原因同 9.4.12：Qt5 路线开不了 `SNOW_APPS_BUILD_TESTS`；本机唯一的 Qt6 树是 smoke 档，根本没 `add_subdirectory(snow_shot)` |

**本片未覆盖（诚实记账）**：

- **未在真机 daemon 上跑过**：`dde-daemon` 的实际收包、勿扰状态下的返回码、过期时长是否被尊重、
  点击气泡是否真能回到 `ActionInvoked`。这些只能到 UOS 上验。
- **`GetCapabilities` 存在竞态**：探测是异步的，第一枚通知可能早于答复到达，此时按「支持 markup」
  发送。只在 body 含 `<`/`&` 且 daemon 恰不支持 markup 时有可见差异。
- **未做 `CloseNotification`**：动作触发后依赖 daemon 自行关闭；有的 daemon 会把气泡留着。
- **多显示器 / 锁屏 / 通知中心的历史记录** 的呈现差异未验。
- **`screenshotnative.h` 拆分** 仍是 P4 剩余 slice。

#### 9.4.14 P4 slice 7：托盘导出 StatusNotifierItem（2026-10-20）

**问题**：托盘从头到尾是 `QSystemTrayIcon`。Qt 5 的托盘实现不在 `QtWidgets` 里，而在**平台插件**
里：`xcb` 插件带一个 `StatusNotifierItem`，并且在有 XEmbed 管理器时还能退回 XEmbed。于是：

- Wayland 会话（Qt5 `wayland` 插件）**没有**任何托盘后端，`show()` 是空操作；
- 跑在 XWayland 里时图标"存在"，但它属于 XWayland 的那棵 X 树，合成器的面板看不到；
- 而主程序是 `QApplication::setQuitOnLastWindowClosed(false)`。托盘不见了，用户关掉主窗口后
  进程还在、却没有入口 —— 这不是"少一个图标"，是把应用弄丢了。

**做法**：自己导出 `org.kde.StatusNotifierItem`。这个对象是**会话总线**上的，跟显示协议无关，
理由和 slice 6 完全一样 —— 也就是它天生在 X11 与 Wayland 上同时可用。

**分层**（沿用 slice 5/6 的切法）

| 层 | 文件 | 平台 |
|---|---|---|
| 纯逻辑 | `platform/linux/statusnotifierpayload.cpp` | 全平台（不碰总线，Windows 跑单测） |
| 总线 | `platform/linux/statusnotifierservice.cpp` | linux（QtDBus 对象导出） |
| 接线 | `systemtraycontroller.cpp` 的 `#elif Q_OS_LINUX` | linux |
| 测试 | `tests/linux_tray_tests.cpp`（34 条断言） | 全平台 |

**何时才导出，写成纯函数 + 断言**：`needsStatusNotifierItem(platformTrayAvailable,
waylandSession) = !platformTrayAvailable || waylandSession`。也就是**平台插件看不见就不用了它**，
其余情况交给插件（同一个进程两个图标比没有图标更糟）。第二条判据 `isWaylandSession(WAYLAND_DISPLAY,
XDG_SESSION_TYPE)`：`XDG_SESSION_TYPE` 优先 —— `WAYLAND_DISPLAY` 会跟着从 Wayland 会话里启动的
XWayland 客户端一起活下来，只看它会把 x11 会话误判成 Wayland。

**图标必须逐字节重排**。`IconPixmap` 的类型是 `a(iiay)`：ARGB32、**网络字节序**（A,R,G,B 依次）。
`QImage::Format_ARGB32` 在任何目标平台上都是 host byte order 的 `0xAARRGGBB`，所以每个像素都要按
qAlpha/qRed/qGreen/qBlue 取出来重写，并且按 `bytesPerLine` 遍历 —— 外部缓冲、子图这类 stride 大于
`width * 4` 的图像不能直接 `memcpy`，否则之后每一行都错位。预乘也不做：宿主自己合成，Qt 自己的托盘
同样发直通 alpha。

**菜单走 `/NO_DBUSMENU`**。`Menu` 属性填 `/NO_DBUSMENU`，宿主于是调 `ContextMenu` 方法，我们把
`AdContextMenu`（换肤菜单，不是 `QMenu`）弹在指针处。代价写在明处：只认 D-Bus 菜单、完全不理
`ContextMenu` 的宿主会**没有右键菜单**。选这个是因为另一条路要放弃菜单皮肤、把每一项翻译成
`com.canonical.dbusmenu` 的 label/icon-name/toggle-state，那等于把设置菜单做两套。

**面板晚于应用启动是常态**，所以注册不是"构造时试一次"：先看
`org.kde.StatusNotifierWatcher` 在不在，不在就订阅 `NameOwnerChanged` 等它出现；出现后补注册。
注册参数用 `baseService()`（唯一名）而不是对象路径 —— Qt 与 GTK 的既有实现都发这个形态，宿主对它的
测试覆盖最多。

**验证（2026-10-20，Windows + Qt 5.15.2 / Qt 6.12.0，MSVC 14.44）**

| 检查 | 结果 |
|---|---|
| `snow-shot-linux-tray-tests`（手工 `cl` 编译运行，Qt **5.15.2**） | **passed**（34 条断言） |
| 同一份源与测试，Qt **6.12.0** | **passed** |
| 测试抓到的真问题 | `QImage(w,h,ARGB32)` 的未写像素是**分配器残留**，不是 0 —— 判据必须 `fill()` 后再断言，否则"逐行紧凑"这条会随机失败 |
| 判据修正 | `one</p><p>two` 是**两个**边界 ⇒ 两个换行（HTML 渲染本来也是空一行）。一开始按一个换行断言，是测试错了不是实现错了 |
| `moc` 解析头文件 | `MOC_EXIT=0`，生成的 `qt_metacall` 里出现 `qRegisterMetaType<QVector<…IconPixmap>>()`；moc 产物再过 `-Zs` 也 `EXIT=0` |
| `bash scripts/syntax-check-linux-cpp.sh` | `SYNTAX OK`（覆盖面 6 → **7** 个文件） |
| 接线 lambda 的语法 | 探针 TU（`-DQ_OS_LINUX`，不引 ant_design）过 `-Zs`，`EXIT=0` |
| `systemtraycontroller.cpp` 的 **Windows 分支** | 用 `compile_commands.json` 里的真实编译命令 + `-Zs` 跑，`EXIT=0`（0 error）—— 重构出 `showContextMenu()` / `applyToolTip()` 后原语义未变 |
| `windows-qt5-full` configure | `EXIT=0` |
| `windows-qt5-full` build `snow_shot_status_notifier_payload` | 5 步全绿（MOC + 编译 + 静态库） |
| 六个改动/新增文件 clang-format | 全部 0 违规 |

**本片未覆盖（诚实记账）**

- **D-Bus 线上签名没有任何离线验证**。这条不是懒，是**没有路**：`QDBusMetaType::typeToSignature`
  在 Qt 5.15 对**任何**用户类型都返回空串 —— 用一个最小案例（`struct Simple { int; QString; }` +
  `qDBusRegisterMetaType`）单独复核过，先初始化 `QDBusConnection` 也一样。Qt5 只在真正 marshal
  时推导签名，而 marshal 需要连接；本机没有会话总线。于是 `(iiay)` / `(sa(iiay)ss)` 是否被宿主接受，
  只能到真机用 `busctl --user introspect` / `gdbus introspect` 看。**我们与 Qt 自己的
  `qdbustrayicon.cpp` 用的是同一套写法**（`Q_PROPERTY` + `qDBusRegisterMetaType`），这是当前能拿到的
  最强保证。
- **真机一个都没跑**：`dde-dock` 是否显示、左/中/右键是否分别触发 `Activate`/`SecondaryActivate`/
  `ContextMenu`、图标尺寸是否被采纳（我们发 16/22/24/32/48/64）、tooltip 的标题/副标题排版。
- **`dde` 的右键菜单**：如果它只渲染 D-Bus 菜单而不调 `ContextMenu`，右键就是空的（见上文取舍）。
  这一条要优先验，验不出来就得补 `com.canonical.dbusmenu`。
- **watcher 重启后的状态**：已订阅 `NameOwnerChanged` 并会补注册，但"宿主重启时正在弹的菜单"、
  "重启后 `Status` 回到 `Active` 是否需要重发 `NewStatus`"未验。
- **`NeedsAttention` 未使用**：`showWarningMessage` 之类的告警没有让图标闪烁/高亮，仍是普通通知。
- **CTest 目标本身仍未进 CMake 配置验证**，原因同 slice 5/6：Qt5 路线开不了 `SNOW_APPS_BUILD_TESTS`，
  本机唯一的 Qt6 树是 smoke 档、不含 `snow_shot`。

#### 9.4.15 P4 slice 8：单实例端点搬进会话运行时目录（2026-10-20）

**背景**：单实例这一层本来就是纯 Qt（`QLocalServer` + `QLockFile`，`singleinstancecoordinator.cpp`
里一个平台条件编译都没有），看起来"直接可用"。问题不在能不能编，而在**端点落在哪**：Unix 域
socket 需要路径，而 `QLocalServer::listen()` 拿到**不带斜杠的名字**时，Qt 会自己在
`QDir::tempPath()` 下拼一个 → Linux 上就是 `/tmp/snow-shot-<hex>`。这一条的后果全都很晚才显形：

1. **`/tmp` 会被按龄清理**。`systemd-tmpfiles` 对 `/tmp` 的处理会删掉超过保留期的条目，**包括
   socket**。主程序是 `setQuitOnLastWindowClosed(false)` 的托盘常驻进程，跑满十天之后它手里的
   socket 文件被 unlink —— 进程还活着、还握着锁，但**新启动的实例连不上**，于是走到
   "owns the storage lock but could not be contacted" 分支、`return 2`。用户看到的是"应用再也
   打不开"，日志里没有任何与"托盘挂了两周"相关的线索。
2. **多用户机器上的抢占**。`/tmp` 是 1777，名字是 `homePath|org|app` 的 SHA-256 前 24 位
   ——可枚举。别人预先建同名 socket 后，我们的 `removeServer()` 删不掉（sticky 位），
   `listen()` 拿到 `EADDRINUSE`，应用同样起不来。这是对常驻托盘应用的 DoS。
3. **`PrivateTmp=`**（systemd 单元）或各种沙箱里，两次启动各自看到不同的 `/tmp`：锁在共享的
   `$XDG_RUNTIME_DIR`（所以第二个实例确认"已经有人占着"），socket 却在各自私有的 `/tmp`
   （所以谁也连不上）。症状与第 1 条一样。
4. **锁和 socket 分处两地**：锁本来就在 `$XDG_RUNTIME_DIR`，socket 在 `/tmp`。出问题时要看两个
   地方，而其中一个会被自动清掉。

**做法**：把两样东西搬进同一个目录，优先 `$XDG_RUNTIME_DIR`（per-user、0700、tmpfs，没有任何
清理器会动它），只有在它不可用时才退回 `$XDG_TMPDIR/snow-shot-<uid>`（自己建、自己 chmod 0700）。
纯逻辑模块 `snow_shot/app/singleinstanceendpoint.{h,cpp}`（全平台，不碰文件系统）：

| 决策 | 判据 |
|---|---|
| 目录优先级 | `runtimeLocation` 非空**且绝对** ＞ `tempLocation/snow-shot-<uid>`。相对路径按未设置处理 —— XDG 规范要求绝对路径，而 Qt 会把相对值当成"临时目录下的名字"，那是静默错位。 |
| 锁与 socket 同目录 | `lockFilePath = directory/snow-shot-<id>.lock`，`legacyEndpoint = directory/snow-shot-<id>`，`receiptEndpoint = legacy + "-v2"`。 |
| 路径长度 | `sun_path` 在 Linux 是 108 字节**含结尾 NUL**、BSD 系 104 ⇒ 取 103 作上限；`toUtf8().size()` 量**字节**（35 个汉字 = 105 字节，字符数完全看不出问题）。首选目录放不下就换另一个候选，两个都放不下才报错，且错误里点名的**是用户配置的那个目录**，不是他从未听说过的兜底目录。 |
| 已有目录能不能用 | 必须是**本人的目录**：`isAcceptableEndpointDirectory(exists, isDir, ownerId, userId)`。不存在 ⇒ 我们自己建；存在但不是目录、或属主不是自己 ⇒ **拒绝启动**并报出路径。让"从不采用无法证明属于自己的目录"成为显式规则，比"能用就用"安全：否则别人在 `/tmp` 里种的目录里那个 socket 随时可以被换掉。 |
| 谁 chmod | 只有我们自己新建的目录（即兜底路径）才 chmod 0700。`$XDG_RUNTIME_DIR` 由会话管理器建好且已是 0700，改别人的目录属无关变更。 |

`singleinstancecoordinator.cpp` 里只多了一个 `#elif`-式的 `#if defined(Q_OS_LINUX)` 分支和
`prepareEndpointDirectory()`（在创建任何文件**之前**跑），协议、receipt、legacy 降级、
`QLockFile` 的 stale 处理全部原样不动。

**两个实现细节值得记**：

- **取 uid 不用 `geteuid()`，用 `QFileInfo(QDir::homePath()).ownerId()`**。原因是这个文件的
  Linux 分支只有塞 `-DQ_OS_LINUX` 给 MSVC 才编得到，而 `<unistd.h>` 在 Windows 上不存在。
  家目录的属主按定义就是当前用户（root 的家目录属主是 0，正好正确）。若某天它返回 0 而用户
  不是 root，后果是**拒绝**共享临时目录而不是误用，方向是安全的。
- **判据用 `Q_OS_LINUX` 而不是 `Q_OS_UNIX`**。macOS 也是 Unix，但 macOS 现在是能跑的、且路径
  行为没在那边验证过；把 macOS 一起改会顺手给一个不可验证的平台换掉单实例端点。
  代价写在这里：macOS 侧仍有"`$TMPDIR` 被清掉"的同款风险，等 macOS 排进计划时按 9.4.15 的
  同一套规则处理。

**验证**：

- `tests/linux_single_instance_endpoint_tests.cpp`（纯逻辑，9 组 / 35 条断言）：优先级、
  尾斜杠归一化、per-user 兜底、相对路径被忽略、无可用目录时报错、**字节 vs 字符**的上限、
  首选过长自动降级、两个都过长时报错点名首选目录、属主规则四种组合。
  手工 `cl` 编译运行：**Qt 5.15.2 passed、Qt 6.12.0 passed**。
- **既有 700 余行单实例测试全套通过**（Windows 分支语义未变）：把它手工 moc + 编译链接后运行，
  `EXIT=0`，覆盖 legacy 协议降级、receipt 重试、stale 锁恢复、无响应 peer、子进程转发等场景。
- `scripts/syntax-check-linux-cpp.sh` 覆盖面 7 → **9** 个文件（新增 `singleinstancecoordinator.cpp`
  与 `singleinstanceendpoint.cpp`），`SYNTAX OK`；补了 `QtNetwork` 头目录。
- **门禁做了反向验证**：往 Linux 分支里注入一个成员名拼写错误，带 `-DQ_OS_LINUX` 报 1 个
  `error C`、去掉宏报 0 个 —— 证明这条门禁真的在编译那个分支，不是空跑。
- 用 `compile_commands.json` 里的真实编译参数对两个新旧文件做语法检查：`EXIT=0`、0 error。
- `windows-qt5-full` configure `EXIT=0`（CMake 新增目标与源清单都被解析）。
- 6 个改动/新增文件 clang-format 全 0 违规。

**本片未覆盖**：

- **真机未跑**：`$XDG_RUNTIME_DIR` 在 dde 会话里是否总有值（正常登录有；从 `systemd --user`
  早期单元或 `sudo` 里起可能没有）、socket 实际权限位、第二次启动的转发时延。
- **`systemd-tmpfiles` 的清理策略是按其发行版配置推断的**，没有在 UOS 上实测"十天后 socket
  是否真被删"。这条恰恰是本片的动机，建议真机验证时用 `touch -d` 改 socket 的 mtime 再跑
  `systemd-tmpfiles --clean` 观察。
- **`$XDG_RUNTIME_DIR` 本身不可写 / 属主异常** 的情况只做了代码路径，没有构造环境验。
- **P6 打包要配合**：如果 deb 的 systemd 单元设了 `PrivateTmp=`，本片之后不影响正确性
  （socket 已不在 `/tmp`），但 `$XDG_RUNTIME_DIR` 必须在单元里可见（`--user` 单元天然有）。
- **`QLockFile` 的跨主机语义没变**：锁文件里记 hostname，NFS 家目录上两个主机共享
  `$XDG_RUNTIME_DIR` 是不可能的（per-machine tmpfs），所以这条在 Linux 上反而比 Windows 更安全。

#### 9.4.16 P4 slice 9：`screenshotnative.h` 拆成三个职责单一的头（2026-10-20）

**问题不是"文件太大"，是它同时是三件不相干的事**，而且第 3 件把 Windows 头拖进了平台中立的
include 路径：

| 原 `screenshotnative.h` 里的东西 | 真实性质 | 谁实现 |
|---|---|---|
| 11 个 `configure*Window` / `setScreenshotInputTransparent` / `stackScreenshotWindowBelow` | 我们自己窗口的**平台适配钩子** | 只有 macOS（`screenshotplatform.mm`） |
| `screenshotDisplayAtCursor` / `screenshotFocusedWindow` / `screenshotFocusedWindowBounds` | **"用户在哪个窗/哪块屏"查询** | 只有 macOS；Windows 的调用点直接用 Win32 |
| `ScrollInputResult` + `screenshotScrollPermission` + `sendScreenshotScroll` | **合成滚轮事件**，真正平台中立 | macOS 原生 / Windows `SendInput` / 其余"没有" |

第 3 组尤其坏：`screenshotnative.h` 第 3 行就是
`#include "snow_shot/platform/windows/scrollinput.h"` —— 一个平台中立的头**无条件**依赖
Windows 头，非 Windows 平台把它 include 进来连编译都过不去；而它的 `#else` 分支把
`sendScreenshotScroll` 直接转发到 `windows::sendScrollingWheelStep`（`SendInput`），
就算编过去了，Linux 上也是一条永远失败的路。**这正是 P4 里"非 Windows 平台滚轮捕获根本
不存在"的隐藏形态：不是没实现，是借用了一个不存在的实现。**

**做法**：

- `snow_shot/platform/screenshotwindow.h` —— 11 个窗口钩子。**全平台声明**，非 macOS 是
  `inline` 空实现。空实现是"这个平台不需要额外动作"的正确表达（Qt 的窗口 flag 已经把意图
  说清楚了），它让共享代码以后可以无条件调用，而不必在十个调用点各写一遍 `#ifdef`。
  **本片没有改动调用点的 `#ifdef Q_OS_MACOS`**：那些块里往往还有 `platform::macos::*`
  的原生逻辑，摘 guard 需要 X11 侧先有实现可验，否则只是把"没做"换成"调了个空函数"。
- `snow_shot/platform/screenshotfocus.h` —— 三个查询，**仍然只在 `Q_OS_MACOS` 下声明**。
  这不是偷懒：Windows 的调用点走 `GetForegroundWindow()`/`MonitorFromPoint()`，给一个中立
  声明会变成第二套没人用的问法；Linux 的机制其实已经有了（`snow-linux` 里给全屏检测写的那套
  `parent==root` 窗口树爬升 + `translate_coordinates`），但**没有 C ABI 暴露**。宁可保持
  macOS-only，也不放一个返回"没有窗口"的桩 —— 那会被读成"确实没有聚焦窗口"。
- `snow_shot/platform/screenshotscroll.h` + `src/platform/screenshotscroll.cpp` —— 结果类型
  搬到这里（`windows/scrollinput.h` 改成 `using ScrollInputResult = platform::ScrollInputResult`，
  只保留发送者），实现按平台分派：Windows 转发 `sendScrollingWheelStep`；**Linux 返回
  `Unsupported` 且 `screenshotScrollPermission()` 返回 false**，注释写明原因是
  snow-linux 只**观测**滚轮、没有注入实现。返回 false 而不是 true，是为了让自动滚动第一时间
  自我关闭，而不是拿着一串永远不生效的 `Unsupported` 空转。
- macOS 实现文件 `screenshotnative.mm` → `screenshotplatform.mm`（`git mv`，一处 CMake 引用），
  一个头被拆成三个之后，文件名再叫 native 只会让下一个人去 grep 一个不存在的头。
- 24 个 include 点（14 个 `src/*.cpp`、1 个 `.mm`、9 个 `tests/`）逐个改成它真正需要的那一个头。
  `.mm` 一律给三份：Objective-C++ 只在本机之外的 macOS 上编译，这里无法验证，宁可多 include。

**验证**：

- `windows-qt5-full` configure `EXIT=0`；用 `compile_commands.json` 的真实参数对 4 个受影响
  TU（`screenshotscroll.cpp`、`scrollinput.cpp`、`screenshotscrollingcapturecontroller.cpp`、
  `screenshotcontroller.cpp`）做 `-Zs`：全部 0 error。
- **非 macOS 侧的 11 个空实现单独写了探针 TU**：Windows 构建里没有任何文件能在
  `Q_OS_MACOS` 未定义时 include 到那段（所有调用点都还在 guard 里），不专门验一遍等于
  把一段只写不编的代码合进去。探针 `-Zs`：0 error、0 warning。
- **Linux 分支进语法门禁**：`src/platform/screenshotscroll.cpp` 加入清单（9 → **10** 文件），
  `SYNTAX OK`。
- 全仓 `grep screenshotnative.h` 只剩注释里一句历史说明。

**本片未覆盖 / 留给后面**：

- **Linux 的窗口级捕获目标当时拿不到**（`request.window` / `monitorName` 为空 ⇒ 退回整屏）。
  已在 **9.4.17** 补上：`snow-linux` 暴露 focused-window 的 id+frame C ABI、`screenshotfocus.h`
  加 Linux 声明、`directcapturecontroller.cpp` 的 `#elif` 加分支。
- **X11 上窗口钩子的真实需求未验**：置顶、输入穿透（XShape）、层叠顺序在 dde 合成器下
  是否需要原生干预，只能在真机上看。现在调的是空实现，行为等同 Qt 默认。
- **macOS 侧只做了 include 改写，没有 macOS 主机可编译**；改动是纯 include + 类型别名，
  风险面是"某个 `.mm`/测试少 include 了一个头"，因此 `.mm` 全部给三份。
- **滚轮注入（Linux）** 仍是 0，返回 `Unsupported` 时用户看到的是
  `scroll_dispatch_failed` / 自动滚动自我关闭；`permission_required` 这个文案是
  Windows/mac 语境，Linux 上语义不准确，等真实现再改文案。

#### 9.4.17 P4 续：Linux 窗口级捕获目标（2026-10-20）

P4 收尾时留下的第一条待办。症状是"截图→当前窗口"和"截图→当前显示器"在 X11 上什么也拿不到：
`DirectCaptureRequest::window` 和 `monitorName` 都是空，`captureDirectTarget` 在入口就返回空帧。

**决定 1：X11 没有窗口级采集，接受"从整屏裁"作为语义。**

`snow-capture` 的 Linux 后端拒绝 `CaptureTarget::Window`（见
`snow-capture/src/platform/linux.rs` 顶部注释），理由成立：读别的窗口自己的 drawable 需要
Composite redirect，否则服务器在窗口被遮挡时读到的是上次留下的帧缓冲内容。所以窗口截图的
实现是**在根窗口采集结果上按窗口 frame 裁剪**，这也是 scrot 等工具的做法。代价写在文档里：
被遮挡的窗口截出来就是被遮挡的样子。这条不能靠"更好的实现"绕过，要改得上 XComposite，
那是另一片工作量。

**决定 2：几何规则全放 Rust 纯逻辑，C++ 只搬字节。**

`windowcapture.rs`（新增，全平台编译）有两个函数：
`capture_target(id, &FocusedWindow)` 决定"这个聚焦窗口能不能单独截"，
`crop_plan(frame, displays)` 决定"哪块屏的哪个矩形，贴到输出的哪个位置"。
后者是本片真正有价值的部分：**窗口跨两块屏时，只从一块屏取会静默返回半个窗口**，
而这是双屏用户拖窗口时最常见的状态。10 组单测在 Windows 上跑，含负坐标屏、上下排列、
半开区间边界、退化屏、饱和坐标。

"能不能单独截"这个判据**没有新写一份**，而是把 9.4.10 里"能不能算全屏应用"的那条抽到
`window::is_paintable`，fullscreen 与 windowcapture 同时调用它 —— 两个问题拒绝的是同一批
表面（桌面背板、面板、菜单、隐藏窗口），分成两份实现早晚会给出不一致的答案，症状是
"截出来是桌面，标题却是窗口名"。

**决定 3：一个查询吐三个答案。**

`screenshotFocusedWindowTarget(quint32* id, QRect* frame, QRect* logicalBounds)`：
id 用来做请求身份与历史记录，frame 是根窗口像素（裁剪用），logicalBounds 是 Qt 逻辑坐标
（浮层与历史画布用）。合成一次查询而不是三个函数，因为三次读可能描述三个不同的窗口。
`frame → logicalBounds` 的换算是**按边除**（不是按尺寸乘），与 `fullscreen::logical_to_native`
的方向相反、规则相同；跨屏窗口没有唯一比例，此时 logicalBounds 返回空，调用方回落物理矩形
（历史画布本来就这么兜底）。

顺带把 `onX11()` / `openPlatform()` 两个 12 行助手抽进 `src/platform/linux/x11platform_p.h`
（inline，无新增 TU），`focusedfullscreenwindow.cpp` 与新文件共用。

**决定 4：`CurrentMonitor` 用 Qt 的屏幕名，不引入第二套坐标。**

Win32 是 `MonitorFromPoint`，macOS 是 `display:%1`。Linux 上 `QScreen::name()` 在 xcb 下就是
RandR output 名，也正是 `snow-capture` 报告给 Qt 的 monitor 名，所以直接把指针所在屏的名字
填进 `monitorName` 就够了。名字对不上时不再静默返回空图，而是给出
`No captured display matched the current monitor` —— 这条在真机上若出现，就说明两侧命名真的
分叉了，比"截图无声失败"好查得多。

**一个反向的坑**：`captureDirectTarget` 里 `nativeRequest.focused_window` 在 Linux 上**必须**
清零。那个字段在 native 层的唯一用途是 `WindowId::from_windows_handle`，也就是 HWND；把
X11 的 window id 递下去会走进 Win32 采集路径拿一个毫无意义的数字。因此 Linux 分支改为请求
整屏 + 自己裁。

**验证**：

- `cargo test -p snow-linux`：**174 passed**（新增 `windowcapture` 10 组 + `window::is_paintable`
  1 组）。
- `linux-cross-check.sh` 对 `snow-linux` / `snow-linux-c` 双架构（x86_64、aarch64）**PASS**。
  *注：不带 filter 的全仓扫会在某个 crate 上触发本机 Program Blacklist（`reg.exe`），
  与本次改动无关，已单独记在环境约束里。*
- `scripts/syntax-check-linux-cpp.sh` 覆盖面 10 → **12** 个文件（新增 `screenshotfocus.cpp`
  与 `directcapturenative.cpp`），补了 `snow_image` / `snow_memory` / `snow-capture-c` /
  `snow-recording-c` 头目录。
- 4 个改动 TU 用 `compile_commands.json` 的真实参数复查（新脚本
  `.workbuddy/qt5-port/check-tu.sh`）：`directcapturenative.cpp`、`directcapturecontroller.cpp`、
  `applicationcontroller.cpp`、`screenshotcontroller.cpp` 全部 **PASS**。
- 新脚本 `replay-tu.py` + `check-tu.sh`：从 `compile_commands.json` 取真实命令行、去掉
  `-o` 换成 `/Zs` 再跑。比手抄命令可靠，因为 include 路径与 feature define 会随时间漂移。

**本片未覆盖**：

- **真机未跑**：`_NET_ACTIVE_WINDOW` 在 dde-kwin 下是否始终维护（否则走输入焦点回退，
  而回退拿到的是**客户端**窗口，frame 会少一圈标题栏）、窗口 frame 含不含 DDE 的阴影边距、
  裁剪路径在 SHM 采集下的耗时。
- **`QScreen::name()` 与 RandR output 名是否真的一致**只在 Qt 源码层面推断过。不一致的表现
  是 `No captured display matched the current monitor`，一条日志就能判定。
- **跨屏窗口**的 history 画布位置：logicalBounds 为空 ⇒ 回落物理矩形，1x 缩放下正确，
  带缩放时会偏，等真机确认。
- **被遮挡的窗口**截出来是被遮挡的样子（决定 1 的直接后果），这不是 bug，但如果用户预期是
  "干净的单窗口截图"，只能靠 XComposite 再开一片。
- **Wayland** 全部不适用：`onX11()` 为假 ⇒ 两张表都返回 false ⇒ 行为回到"截不到"。

#### 9.4.18 P5 slice 1：NEON 像素转换内核（2026-10-21）

P5 的第一项（§7.2 表里唯一的 P0）是 `snow-capture/src/convert/simd_x86.rs` 的 NEON 对应物。
结论先说：**这不是 2000 行的移植，是 200 行**。原因是 x86 那六个 BGRA 内核的语义可以一句话
说完——"对每个 16 字节块查一张 16 项字节表，需要时再把 alpha 位置 OR 成 0xFF"——而 AArch64
的 `TBL`（`vqtbl1q_u8`）与 `pshufb` 是同一个操作、同一个越界约定（索引 ≥ 16 读作 0，而表里
所有索引都在 `0..=15`）。所以 `simd_neon.rs` 与 `ssse3_bgra_core` 结构完全一致：4 块主循环、
4 像素次级循环、标量尾巴。

**决定 1：表只有一份，两个架构都读它。** `scalar::BGRA_TO_RGBA_SHUFFLE` 是唯一真值来源，
NEON 与 SSSE3 都用 `vld1q_u8` / `_mm_loadu_si128` 加载它，**不是各自抄一份**。理由很实际：
本机不能跑 AArch64，如果两边各写一份，颜色映射错了只会在一台机器上复现，排查成本是一天。

**决定 2：AArch64 没有 NT store，`_nt_*` 直接别名普通内核。** `STNP` 只支持寄存器对
（16 字节），且 Rust 没有对应 intrinsic；盲写没跑过的内联汇编比别名更危险。所以
`convert_bgra_to_rgba_neon_nt_*` 三个入口都存在、都调普通内核，**写在这里而不是静默降级**，
dispatch 表在两边形状一致，调用方不需要长 `cfg` 分支。

**决定 3：没有"NEON 之下的档位"。** ARMv8-A 强制 Advanced SIMD，所以 x86 的四档
（AVX-512/AVX2/SSSE3/标量）在 AArch64 上只有一档。`neon_available()` 保留只是为了 dispatch
读起来与 x86 一致，它恒为真。

**决定 4：f16/HDR 的 NEON 不做，因为 Linux 没有消费者。** f16→sRGB 那条路是给 DXGI HDR
回读用的；X11 采集只有 8 位。等将来有 Wayland/HDR 采集再谈，现在写就是没有验收标准的代码。

**验证链（本机不能执行 AArch64，所以链条要写清楚）**：

1. `cargo check --target aarch64-unknown-linux-gnu`（默认 + `--all-features`）**PASS**；
   `cargo build --target aarch64-unknown-linux-gnu --lib`（真做 LLVM 代码生成，53s）**PASS**
   —— 这一条比 `check` 强得多，intrinsic 的类型/约束错误在这一步才会暴露。
2. **读汇编**：两个 const 实例（`neon_bgra_core::<false>` / `<true>`）里各有 **6 条**
   `tbl v?.16b, { v?.16b }, v?.16b`（4 块主循环 + 4 像素循环，与设计一致）；
   `orr v?.4s, #255, lsl #24` **只出现在 opaque 实例**（另一个是 0 条）——说明
   `FORCE_OPAQUE_ALPHA` 的常量折叠没有把掩码丢掉。
3. **表字节在 aarch64 rlib 里**：按 `02 01 00 03 06 05 04 07 0a 09 08 0b 0e 0d 0c 0f`
   二进制搜，命中。
4. 新增 3 个本机测试：表语义 == 逐像素 `swap_bgra_to_rgba`；**ssse3 内核 == 标量**
   （1..=70 像素，覆盖两条循环与尾巴）；**被 dispatch 选中的内核 == 标量**（同样 1..=70）。
   最后一条就是 AArch64 上 NEON 的验收测试，一到真机自动生效。
5. **反向验证**：把表里第 0 个像素改成恒等映射 ⇒ 上面第 1、2 条测试立刻失败。
   记一个反直觉现象：`selected_bgra_kernel_matches_scalar` 在**本机仍然通过**，因为本机的
   选中内核是 AVX-512，它不读这张常量表——这正是为什么要单独写一条直接打 ssse3 的测试。

**顺带修掉一个真 bug**：`--all-features`（`stage-timing`）在非 Windows 上根本编译不过。
先是 3 个错（`qpc_duration_between` / `qpc_ticks_to_duration` 只有 `#[cfg(windows)]` 定义
却被无条件 re-export、`OnceLock` 未使用），修完又露出 6 个 dead-code 错（`StageScope` /
`attach_stage_timings` / `stage_recording` / `pixel_counters::readback` 在 Linux 上没有调用者）。
处置：QPC 两个 helper 的 re-export 加 `cfg(windows)`；记录器 API 用**作用域明确的**
`#[cfg_attr(not(windows), allow(dead_code))]` + 注释说明这部分 recorder API 只有 Windows
调用者，而不是删代码（删了会打断全部 Windows 调用方）。**后续修正**：当时注释写的
"X11 后端只记 checkpoint，不开 scope"确实是 X11 后端的真实状态，但那是缺口而不是设计——
9.4.24 让 X11 也开 scope 后，只剩 `stage_mark` / QPC helper / `stage_recording` 仍为
Windows 专有。P7 的 CI 一定会跑 `--all-features`，
这条不通就是红。

**CMake 侧不改，理由写进 `SnowSimd.cmake` 顶部**：AArch64 上 NEON 是基线，不需要
`-march=armv8-a+simd`（加上反而把基线钉回 v8.0，丢掉 v8.2 的 dot-product/FP16）；
真正需要开关的地方在 Rust 的 `#[target_feature(enable = "neon")]`。现存 C++ `*_avx2.cpp`
每个 TU 都被 `#if defined(__x86_64__) || ...` 包住，aarch64 上编译成 `return false` 桩，
调用方回落标量。

**本片未覆盖**：

- **NEON 从未在 ARM 上执行**：上面链条里最弱的一环就是"`TBL` 语义按 ARM ARM 推断"。
  到真机的第一条命令是 `cargo test -p snow-capture convert::` 与 `--all-features` 各跑一遍。
- **没有性能数字**：`snow-capture` 的 `conversion_hot_path` bench 需要真机；标量 vs NEON 的
  差距在 X11 8 位路径上可能被 SHM 采集掩盖。
- **C++ 侧没有 NEON 内核**：`snow_image/src/raster.cpp` 的硬件 CRC32C 仍只认 `_M_X64`
  （AArch64 有 CRC32 指令，属 §7.2 的 P1）、`snow_canvas_filter_*_avx2.cpp` 在 aarch64 上
  整体退化标量（属 P1/P2）。
- **f16/HDR 全链**（见决定 4）。

**后续修正（2026-10-30，合入上游 92 个提交之后）**：`simd_neon.rs` **已被删除**，本片的内核
由上游的 `simd_arm.rs`（上游 `392a75a9 perf(arm64): accelerate image and recording pipelines
with SIMD`）取代。合并时 `select_bgra_kernel` 一族函数被上游改成了两只互斥的
`#[cfg(target_arch = "aarch64")]` / `#[cfg(not(target_arch = "aarch64"))]`，aarch64 那只直接
返回 `simd_arm::convert_bgra`，于是我们这六个 `simd_neon` 入口再没有任何调用者；而它们原先是
写在**另一只** `not(aarch64)` 函数体里的，那段 `#[cfg(target_arch = "aarch64")]` 块在 aarch64 上
永不编译——合并把它变成了纯粹的孤儿。CI 的 arm64 job 因此报 9 条 `-D dead-code`
（`neon_available` / `convert_bgra_to_rgba_neon_*` / `neon_bgra_core` /
`BGRA_TO_RGBA_SHUFFLE`）。

处置：删 `simd_neon.rs`、删 `mod simd_neon;`、删 6 处死分派；`BGRA_TO_RGBA_SHUFFLE` 收窄为
`#[cfg(any(target_arch = "x86_64", test))]`——aarch64 走 `simd_arm`，它用显式车道运算取
RGB，根本不读这张表，只有 SSSE3 与测试用的逐字节参考实现读。上游那份覆盖 BGRA **加** f16/HDR，
比本片更全，所以是取代而不是并存。

验证：本机 `cargo check` 交叉检查 aarch64（dev + `--profile release`）与 x86_64 三个组合全绿。

#### 9.4.19 P3 slice 7：X11 Damage 脏矩形（2026-10-21）

9.4.7 把 X11 采集做到零拷贝（MIT-SHM），但那解决的是"每帧要拷多少"，没解决"这一帧要不要采"。
本片之前 `is_duplicate` 恒为 false、`dirty_rects` 恒为空 ⇒ 录制时桌面完全静止，后端照样每帧
`ShmGetImage` 一次、编码器照样全帧重编。Damage 扩展就是这两件事的答案：根窗口告诉客户端
"自上次以来哪些区域被画过"。

**决定 1：只有合成器在跑时才信这个答案。** 没有合成器时，应用窗口直接画在屏幕上，根窗口的
Damage 只覆盖它自己的壁纸——此时"没有变化"的答案会让录制**冻结**，比慢一点糟得多。判据是
`_NET_WM_CM_S<n>` 这个 selection 有没有 owner（该 selection 的存在意义就是"每屏只能有一个
合成器"）。检查不通过就 `Ok(None)`，后端行为与 9.4.7 完全一致。这是**设计取舍，不是补丁**：
宁可在无合成器的老会话上退化成"每帧都采"，也不要在那种会话上跳帧。

**决定 2：两条捷径都有保质期。** Damage 漏报的途径不止一条（合成器直接把画面翻到硬件
overlay、驱动丢报、合成器中途退出），而漏报的表现是画面卡住不动。所以：

| 保证 | 默认 | 触发后 |
|---|---|---|
| 心跳：最久多久必须真读一次服务器 | 500 ms | 即使"没有变化"也采一帧 |
| 全帧刷新：最久多久必须给出一帧无脏矩形的完整帧 | 2 s | 即使知道改哪儿也当全帧发 |

两者的作用不同：心跳防"画面冻住"，全帧刷新防"部分编码的参考帧一路错下去"（编码器只有在
手里有完整参考帧时才能安全地只重编局部）。它们都是 `Duration` 参数，不写死。

**决定 3：合并只许变大。** `localize` 的顺序是"裁到目标 → 平移到目标本地坐标 → 合并相接的
矩形 → 超过 16 块就标 truncated"。合并用的是**包围盒**，会连带重编两块之间没变的像素——这是
故意的：少报一块变了的地方，屏幕上就永久留下错误；多报一块，只是多编几个块。同理
truncated 的答案一律当全帧，因为"精确到可用的短列表"这时根本不存在。

**决定 4：跳帧要调用方担保缓冲区的来历。** 只有 `capture_with_history_hint(.., true)` 并且
`last_extent.is_some()`（已经交付过至少一帧自己的输出）时才可能返回 `is_duplicate`。普通的
`capture()` 永不跳帧，并且会把累积的 Damage 丢掉——否则下一次带提示的调用会拿到"自创建以来
的全部变化"这种无界积压。

**决定 5：跳过的帧要睡。** 没有变化时服务器是**立即**回答的，不休的话采集循环会以几千次/
秒的频率做 `DamageSubtract` 往返。暂停时长由 `set_target_fps` 给的目标帧率算出（未设置时
16 ms ≈ 62 Hz，钳在 2–33 ms）。这也是为什么 `set_target_fps` 在这一片才被实现：真采集的
节奏由会话决定，只有跳帧需要后端自己撑住时间。

**决定 6：协议调用失败就丢掉 tracker，不重试。** 能失败的原因（合成器重启、damage 对象被
销毁）都不是"下一帧会好"的类型；每帧重试等于每帧白付一次往返。丢掉之后行为回到"每帧都读"，
并且**不会**在会话中途重建——重建的成本是一串握手，收益是不可预期的。

**验证链（本机没有 X server，所以要说清楚证明了什么、没证明什么）**：

| 检查 | 结果 |
|---|---|
| `cargo test -p snow-linux --lib` | **192 通过**（9.4.18 后是 174，本片 +18 全在 `damage`） |
| `cargo check -p {snow-linux,snow-capture} --target {x86_64,aarch64}-unknown-linux-gnu` | 双架构全绿 |
| `cargo check -p snow-capture --lib --all-features` | 绿（P5 修过的那条不回归） |
| `cargo fmt --check` + `cargo clippy -D warnings`（host 与 linux 目标各一遍） | 绿 |
| 反向验证 4 点 | 心跳判据取反 ⇒ 2 failed；全帧刷新判据改成永不成立 ⇒ 1 failed；`localize` 去掉裁剪 ⇒ 2 failed；`touches` 恒 false ⇒ 3 failed |

反向验证本身有个坑值得记：**`-D warnings` 会让"删掉一行"这种变异变成编译错误**（删了心跳
分支后 `since_capture` 变成未使用变量），于是测试根本没跑、输出为空，看起来像"注入没被执行"。
能编译的变异（取反比较符、把条件改成永假、让 `touches` 返回一个恒假但仍读参数的表达式）才是
有效的注入方式。

**本片未覆盖**：

- **从未在真 X server 上跑过**。上面证明的是"几何与决策正确、双架构类型正确"，`DamageSubtract`
  ↔ `FetchRegion` 的往返、`_NET_WM_CM_S0` 在 dde-kwin 下是否真被持有、有无合成器时根窗口
  Damage 的实际覆盖面，全部要真机。到真机的判断方法：开一次录制，比较"静止桌面"与"拖窗口"
  两种情况的 CPU 占用——静止时明显更低说明跳帧真的生效；接着让一个播放器全屏放视频，确认画面
  不卡（心跳兜住了漏报）；最后 `kill` 掉合成器再录一次，画面必须照旧跟手（此时 `compositor_running`
  的那次检查已经发生过了，兜底的是 500 ms 心跳）。**没有计数器能直接看"这一帧是跳过的还是采的"**
  —— 这一片的可观测性只有间接证据，加计数器属下一片。
- ~~**多屏区域路径会丢掉脏矩形**~~：`capture_session` 的区域内回退路径把 `dirty_rects` 硬编码成
  空，所以部分编码只在"单个显示器整屏"这条路径上生效。**已在 9.4.21 修掉**（裁剪 + 平移，
  目标偏移仍由 `append_exact_sample_damage` 加一次）。
- **不用 Damage 事件唤醒**：现在是"每帧同步 subtract + 取区域"，靠 16 ms 轮询。改成阻塞等
  `DamageNotify` 能把静止时 CPU 再压一个量级，但阻塞会挡住取消（会话要能随时停下），属于
  单独一片。
- **光标移动不是 root damage**：X 把光标叠在根窗口**之上**而不是画进去，所以纯鼠标移动的一帧
  会被判成重复。对当前架构是对的（光标由 `snow-cursor` 单独合成），但如果将来把光标画进帧里，
  这个判据必须一起改。
- 真机性能数字（跳帧省下的 CPU、部分编码省下的码率）。

#### 9.4.20 P3 slice 8：Wayland portal 会话协商（2026-10-21）

Wayland 不给任何客户端屏幕像素。唯一受支持的路径是经 `xdg-desktop-portal` 的 `ScreenCast`
接口向合成器申请一条 PipeWire 流，而"申请"本身是一串三次调用、异步应答、带版本特性的握手。
这一片做的是这串握手，**不做**帧消费。

**先说这一片修掉的一个真实错误行为。** 此前 `snow-capture` 的 Linux 后端无条件连 X11：在
Wayland 会话里 XWayland 会导出 `DISPLAY`，于是 `X11Session::connect()` 成功、`GetImage` 返回
真实像素——只不过那是 XWayland 根窗口的像素，只包含 X 客户端。截图"成功"了，内容是一张几乎
空白的桌面，而且**下游没有任何地方能把它和真空白区分开**。所以本片在 `X11Capturer::new` 里
加了一道判据：不是 `CapturePlan::X11Root` 就返回 `BackendUnavailable` 并带上具体原因。宁可
明确失败，也不要交付一张看起来正常的错图。

**决定 1：把"判断"和"管道"分开，管道先不写。** `portal` 模块里纯逻辑部分（路径构造、版本门控、
应答判定、流列表解析、握手顺序）在 Windows 上跑单测；真正的 D-Bus 调用在 `portal::dbus`，
只有 `cfg(target_os = "linux")` 才编译。中间的缝是一个 `PortalTransport` trait——正因为有它，
"三次调用的顺序""版本门控""取消发生在哪一步"这些真会出错的判断都能用假 transport 测，而不用
先让 portal 跑起来。这与 9.4.7 的 MIT-SHM、9.4.19 的 Damage 是同一套切法。

**决定 2：Wayland 会话永远不返回 `X11Root`。** 这是本片唯一的硬不变量，也是唯一写成
"无论 portal 探测结果如何"的判据（探测到 0/1/6 或干脆没有，都不得回落）。XWayland 让
`DISPLAY` 看起来可用，这是本片存在的全部理由。

**决定 3：版本不够就报错，不静默丢选项。** portal 对不认识的选项是忽略而不是拒绝，所以
"v1 上请求 `cursor_mode`"会握手成功、行为却不是调用方要的；`restore_token` 更糟——它被丢掉时
用户每次都要重新授权，而日志里一个字都没有。`select_sources_options` 因此返回
`UnsupportedOption { option, found, needed }` 而不是少塞一个键。版本锚点（`cursor_mode` v2、
`restore_token`/`persist_mode` v4、流属性 `source_type` v3、`pipewire-serial` v6）取自上游
接口文档 v6。

**决定 4：先订阅再调用，且允许 portal 换路径。** 应答不是方法返回值，而是 `Request` 对象上的
`Response` 信号。路径可由 `sender + handle_token` 预先算出（`request_path`：去掉 unique name
的前导 `:`、`.` 换 `_`），所以订阅能发生在调用之前——否则快到没有对话框的 `CreateSession`
会赢下这场竞争。但 token 只是"请求"：方法返回的对象路径才是权威，两者不一致时按返回值重新
订阅，而不是判失败（部分旧版 portal 会自己挑路径）。

**决定 5：取消不是错误。** `Response` code 1 是"用户关掉了对话框"，把它当失败上报会诱导上层
重试，而重试只会再弹一次对话框。code 0/1 之外的码保留原始数字（`Denied { code }`），不折叠成
"失败"——portal 将来新增码时那个数字本身是信息。

**决定 6：请求 `cursor_mode = Embedded`。** Wayland 下客户端拿不到权威指针位置（`XQueryPointer`
读的是 XWayland 自己那份副本），所以"隐藏光标、我们画"这条路不存在。Embedded 让合成器把指针
画进帧里，恰好一次。等 `cursor_mode = Metadata`（位置随流元数据下发）落地，才轮到录制器自己
渲染高亮——那需要消费 PipeWire metadata，属后续。

**决定 7：自己写超时组合器。** zbus 6 的超时是 crate 内部 API，且 6.0 起**移除了 blocking
API**（`zvariant` 也被并入 `zbus::wire`，`Value` 改名 `Variant`），所以同步接送必须
`zbus::block_on` 驱动 future。`Response` 可能永远不来（portal 中途崩掉），而调用方是同步的
`capture()`、没东西可取消，因此在 future 内部加边界：`portal::deadline::timeout_after`，
一个 50 行的 waker+线程组合器，不依赖具体 runtime，也能在 Windows 上单测。默认 120 s——那是
一个人在看对话框，不是网络超时。

**另一个协议陷阱**：`CreateSession` 结果里的 `session_handle` 声明是 `o`，实际是 `s`（上游
承认的历史实现错误），所以两种都接受，并且要求它真的以 `/` 开头。

**验证链（本机没有 D-Bus，所以要说清楚证明了什么、没证明什么）**：

| 检查 | 结果 |
|---|---|
| `cargo test -p snow-linux --lib` | 192 → **219 通过**（本片 +27：`portal` 23 + `portal::deadline` 4） |
| `cargo check -p {snow-linux,snow-capture} --target {x86_64,aarch64}-unknown-linux-gnu` | 双架构全绿（`portal::dbus` 首次进编译） |
| `cargo check -p {snow-linux,snow-capture} --all-features` | 绿 |
| `cargo fmt --check` + `cargo clippy -D warnings`（host 与 linux 目标各一遍） | 绿 |
| 反向验证 5 点 | sender 改写符号 `.`→`-` ⇒ 1 failed；Wayland 允许回落 `X11Root` ⇒ 2 failed；版本门控放松一档 ⇒ 2 failed；取消当成功 ⇒ 1 failed；deadline 恒 Ready ⇒ 2 failed |

反向验证的两点都改成"能编译的变异"：`sender.replace('.', "_")` 改字符而不删行，`capture_plan`
里**加**一条 `Wayland if portal_version.is_none() => X11Root` 的守卫（它同时证明那条不变量测试
是真的在测不变量，而不是恰好没覆盖到），`deadline` 的 `if state.fired` 改 `if true`。

**本片未覆盖**：

- **`portal::dbus` 从未执行过**。本机没有 session bus、没有 portal，证明的只是"双架构类型正确"。
  到真机的第一条命令：`WAYLAND_DISPLAY=wayland-0 XDG_SESSION_TYPE=wayland <binary> --probe`，
  先看非交互探测能不能拿到版本号；再看 `negotiate_screencast` 能否弹出授权框并拿到 `streams`。
  最可能的第一处失败是 `a{sv}` 的序列化形态（`HashMap<String, Variant>` 是否真按 `a{sv}` 落线），
  这也是它被单独放在一层的原因：改这一层不牵动任何已测逻辑。
- **帧消费不存在**：`negotiate` 返回的 `ScreenCastStream`（node id / size / position）目前没有
  消费者，`OpenPipeWireRemote` 也没调用。所以 `capture_plan` 对"Wayland + 有 portal"返回的是
  `PortalCaptureUnimplemented`，而不是"可用"。这条分支是这一片最诚实的一行代码。
- **`restore_token` 无处持久化**：拿到了但没有存储位置（配置 schema 属 C++ 侧），所以每次仍会
  弹框。`persist_mode` 同理没被请求。
- **多流未处理**：`multiple = false`，但仍接受多流应答（不硬性要求 1 条）。真的出现多流时
  调用方要自己决定是拼接还是取第一条，本片不替它决定。
- **会话生命周期绑在连接上**：`PortalSession` 拿着 `BusTransport`，drop 即断开、portal 随即
  关闭会话。这是有意的，但也意味着帧消费必须把 `PortalSession` 活着带到不需要为止。
- **探测有代价**：Wayland 下每次创建 capturer 都会走一次属性读取（3 s 上限）。X11 会话完全不
  碰 portal——`session_plan` 里那条 `if session.is_wayland()` 就是为此。

#### 9.4.21 P3 slice 9：区域内回退路径保留脏矩形（2026-10-21）

9.4.19 留下的第一条明确缺口：**多屏区域采集的脏矩形被丢掉**。`capture_session` 的区域内回退
路径（`capture_region_into` 返回 `None` 时走"整屏采集 + CPU 贴图"）把 `dirty_rects` 硬编码成
空列表。后果是整条链上"部分编码"只在**单个显示器整屏**这一种情形生效：多屏区域录制每帧都
报"变了，但不知道变了哪儿"，编码器退化成全帧刷新。而 X11 后端根本没有实现
`capture_region_into`，所以**Linux 的多屏区域录制 100% 走这条回退路径** —— 这个缺口恰好落在
最常用的场景上。

**修法不是"把监视器的脏矩形直接抄过来"，而是先裁后移。** `CaptureBlitRegion` 是 1:1 的裁剪
加放置（`src_*` 取源窗口，`dst_*` 放到输出位置，不缩放），所以每个监视器脏矩形要：裁到
`[src_x, src_x+width) × [src_y, src_y+height)`，再减去 `src_x/src_y` 变成"相对被拷贝窗口"的
坐标。落在窗口外的矩形**丢弃**——它描述的是这一帧输出里根本不存在的像素，报上去会让编码器
去刷新从未变过的区域。

**目标偏移只许加一次，这是本片最容易错的地方。** `append_exact_sample_damage` 在聚合时已经给
每个矩形加过 `blit.dst_x/dst_y`（对加速路径和回退路径一视同仁，因为后端返回的脏矩形一律是
**目标本地**坐标）。如果裁剪函数自己也加一遍，双屏右侧那块会被报成 `x=3850` 而不是 `1930`。
第一版就是这么写的，端到端测试当场抓到（`left: 9, right: 5`）——**纯函数单测全绿而端到端红**，
因为单测里 `dst_x` 写的是 0。这条教训比修法本身值得记：涉及两套坐标系时，单测必须让两套
坐标系取不同值，否则等于没测。第二版专门加了 `blit_damage_leaves_the_destination_offset_to_the_caller`，
用一个 `dst_x = 1920` 的监视器把这条不变量钉住。

**空集语义必须保持原样。** 回退路径里"监视器报了重复帧"和"监视器说变了但给不出位置"是两件事：
前者 `is_duplicate = true` 且空列表 ⇒ 这一路没有贡献；后者 `is_duplicate = false` 且空列表
⇒ `append_exact_sample_damage` 把整个聚合清空并标记不精确 ⇒ 全帧刷新。裁剪函数对空输入返回
空，不发明矩形，正是为了不打乱这个判据。

验证：

| 检查 | 结果 |
|---|---|
| `cargo test -p snow-capture --lib` | 283 → **291 通过**（+6 纯函数、+2 端到端） |
| `--all-features` | 299 通过（+8） |
| 双架构 `check`（x86_64 / aarch64） | 绿 |
| `check-rust.sh snow-capture`（fmt + clippy `-D warnings`，host 与 linux 目标） | 绿 |
| 反向验证 3 点 | 调用点改回 `Vec::new()` ⇒ 两个端到端测试都红；裁剪换成 `max`（过度上报）⇒ 4 个纯函数测试红；目标偏移加两次 ⇒ 端到端 + 不变量测试都红 |

端到端用的是"两台并排 8×8 监视器、区域横跨接缝、后端没有加速区域路径"的假后端，两台监视器
故意把脏矩形放在不同偏移，这样两侧的目标偏移一旦搞混就必然对不上。第二个端到端用例把区域
只放在右侧监视器上，验证左侧的脏矩形被丢掉而不是被平移进输出。

**本片未覆盖**：

- 仍然**没有真机**。证明的是"坐标系换算与裁剪正确 + 双架构类型正确"，不是"UOS 上多屏录制
  码率真的降了"。到真机的判据同上：录一段双屏区域，静止时看编码器输出的帧大小是否接近 0。
- **`capture_region_into` 仍然只有 Windows 实现**。X11 后端本可以直写输出帧（它有
  `capture_image(rect)`，天然支持子矩形），但那是"省一次 memcpy"，与本片的"省一次全帧编码"
  不是同一件事，属另一片。
- **`capture_desktop_region_into` 同理**：单显示器整屏区域的那条快路径仍只有 Windows 后端实现，
  Linux 上它返回 `None` 后落到本片修好的回退路径。
- 脏矩形仍然**不做合并**：两个监视器各报一块，聚合里就是两块。编码器侧的合并策略不在这一层，
  本片不替它决定。

#### 9.4.22 P3 slice 10：X11 PRIMARY selection 取选中文字（2026-10-21）

`snow-selected-text` 原先只有 `platform/{windows,macos}`，Linux 上 `SelectedTextService::new()`
恒 `UnsupportedPlatform` ⇒ C++ 的 `SelectedTextTranslationController` 在 Linux 上永远走失败分支
（"划词翻译"整个功能缺失）。§4.2 里这一行写的就是"AT-SPI2（D-Bus）+ PRIMARY selection，未开始"。

**PRIMARY 不是剪贴板，这决定整片形态。** X11 上被选中文字的规范位置就是 PRIMARY selection：
owner 发布、任何客户端可读。所以 Linux 路径**不注入按键、不开剪贴板、不拍快照、不需要恢复**，
`clipboard_status` 恒 `Unchanged`，`CaptureStrategy::Clipboard` 直接报 `Unsupported`（那意味着
往一个本进程无法校验的窗口里合成 Ctrl+C，与 Windows/macOS 的"有快照可回滚"完全不是一件事）。
`NativeControl` 同样 `Unsupported`：X11 核心协议没有控件级文本，那需要 AT-SPI2。

**方法字段不能借 stage 名凑数。** `policy::acquire` 原先直接把"哪个 stage 跑成功了"写进
`method`，Linux 上会把 PRIMARY 报成 `Accessibility(1)` —— 一个可机读字段说了假话。新增
`RetrievalMethod::Primary = 4` 并给 `Backend` 加 `passive_method()`（默认 `Accessibility`，
X11 后端的 `mod.rs` 覆写）。**strategy 与 method 是两个轴**：strategy 是"要哪一段"，
显式 `Accessibility` 在 Linux 上照样报 `Primary`，因为报的是实际用了什么。C ABI 同步加
`SNOW_SELECTED_TEXT_PRIMARY`（纯增量；C++ 侧只读 text/error，不看 method）。

**目标协商**：先问 `TARGETS`，按 `UTF8_STRING` > `text/plain;charset=utf-8` > `STRING`
(ISO 8859-1) > `TEXT` 取；owner 拒答 `TARGETS`（`xterm` 一类）就逐个试，一次拒绝只花一个
`ConvertSelection` 往返。**`COMPOUND_TEXT` 故意不问**：它是依赖 locale 的复合编码，没有 Xlib
的转换器就只能自己瞎猜，猜错就是乱码 —— 只提供它的 owner 报 `Unsupported`，不吐脏数据。

**本片最容易写错的一行是判据顺序。** `INCR` 公告回来时 `format` 是 **32**、值是 32 位长度估计，
所以 `fetch_step` 里"先判 `format == 8`"会把所有大传输当场判成 `MalformedData`。纯函数把这条
顺序固定下来，单测名字就叫 `incr_announcement_is_recognised_before_the_format_check`，反向验证
时正是把这条判断移到最前面来制造红。

**`GetProperty` 的 offset 单位是 4 字节，不是字节**，且非末尾回复可能不足一个单位 ⇒
`next_offset` 除 4 后 `max(1)`，否则破坏性的 owner 能让读取原地打转。长度上限在
`append_chunk` **拷贝之前**检查，伪造的 owner 无法逼出超过 `max_text_bytes` 的分配。

**超时靠自己盯**：`RustConnection` 在连接时就把 socket 设成非阻塞，`poll_for_event()` 不会阻塞，
所以 `SelectionNotify`/`PropertyNotify` 的等待是 1 ms 粒度的轮询 + deadline 判定。X server 自己的
reply（`GetProperty` 等）走 cookie 的阻塞等待 —— 那部分只能假定 server 活着，否则任何 X 客户端
都会卡死。

**源应用识别是尽力而为，永不失败。** `_NET_ACTIVE_WINDOW` → `_NET_WM_PID`（最多向上走 3 层，
防一些 WM 把 frame 报成活动窗口）→ `/proc/<pid>/exe`（去掉内核在二进制被就地替换后追加的
`" (deleted)"` 后缀，自更新正好会触发）。**裸 X（无 WM）没有 `_NET_ACTIVE_WINDOW`，但 PRIMARY
照样读得到**，所以 `capture_source()` 返回 `Ok`，字段各自降级为 None/0/空串 —— 若照 Windows 的
写法在这里返回 `NoForegroundWindow`，等于把"能读的文字"变成"报错"。

分层照铁律切：纯逻辑在 `snow-selected-text/src/selection.rs`（`cfg(any(target_os="linux", test))`，
16 个单测在 Windows 上跑），传输在 `src/platform/linux/{selection,source}.rs`（只
`cfg(target_os="linux")`）。依赖只加 `x11rb`（`default-features = false`，零扩展特性），
**刻意不引 `snow-linux`** —— 这条路径只需要 xproto，而 `snow-selected-text-c` 是独立 archive，
多引会把它的依赖面撑到 zbus/ab_glyph/ttf-parser，失去"独立 archive 只依赖纯 Rust 小集合"的意义。

Wayland：`WAYLAND_DISPLAY` 有而 `DISPLAY` 空 ⇒ `UnsupportedPlatform`。PRIMARY 在 Wayland 归合成器，
只有焦点客户端能读，本进程作为后台请求方没有合法路径。

验证：

| 检查 | 结果 |
|---|---|
| `cargo test -p snow-selected-text --lib` | 33 → **50 通过**（+16 纯函数、+1 policy 映射），另有 1 个默认忽略的真机用例 |
| `cargo test -p snow-selected-text -p snow-selected-text-c` | 50 + 6 通过 |
| 双架构 `check`（x86_64 / aarch64） | `checked=4 failures=0` |
| fmt + clippy `-D warnings`（host 与 linux 目标、`--all-targets`） | 绿 |
| 反向验证 5 点 | 全红：判据顺序对调、NUL 裁尾改判空格、忽略 TARGETS 列表、passive 段自报 stage 名、长度上限失效 |

反向验证脚本 `.workbuddy/uos-port/reverse-verify-primary.py` 里踩到一个值得记的坑：`copy2`
恢复文件会带回**更旧**的 mtime，cargo 于是信任上一次变异编译出的产物，下一次跑出来的是变异结果
而不是恢复后的结果。恢复后必须 `os.utime(path, None)`。这也说明"某次反向验证是红的"必须配合
"恢复后确实绿"一起看，否则可能两边都在看同一份过期二进制。

**本片未覆盖**：

- **`platform::linux::*` 一行都没在真机跑过。** 未验证：`ConvertSelection` → `SelectionNotify`
  往返、INCR 的 `PropertyNotify` 逐块取、真实 owner 的 `TARGETS` 应答格式、
  `_NET_ACTIVE_WINDOW`/`_NET_WM_PID`、裸 X（无 WM）、Xwayland 下 PRIMARY 是否真通、以及
  dde-kwin 桌面下 GTK/Qt 应用发布的主选择内容。到真机的判据：开一个编辑器选中一段中文，
  跑 `cargo run -p snow-selected-text --example selected_text`，看拿到的字与选中一致。
- **`TARGETS` 的 32 位列表按本机字节序解释**（`from_ne_bytes`）。X 握手声明的是客户端字节序，
  且 x11rb-protocol 自身也只按小端解析属性，两者同源；只有 owner 用非 32 位格式写 `TARGETS` 时
  才退化成逐个目标试（行为正确，只是多几个往返）。
- **只有 PRIMARY，没有控件级/AT-SPI2**：`native_focus` 恒 `None`，`bounds` 恒空。想要"选中文字的
  屏幕矩形"（翻译气泡定位用）必须走 AT-SPI2，本片不涉及。
- **不重连**：一次抓取失败不会重建连接；X 连接整个断掉后该进程后续请求全是 `NativeApi`，
  与 Win/macOS 的固定 worker 同构（`SelectedTextService` 的 `OnceLock` 也不重试）。
- **C++ 侧未接线验证**：`snow_rust_ffi` 在 Linux 目标仍不能 `cargo check`（9.4.2 A 类），
  打开 `selected-text` feature 后能不能链接只能到真机验。
- **`snow-selected-text-c/tests` 的独立 C/C++ 消费者**没有为新常量补用例（它不读 method）。

#### 9.4.23 P3 slice 11：X11 区域采集只读子矩形（2026-10-21）

**问题**：`snow-capture` 的区域采集走 `MonitorCapturer::capture_region_into`，Linux 后端没实现
⇒ 落回默认的"整屏采集 + `copy_region_rgba` 裁剪"。2560x1440 的显示器上截一块 300x200 的区域，
要服务器写 14 MB 进共享段、本地再逐行拷 14 MB，最后留下 240 KB。`GetImage` 一直支持子矩形；
挡路的是**解码端要求目标缓冲区尺寸恰好等于图像尺寸**，所以不能直接往更大的帧里写。

**三层改动**：

| 层 | 文件 | 内容 |
|---|---|---|
| 纯逻辑 | `snow-linux/src/capture.rs` | `ImageLayout::decode_window_into`（按目标 stride 写入指定窗口）+ `region_read_rect`（blit → 根窗口矩形，或拒绝）+ `OutputWindow`/`RegionBlit` |
| 传输 | `snow-linux/src/x11.rs` | `RawImage::decode_window_into` 转调 |
| 后端 | `snow-capture/src/platform/linux.rs` | `capture_region_into` → `read_region_into`：只 `GetImage` 子矩形，脏矩形照旧来自 Damage |

**四个关键决策**：

1. **解码按下标窗口写，而不是先解到临时缓冲再逐行拷**。`decode_into` 因此退化成
   `decode_window_into(…, OutputWindow::full(w, h), …)`，快慢两条路共用同一份行循环——否则
   就会出现"全帧解码"和"窗口解码"两份实现，其中一份的正确性没人测。
2. **拒绝比聪明便宜**。`region_read_rect` 对四类输入直接返回 `None`（交给既有的整屏回退：
   它慢但不坏）：窗口不在根窗口内、窗口超出它所属的帧、目标放不下、协议字段放不下
   （`i16` 原点 / `u16` 尺寸）。**窗口一半在屏外不在这里做平移**——那种情况下回退路径的
   钳位语义才与 `capture_target_info` 报的矩形一致。
3. **跳帧要三个条件，不是一个**。`destination_has_history`（调用方担保缓冲里是这一块的上一帧）
   ∧ `last_window == (这次窗口, 这次尺寸)`（上次读的**就是这块**）∧ Damage 说 Skip。
   缺第三条会把"另一块区域没变"当成"这块没变"，把别的区域的像素留在目标里。
4. **区域尺寸不能用 `last_extent` 记**。同一个 capturer 实例既服务区域读、也服务整屏回退
   （回退正是本片 `None` 时跑的那条路），共用一个 extent 会让每次切换都命中
   `ResolutionChanged`。所以另开 `last_window: Option<(Rect, (u32, u32))>`。

**顺带修掉一个已存在的红**：`tests/screenshot_stage_timing_smoke.rs` 自 `f93b1402`
（加 `CaptureBackendKind::X11`）起**编译不过**——两处 `match` 非穷尽。它 `#![cfg(windows)]`，
Linux CI 上编译成空 crate 所以一直没暴露，只在本机 `cargo test --all-features` 撞到。本片补上
`X11` 分支。**注意**：能编译之后该文件里三个用例在本机是红的（`stage timings missing on
captured frame`，DXGI/WGC/GDI 各一），与本片无关、待单独查——它们是唯一能发现"计时没挂上"
的测试，此前一直没被跑过。

**验证**：`cargo test -p snow-linux --lib` 219 → **232**（+13：窗口解码 7、区域矩形 6）；
`snow-capture --all-features` 299 lib 用例全绿；双架构 `cargo check` 8/8 PASS；
`fmt` + `clippy`（host 与 linux 目标、`--all-targets`、`-D warnings`）绿；反向验证 6 点全红。

**本片未覆盖**：

- **真机一行没跑**：`capture_region_into` 的整条路径（`GetImage` 子矩形 → 窗口解码 →
  目标帧）只做过类型检查。判据：UOS 上框选一块区域截图，与整屏路径的同一区域逐像素比对，
  并确认 `dirty_rects` 与整屏路径一致。
- **`ShmGetImage` 的子矩形尺寸推导**未变（`shm_segment_size` 按请求矩形算），但"同一次会话里
  从大矩形切到小矩形"（段只增不减）只在代码上成立，没有真机往返验证。
- **多个区域共享一个 capturer**：`last_window` 是单槽，来回切换两块区域的 `capture_region_into`
  会一直读（不跳帧），行为正确但没有为它专门做多槽缓存。

#### 9.4.24 P3 slice 12：X11 采集计时真的产出，阶段名不再漂移（2026-10-21）

上一片的"待单独查"就是这一片。查下来是两个互不相干的洞，恰好都在"计时"这条线上：

1. **`record_stage_timings` 在 Linux 上是句空承诺**。`X11Capturer` 既没实现
   `set_record_stage_timings`（走的是 trait 默认的 no-op），也从不 `StageScope::enter`，
   于是每个 marker 都是空转、`stage_timings` 恒为空。更糟的是 session 侧照旧把
   `capture_duration` 填上（`stamp_captured_frame`），帧上就出现"总耗时 8 ms、明细 0 项"——
   消费方分不清"没测"和"没耗时"。
2. **Windows 的冒烟测试自带一份阶段名前缀白名单**，而它是**唯一**枚举阶段名的地方。
   `653a4102` 给分配器加了 `buffer.allocate` / `buffer.writable_reuse`，白名单没跟上 ⇒
   DXGI/WGC/GDI 三个用例一起红。它 `#![cfg(windows)]`，Linux CI 上编译成空 crate；本机也只在
   `--features stage-timing` 时才构建，所以红着没人看见。

**改动**：

| 层 | 文件 | 内容 |
|---|---|---|
| 契约（唯一来源） | `snow-capture/src/timing.rs` | `BACKEND_STAGE_PREFIXES`（`dxgi./wgc./gdi./x11.`）+ `AGNOSTIC_STAGE_PREFIXES`（`readback./buffer.`）+ `is_known_stage` |
| 门禁（跨平台） | `timing.rs` 的 `tests` | 扫 `src/**/*.rs` 里所有 `stage_mark` / `stage_record` / `stage_record_since` 的**字面量**，断言每个都落在已声明家族内；跳过 `timing/` 自身（那里有 `stage.one` 这类测试夹具） |
| 后端 | `snow-capture/src/platform/linux.rs` | `record_stage_timings` 字段 + `set_record_stage_timings`；`capture` / `capture_with_history_hint` 开头 `StageScope::enter`、结尾 `attach_stage_timings`；新增 `x11.sys.get_image`、`x11.decode`、`x11.damage_drain`、`x11.skip_hold` 四个阶段 |
| 冒烟 | `tests/screenshot_stage_timing_smoke.rs` | 前缀判据改用 `AGNOSTIC_STAGE_PREFIXES`；新增"明细里必须同时出现 `buffer.*`"的断言 |

**五个关键决策**：

1. **`buffer.*` 是诊断明细，不是可加桶**。GDI 的 `readback.frame_alloc` 量的正是
   `frame.prepare_for_overwrite(...)` 这一段，而它内部就 `stage_record("buffer.allocate")`——
   两者相加会把同一段微秒数两遍。所以 `buffer.` 进 `AGNOSTIC_STAGE_PREFIXES`、**不进**
   `primary_stages`。新增的那条断言把这件事钉住：明细里必须**同时**有 `readback.frame_alloc`
   与 `buffer.*`，否则说明 scope 没覆盖到帧交接。
2. **白名单放进库，不留在测试里**。名字的**产生方**（`frame.rs`、各后端）与**校验方**（冒烟
   测试）必须共享同一份声明，否则下一个家族照样能溜过去。测试自己只保留一条更严的判据：
   "本后端前缀必须匹配"——`dxgi.` 出现在 WGC 的明细里仍然要红。
3. **用源码扫描补上 `#![cfg(windows)]` 的盲区**。Windows-only 测试在 Linux CI 里是空 crate，
   这是本轮两个洞里唯一"没人看着"的那个。扫 `src/` 是全平台、全 feature 组合都跑的判据，
   而且它扫的正是人写阶段名的方式（字面量）。
4. **Linux 的 region 路径刻意不开 scope**。`CaptureSampleMetadata` 没有 timings 字段，
   开一个再把结果丢掉只是骗自己。
5. **跳帧的 `duplicate_hold` 要记成 `x11.skip_hold`**。"跳过一帧"和"真读一帧"耗时相当，
   本身就是策略问题的信号；不测出来就只能靠猜。

**验证**：`snow-capture --all-features` lib 299 → **302**（+3 计时契约用例）；
`--features stage-timing` 的 4 个冒烟用例 3 红 1 绿 → **全绿**；双架构 `cargo check` 8/8 PASS；
`clippy`（host `--all-targets --all-features`，x64/arm64 linux `--lib` 的两种 feature 组合）绿；
反向验证 **5/5 红、2/2 基线绿**（`.workbuddy/uos-port/reverse-verify-stage-timing.py`）。

**本片未覆盖**：

- **Linux 的计时只在类型层面成立**。没有 X server 就没有 `capture`，`x11.*` 四个阶段一次都没
  被真正记录过。真机判据：UOS 上开 `record_stage_timings` 抓一帧，断言明细非空、
  `x11.sys.get_image + x11.decode + buffer.allocate ≤ capture_duration`。
- **源码扫描是文本级判据**：认 `stage_record("...")` 的写法，认不出通过常量间接传入的名字
  （当前代码里没有这种写法）。
- **脚本自身的坑**：本片的反向验证脚本第一次运行时被沙箱 `SIGTERM` 打断，Python 的 `finally`
  没执行，于是 `BACKEND_STAGE_PREFIXES` 留在"缺 `x11.`"的变异状态；下一次运行把两个用例报成
  `pattern occurs 0 times`——看着像"找不到目标"，实际是**上一次的变异没被撤销**。现在脚本启动
  时先扫 `*.revbak` 自动恢复，并且把最贵的用例放在第一位（要杀就趁还没改过任何文件）。

#### 9.4.25 P3 slice 13：AT-SPI2 —— 控件级文本与选中矩形的地基（2026-10-21）

9.4.22 的 PRIMARY 读得到字，但**拿不到那段字画在屏幕的哪儿**：`native_focus` 恒 `None`、`bounds`
恒空，翻译气泡只能跟着鼠标走。控件级文本同理。X11 核心协议里没有控件树，那在 AT-SPI2 里——
一条**独立于 session bus** 的 D-Bus 连接，每个 toolkit 在它上面发布自己的控件树。

本片只做 `snow-linux` 这一层，**不接线** `snow-selected-text`（理由在末段，那是一个要先定的架构问题）。

> 注：本片的模块路径在 9.4.26 已搬到独立 crate `snow-atspi`（`lib.rs` + `bus.rs`），
> `snow-linux::a11y` 不再存在。下面按**当时落地的位置**记录，能力描述不变。

| 层 | 位置 | 平台 | 内容 |
|---|---|---|---|
| 纯逻辑 | `snow-linux/src/a11y/mod.rs` | **全平台** | `ObjectRef`（含空 bus name 继承）、state 值、`char_range_to_byte_range`、`choose_selection`、`extents_to_rect`、`clamp_text`、有界 BFS `find_focused`、`match_app_by_pid`；单测**在 Windows 上跑** |
| 传输 | `snow-linux/src/a11y/bus.rs` | linux | `org.a11y.Bus.GetAddress` → 第二条连接；`GetConnectionUnixProcessID` 把 X11 的 pid 对到 a11y 应用；`GetChildren`/`GetState`/`GetInterfaces` 走树；`GetNSelections`/`GetSelection`/`GetText`/`GetRangeExtents` 读选中 |

**七个关键决策**：

1. **`GetText` 的偏移是字符，不是字节。** 它们是 UCS-4 码点，而 Rust 字符串按字节索引：
   ASCII 上两者一致，中文上完全不是一回事，`&text[1..2]` 直接在非边界处 panic。
   两端策略刻意不对称 —— `end` 越界**钳位**（toolkit 先算后读，中间文本可能变了，读到前缀仍有用），
   `start` 越界**拒绝**（说明两边对"文本是什么"的认知已经分叉，返回空串会被误读成"没选中"）。
2. **空 bus name 表示"与父对象同一条连接"。** 规范允许子引用省略它，把它当字面空串就会把下一次调用
   发给总线守护进程——那个对象既不在 accessibility bus 上，也不可能实现 `Text`。
3. **`GetState` 返回的是枚举值列表，不是位掩码。** 它回 `au`，里面是会下标的 `AtspiStateType`。
   写成 `states & (1 << FOCUSED)` 既编译又运行，只是永远为假。
4. **找焦点是有界 BFS，而且 `FOCUSED` 不等于"这段是文本"。** 窗口框、按钮、列表行都会报 `FOCUSED`。
   所以先找"`FOCUSED` ∧ 实现 `org.a11y.atspi.Text`"的节点并立刻返回，第一个"只有 `FOCUSED`"的
   只留作兜底。用 BFS 而不是 DFS：先看见窗口层，别把预算花在一条陈旧分支上。
   `DEFUNCT` 剪枝（子树已被 toolkit 抛弃），环用 visited 挡住（`Parent` 指回上面、同一子节点报两次
   都真实存在）；预算 256 次访问 / 深度 24，每次访问至少一次往返，所以它必须有界。
5. **定位应用靠 pid，而 pid 只能问总线守护进程。** `org.a11y.atspi.Accessible` **没有** `GetProcessId`
   这种东西（`Application` 接口也没有），所以：X11 `_NET_WM_PID` 拿到 pid，对每个应用的 bus name 调
   `org.freedesktop.DBus.GetConnectionUnixProcessID` 比对。`pid == 0` / `u32::MAX` 一律**不匹配**——
   否则"未知"会变成"匹配到另一个同样未知的应用"，比读不到更糟。
6. **超时交给连接，不用 `portal::deadline`。** `timeout_after` 每次调用起一个线程；portal 一个会话只调
   几次，这里一次抓取是几百次调用。改用 `Builder::method_timeout(500ms)`：到期由 zbus 自己的运行时
   让 future 返回 `TimedOut`，不需要时零成本。
7. **单次调用失败 = "没有"，不是"中止"。** 树里大半控件根本没实现 `Text`，问它们的选中必然失败，
   而那个失败与应用无关。所以读取返回 `Option`，只有 `AccessibilityBus::connect` 报错——"没有
   accessibility bus"是调用方必须知道的事实（它要退回 PRIMARY）。

**本片查出来的四个硬事实**（凭记忆写必然错，都对着 `at-spi2-core` 的 XML / 头文件核过）：

| 事实 | 凭记忆会写成 | 实际 |
|---|---|---|
| `AtspiStateType` 的 `SHOWING` | 30 | **25**（30 是 `VISIBLE`）；`FOCUSED = 12`、`DEFUNCT = 6`、`SELECTABLE_TEXT = 38` |
| 取状态的方法名 | `GetStateSet` | **`GetState`**（社区文档两种写法都有，XML 里只有 `GetState`） |
| 应用 pid 从哪来 | `Accessible.GetProcessId` | 不存在，只能问 `org.freedesktop.DBus` |
| `org.a11y.Bus` 的位置 | 记不清 | `org.a11y.Bus` 服务、`/org/a11y/bus` 路径、只有 `GetAddress` |

**验证**：

| 检查 | 结果 |
|---|---|
| `cargo test -p snow-linux --lib` | 232 → **259 通过**（+27 纯逻辑用例） |
| 双架构 `cargo check`（x86_64 / aarch64） | `checked=4 failures=0` |
| 跨目标 `clippy --lib --tests -D warnings` | 绿 |
| fmt + host `clippy --all-targets --all-features` | 绿 |
| 反向验证 | **11/11 红、2/2 基线绿**（`.workbuddy/uos-port/reverse-verify-a11y.py`） |

**本片顺手补上的一条判据**：`cargo check --lib` **不编译 `#[cfg(test)]`**，所以 `bus.rs` 里那三个单测
在 Linux 目标上一次都没被看过一眼；`--lib --tests` 才把 lib 的 test target 编进去。已用"往测试里塞一个
类型错误、确认跨目标检查失败"反向证实（否则这条判据本身也是自说自话）。这和 9.4.24 里"源码扫描补
`#![cfg(windows)]` 盲区"是同一类问题：**只在某个 cfg 下存在的代码，需要一个真的会看它的检查。**

**本片未覆盖**：

- **整条传输路径一行没跑**：`GetAddress`、`GetConnectionUnixProcessID`、`GetChildren` 的 `(so)` 解码、
  `GetSelection` 的 `ii` 解码（两个独立出参 vs 一个 `(iiii)` 结构，靠 zvariant 的元组解码统一）、
  `GetRangeExtents` 的坐标系（0 = 屏幕）、只提供 `Text` 但不实现 `Component` 的控件。真机判据：
  UOS 上开一个编辑器选中一段中文，走 `read_selection` 看文本与矩形都与选中一致。
- **没接线**：`snow-selected-text` 仍只读 PRIMARY，`bounds` 仍空。接线要先回答一个架构问题 ——
  9.4.22 明确让 `snow-selected-text` **不依赖** `snow-linux`（`snow-selected-text-c` 是独立 archive，
  多引会把 `ab_glyph`/`ttf-parser` 拖进它的依赖面），而 AT-SPI2 需要 `zbus`。三个选项：
  ① 在 `snow-selected-text` 里复制一份 a11y（两份实现，必然漂移）；② 引入 `snow-linux`（依赖面撑大）；
  ③ 把这一层抽成独立的 `snow-atspi` crate（只依赖 `zbus`，两个消费者都引）。**推荐 ③**，
  它同时满足"独立 archive 只依赖纯 Rust 小集合"和"一份实现"。
- **一个应用多条连接时取第一个 pid 匹配**；Qt 应用常有"主连接 + 辅助连接"两个 bus name。
- **某个 toolkit 若违反规范按字节报偏移，本层察觉不到**：解码后的字符串与偏移对不上时，
  `clamp_text` 与 `get_text` 都只会给出错误但合法的切片。只能在真机上按"中文选中"验。
- **`GetText` 的传输量不受控**：上限 `MAX_TEXT_BYTES`（64 KiB）是在**收到之后**截断，
  真正的防护是"只请求应用自己报的那个区间"。

#### 9.4.26 P3 slice 14：AT-SPI2 抽成 `snow-atspi` 并接进 `snow-selected-text`（2026-10-21）

9.4.25 把 AT-SPI2 写完了，但没接线 —— 它落在 `snow-linux::a11y`，而 `snow-selected-text` 刻意不依赖
`snow-linux`（`snow-selected-text-c` 是独立 archive，多引会把 `ab_glyph`/`ttf-parser` 拖进依赖面）。
那一节给了三个选项，推荐的是 ③ 抽独立 crate。本片按 ③ 执行，并完成接线。

| 动作 | 位置 | 内容 |
|---|---|---|
| 搬 | `snow-linux/src/a11y/{mod,bus}.rs` → `snow-atspi/src/{lib,bus}.rs` | `git mv`，纯逻辑与传输原样迁移；`Rect` 换成自带的 `Bounds`，`LinuxError` 换成自带 `AtspiError`（一个不透明 message：这一层所有错误对调用方都是同一件事——"这条路不可用"） |
| 新 | `snow-atspi` 的 `WalkLimits::deadline` | 访问预算只管**工作量**，不管**时间**（见下文第 6 条） |
| 新 | `snow-selected-text/src/selection.rs` 的 `choose_answer` / `accessibility_answered` | 两条路的取舍。放在这个文件是因为它"无需连接即可判定"，于是在 Windows 上可单测 |
| 改 | `platform/linux/mod.rs` | 先 AT-SPI2（带矩形），没有才读 PRIMARY；`method` 报真正答话的那条路 |

**七个关键判断**：

1. **`snow-atspi` 是普通依赖，不是 Linux-only 依赖。** crate 根是平台中立逻辑，只有 `bus.rs` 挂在
   `target_os = "linux"` 上。这样 `snow-selected-text` 的**路线判定与矩形换算**能在 Windows 上单测 ——
   它们正好是这次唯一真正会判错的地方。代价是 Windows 构建也会编这 900 行零依赖代码，值。
2. **AT-SPI2 有文本就赢，即使 PRIMARY 也有。** 前者说的是"焦点窗口里焦点控件自己的选中"，后者是
   会话级的"最后被选过的"（可能属于另一个应用）。两者**永不相加**：一致时它们是同一段文本，相加就是
   报两遍。
3. **空答案不是答案。** 零宽选中（光标停着）、刚重排过还没实现的控件都回空文本。让它压住 PRIMARY，
   就把"能读"变成"没选中"。所以 `choose_answer` 用 `filter(has_text)` 而不是 `is_some()`；
   `accessibility_answered` 与它共用 `has_text` 这一个谓词，两处不会分叉。
   （反向验证里这两条是两个独立用例：把 `is_some` 当答案 → 红；把空的 PRIMARY 当答案 → 红。）
4. **`method` 必须报真正答话的那条路。** `Backend::passive_method(&self)` 在阶段**跑完之后**才被问，
   答案不可能是参数，所以 `Worker` 用 `Cell<RetrievalMethod>` 记下 passive 阶段实际用的那条。
   陈旧值读不到：整个阶段是 `&mut self`，`passive_method` 只在同一个 `acquire` 里紧跟其后被调一次。
5. **AT-SPI2 只拿阶段预算的头 350 ms，PRIMARY 留 250 ms。** 阶段总预算 600 ms。AT-SPI2 是增强，
   PRIMARY 是保底，保底不能先被饿死。AT-SPI2 答话时干脆**不读** PRIMARY —— 那几趟往返只会确认同一段字。
6. **一次调用的超时 × 访问次数 ≠ 阶段预算。** `Builder::method_timeout(500ms)` 只管单次调用，
   256 次访问 × 500 ms = 128 s，而阶段预算是 600 ms。所以 `WalkLimits` 加 `deadline`，
   `find_focused` 每次访问**前**查一次；`applications`（枚举桌面应用，数量不受控）同理。
   判据是 `an_expired_deadline_stops_the_walk_before_it_reads`：期限已过时**一次调用都不发**
   （断言 `tree.reads == 0`），而不是"发了才发现晚了"。
7. **顺手补上 session 连接的期限。** `GetAddress` 走的是 session 连接，原先用 `Connection::session()`
   建，没有 `method_timeout`，是整个 crate 里唯一无人看守的调用。改成
   `Builder::session().method_timeout(CALL_TIMEOUT)`。

**验证**：

| 检查 | 结果 |
|---|---|
| `cargo test -p snow-atspi` | **28 通过**（27 个跟着模块搬来 + 1 个新期限用例） |
| `cargo test -p snow-selected-text` | 50 → **56 通过**（+6 路线选择用例） |
| `cargo test -p snow-linux --lib` | 259 → **232**（27 个随模块搬走，不是被删） |
| 双架构 `cargo check`（x86_64 / aarch64） | `checked=10 failures=0`（`snow-atspi`、`snow-linux`、`snow-linux-c`、`snow-selected-text`、`snow-selected-text-c`） |
| 跨目标 `clippy --lib --tests -D warnings` | 绿 |
| fmt + host `clippy --all-targets --all-features` | 绿 |
| 反向验证 | **18/18 红、0 只崩编译、2/2 基线绿**（`.workbuddy/uos-port/reverse-verify-a11y.py`） |

**反向验证脚本这一片暴露了自己的一个假门禁**：某个用例的变异（把 `bounds` 直接置空）让
`desktop_rect` 变成死函数，于是"红"来自 `-Dwarnings` 的 `dead_code`，而不是断言。脚本原先把它计成
RED —— **编译失败比断言失败证明得少**，它可能说明这条变异根本不是行为性的。现在单列 `WEAK`
且不计入通过数；那条用例也改成了 `filter(|_| false)`（保留函数在用，只丢数据）。

**本片未覆盖**：

- **整条传输仍未在真机跑过一次**（同 9.4.25）。真机判据：UOS 上开 gedit 选中一段中文，走
  `capture` 看 `text` 与选中一致、`ranges[0].bounds` 落在选区上、`method` 为 `Accessibility`(1)；
  再关掉 `at-spi2-core`，同一操作应仍能拿到文本而 `method` 变成 `Primary`(4)、`bounds` 为空。
- **连接握手没有期限。** `Builder::build()` 的 socket 握手不吃 `method_timeout`，而连接是在首次抓取时
  才建的。所以进程内第一个请求可能超出 600 ms 阶段预算（只有一次）。没量过，也不打算在这个片里加
  看门狗线程 —— 那会比问题本身贵。
- **应用枚举被 `deadline` 截断时，"不在列表里"和"不存在"不可区分**：结果都是"这次没找到"，退到 PRIMARY。
- **`native_focus` 仍恒 `None`。** AT-SPI2 没有整数控件 id 这种东西；`bounds` 已经覆盖了它原本的
  主要用途（气泡定位），留 `None` 比编一个假 id 诚实。
- **`snow-ui-selector` 的控件级拾取没做**：`snow-atspi` 已经把需要的东西备好了，但那边要改的是
  `HitTestMode::UiElement` 的返回结构与 C ABI，属另一片。**已在 9.4.27 补上**（结论：C ABI 不用改，
  `path` 本来就是「最内层在前」的矩形链）。

#### 9.4.27 P3 slice 15：控件级拾取——点命中下降接进 `snow-ui-selector`（2026-10-21）

9.4.25/9.4.26 让选中文字有了矩形，但 `HitTestMode::UiElement` 在 Linux 上仍只回一个窗口矩形
（9.4.6 的窗口级 provider 把两种模式答成一件事）。本片把控件级接上：指针落点进去，一路
`Component.GetAccessibleAtPoint` 下探到最内层控件。

| 层 | 位置 | 内容 |
|---|---|---|
| 纯逻辑 | `snow-atspi/src/lib.rs` | `ComponentSource`（`child_at_point` + `extents`）、`descend_to_point`（有界下降）、`Bounds::{contains,contains_rect,is_empty}`、`hit_path`（链→路径）；单测在 Windows 上跑 |
| 传输 | `snow-atspi/src/bus.rs` | `AccessibilityBus::point_path(pid, point, window, deadline)`：按 pid 找应用 → 下降 → 读 extents → `hit_path` |
| 数据 | `snow-linux/src/{window.rs,x11/window.rs}` | `WindowAttributes.pid` / `WindowInfo.pid`：读 `_NET_WM_PID`（可上溯 8 层），0 视为"没有" |
| 接线 | `snow-ui-selector/src/linux.rs` | `HitTestMode::UiElement` 先发窗口矩形再走 AT-SPI2；后者答话就换成控件链，否则留窗口矩形。`HitTestMode::Window` 不变 |

**七个关键判断**：

1. **点命中要下探，不要扫描。** 找焦点只能 BFS（没有任何接口能"问焦点"），但找落点可以：
   `GetAccessibleAtPoint` 让工具包自己往下指。六层树 = 六次调用，而不是最多 256 次访问。
   代价是**信任**工具包——某个控件在自己身上回"这里什么都没有"，下降就早停一层，
   拿到的是祖先的矩形；那正是窗口级回退会给的答案，所以失败模式是"粗"而不是"错"。
2. **`GetAccessibleAtPoint` 回 `(so)`，`GetChildren` 回 `a(so)`。** 两者在 Rust 里都能解码成元组，
   用错的哪一个只会在运行时解码失败。已用签名断言把 `(so)` 钉进 `bus.rs` 的测试。
3. **下降有三个终止条件**：null 引用、已经见过的引用、深度/期限。第二条不是防御性编程：
   好几个工具包在叶子节点上回**自己**，没有 seen-set 会一直撞到深度上限，24 次往返白跑。
   这里只查 `max_depth` 不查 `max_visits`——每一层正好一次调用，两个预算其实是同一个数。
4. **矩形要过三道关**：含这个点、落在窗口内、与已接受的内层矩形嵌套且不重复。
   其中"零尺寸"这一关**故意没有单独写**：`(0, 0, 0, 0)` 不含任何点，`contains` 已经挡掉了。
   本片先写了那行守卫，反向验证里那条变异**保持绿**才发现它是任何输入都走不到的代码——
   一个走不到的守卫比没有守卫更糟，它让人以为有保护。已删，理由写在函数文档里。
5. **窗口永远在路径末尾，且只出现一次。** 窗口不含这个点 ⇒ 返回**空路径**而不是窗口：
   那说明调用方拿去命中测试的快照已经过期，硬报窗口等于把这个事实藏起来。
6. **pid 是两棵树唯一的接头。** `_NET_WM_PID` 在客户窗口上（`_NET_CLIENT_LIST` 给的正是它），
   少数窗管会把它复制到自己的框上，所以向上找、上限 8 层。**0 一律当"没有"**：
   否则一个没上报 pid 的窗口会配上一个同样没上报的应用，比读不到更糟。
7. **预算来自调用方，不是常量。** `QueryControl.budget` 是 168 ms（前景）/ 1500 ms（精修），
   与 Windows provider 的遍历预算同一个数。配合 AT-SPI2 每调用 500 ms 的超时，
   真正的上界是 `budget`，而不是"每次调用都很短"。

**验证**：

| 检查 | 结果 |
|---|---|
| `cargo test -p snow-atspi` | 28 → **47 通过**（+19：8 个下降用例 + 11 个路径/几何用例；另在既有签名用例里补了 `(so)`） |
| `cargo test -p snow-linux --lib` | 232 → **233 通过**（+1 pid 随窗口传递） |
| `cargo test -p snow-ui-selector --lib` | **50 通过**，无回归（Linux provider 在 Windows 上不编译，见下） |
| 双架构 `cargo check`（x86_64 / aarch64） | `checked=14 failures=0` |
| 跨目标 `clippy --lib --tests -D warnings` | 绿 |
| fmt + host `clippy --all-targets --all-features` | 绿 |
| 反向验证 | **34/34 红、0 只崩编译、3/3 基线绿**（`.workbuddy/uos-port/reverse-verify-a11y.py`，覆盖 3 个文件 3 个 crate） |

**这一片又一次被反向验证改掉了一个断言**：`contains_rect` 的"空矩形被任何矩形包含"那条规则，
最初的用例用的是原点处的空矩形——算术上不带那条守卫也成立，于是变异保持绿。改成**屏幕外**的空
矩形（`(500,500,0,0)`）才真正钉住"空即真"。同一类问题的第三次：**判据必须能让目标规则自己产生
差别**，否则它测的是别的规则。

**本片未覆盖**：

- **`snow-ui-selector/src/linux.rs` 一行都没被执行过**：它在 `#[cfg(target_os = "linux")]` 之后，
  Windows 上连编译都没有，Linux 上也没有自动化测试跑过它。跨目标 `check` 只证明它**编译**。
  真正的判据在真机：UOS 上悬停在一个 gedit 的按钮上，`count` 应为 ≥2（按钮 + 窗口），
  `rects[0]` 落在按钮上；关掉 at-spi2-core 后同一位置 `count` 应为 1 且是窗口矩形。
- **整条传输仍未在真机跑过一次**（同 9.4.25/9.4.26）：`GetAccessibleAtPoint` 的 `(so)` 解码、
  `GetExtents` 的屏幕坐标系、Qt/GTK 各自下降几层，都没有实测数据。
- **`progress` 会在每次查询里先发一次窗口矩形**：叠加层会先画窗口再换成控件。这是有意的
  （慢工具包不至于什么都不显示），但没有量过它对叠加层刷新频率的影响。
- **连接只尝试一次**：`bus_attempted` 一旦置真就不再重连。无障碍总线在会话中途重启的话，
  这个 service 实例剩下的生命里都会退回窗口级。service 是**每会话**创建的，影响面有限，
  但 `release_cache` 会把它清掉，属于"知道但不修"。
- **多显示器负坐标只做了单位换算**：`Bounds` 是 i32 桌面坐标，`SelectionRect`/`ElementRect`
  各自是 f64 / i32 像素，目前只在纯函数层验过符号不被夹掉，没有在双头布局上真机验过。

### 9.5 打包

#### 9.5.1 P6 slice 1：deb + freedesktop 集成（2026-10-21）

**决定 1：`Depends` 交给 `dpkg-shlibdeps`，不手写。** 本节原来那张草稿写死了
`libqt5widgets5, libqt5svg5, libqt5network5, libpipewire-0.3-0, libpulse0` —— 这是猜的。
链接线一改（多挂一个 Qt 模块、接上 PipeWire、换掉音频后端）它就过期，而症状是**装得上、
起不来**（`error while loading shared libraries`），且只在恰好没装那一套库的机器上复现。
改成 `CPACK_DEBIAN_PACKAGE_SHLIBDEPS=ON`：CPack 对暂存目录里的 ELF 跑 `dpkg-shlibdeps`，从
实际 `NEEDED` 反推依赖。代价是打包机需要 `dpkg-dev` + `file`，交叉打包 arm64 时还要让它能读
目标架构的 ELF。

**决定 2：架构名显式映射，未知架构必须失败。** dpkg 只认 `amd64`/`arm64`/`loong64`/
`mips64el`/`riscv64`。把 `x86_64` 写进 `Architecture:` 的包在任何机器上都装不上，而
`dpkg -i` 的报错完全看不出原因。`snow_shot_linux_debian_architecture()` 做这层映射（顺带
兜住信创侧可能出现的 loongarch64 / mips64el / riscv64），不认识的处理器名直接 `FATAL_ERROR`。

**决定 3：模块只赋值，不 `include(CPack)`。** `include(CPack)` 留在根 `CMakeLists.txt`。
这样 `cmake -P` 能在没有工具链的机器上把整段配置真跑一遍 —— 否则这个模块只会在 Linux
configure 时被解析，又是一块盲区。

**决定 4：四个名字，两个来源。**（本节写于 2026-10-21，当时三个名字都取自
`productId()`；9.6.41 记录了为什么入口文件名必须改，表已按现状更新。）

| 出现在哪里 | 值 | 来源 |
|---|---|---|
| 入口文件名 / AppStream `<id>` | `com.snowshot.snow_shot`（文件 `com.snowshot.snow_shot.desktop`） | `app::edition::desktopEntryId()` |
| `Icon=` | `snow-shot` | `app::edition::productId()` |
| `StartupWMClass=` | `snow_shot` | `app::edition::applicationName()` |
| 自启动条目名 | `snow-shot.desktop`（在 `~/.config/autostart/`，另一个目录） | `app::edition::productId()` |

- 入口文件名不是装饰：`QGuiApplication::setDesktopFileName()` 必须给出同一个值（加
  `.desktop`），否则合成器按 WM_CLASS 单独匹配，从 dock 启动时会出现第二个没有图标的槽位。
- 自启动条目**不跟着改**：它在 `~/.config/autostart/` 下，用户已有的覆盖文件就是那个名字，
  改名等于把所有人手动关掉的开机自启重新打开。两个目录互不相干，不存在冲突。
- 图标不复制：install 时 `RENAME` 成 `snow-shot.svg`。同一份 `app-icon.svg` 也是 Windows
  `.ico` 的来源，复制一份就等着两边漂移。
- 另给 `main.cpp` 补 `QGuiApplication::setDesktopFileName()`：没有它，合成器只按 WM_CLASS
  匹配窗口，从 dock 启动会在固定图标旁多出一个无图标槽位，运行中的实例也不会点亮自己的图标。

**决定 5：NSIS 分支从 `NOT APPLE` 收紧到 `WIN32`。** 原来那个条件是"Windows + Linux"，
在 Linux 上开 `SNOW_APPS_PACKAGE_SNOW_SHOT` 会去调 NSIS —— 一个在 Linux 上不存在的生成器。
Linux 现在走自己的模块。

**验证（本机可跑，不需要 Linux）**：`bash scripts/check-linux-packaging.sh`，两半。

- `cmake -P scripts/check-linux-packaging.cmake`：7 个处理器名映射、未知处理器必须拒绝
  （子进程比对退出码）、25 个 `CPACK_*` 变量、把 `CMAKE_SYSTEM_PROCESSOR` 换成 aarch64 再跑
  一遍、三个安装输入既存在**又真的被 `install()` 引用**（只放文件不装是死重量）、入口必需键。
- `scripts/check-linux-freedesktop.py`：编码（BOM/CRLF）、`[Desktop Entry]` 必需键、无字段码、
  `TryExec`/`StartupWMClass`/`Icon` 与 `edition.h` 一致、Categories 含主分类且分号收尾、
  XML 良构、AppStream 必需元素，以及通用校验器做不到的跨文件一致性
  （`productId` ↔ 文件名 ↔ `Icon` ↔ `launchable` ↔ `provides/binary` ↔ `SNOW_SHOT_VERSION`
  ↔ release 版本 ↔ install 的 `RENAME` 名）。
- 合计 **43 项**检查，全绿。
- **反向验证**：删掉 `Categories=` ⇒ 4 项 FAIL；把 aarch64 映射成 `x86_64` ⇒ cmake 退出 1、
  3 项 FAIL；把 metainfo 的 release 版本改成 1.2.7 ⇒ 跨文件检查 FAIL。
- `main.cpp` 的 `setDesktopFileName` 在 `#ifdef Q_OS_LINUX` 里，Windows 构建编不到、Linux
  语法门禁也覆盖不到（`main.cpp` 其余部分是 Windows-only）⇒ 专门写探针 TU，Qt 5.15.2 与
  Qt 6.12.0 各编一遍，两个 `EXIT=0`。
- `windows-qt5-full` configure `EXIT=0`。

**本片未覆盖**：

- **deb 从未真正生成**：Windows 上没有 `dpkg-deb` / `dpkg-shlibdeps`。`dpkg-shlibdeps` 算出来
  的 `Depends` 实际内容、包体积、`/usr` 布局能否被 UOS 的软件中心索引，只能在 Linux 上答。
- `desktop-file-validate` / `appstreamcli validate` 没跑过：本机没有这两个工具，脚本是按规范
  重写的子集。到真机的第一条命令就是这两个。
- 图标只有 scalable 一份，没有多尺寸 PNG。dde-dock / dde-launcher 的某些版本对 SVG 支持不全，
  可能显示模糊或取不到图。→ **已由 9.5.2 补齐**（7 个 hicolor 尺寸）。
- AppImage 未做（下一片）；`.desktop` 未做翻译以外的本地化（名称与 GenericName 的 zh_CN 已给）。
  → **AppImage 已由 9.5.2 落代码**；本地化仍缺。

产出（Linux 上）：

```
snow-shot_1.2.8_amd64.deb        # 或 _arm64.deb，文件名由 CPACK_DEBIAN_FILE_NAME=DEB-DEFAULT 决定
/usr/bin/snow_shot
/usr/share/applications/com.snowshot.snow_shot.desktop
/usr/share/metainfo/com.snowshot.snow_shot.metainfo.xml
/usr/share/icons/hicolor/scalable/apps/snow-shot.svg
```

#### 9.5.2 P6 slice 2：多尺寸图标 + AppImage（2026-10-21）

**决定 1：位图图标从 `app-icon.ico` 里抽，不在构建期光栅化 SVG。**

freedesktop 的图标查找**不会**从 `scalable/` 回退到固定尺寸以外的路径 —— 反过来说，
`Icon=snow-shot` 在 XFCE 面板、dde-dock、以及多数 AppImage 启动器里只查
`hicolor/<size>x<size>/apps/`，一个只有 `scalable/` 的包在那些位置显示通用占位图。
所以固定尺寸 PNG 不是可选项。

问题是怎么产出。构建期用 `rsvg-convert` / Inkscape / ImageMagick 光栅化看起来最"干净"，
实际是给每个发行版构建根引入一个不保证存在的工具依赖，而且**产出取决于碰巧装了哪个渲染器**
（librsvg 与 Inkscape 对同一份 SVG 的抗锯齿、渐变插值都不同）。而 `app-icon.ico` 里本来就
躺着设计师导出的九个尺寸位图，每个都是**完整的 PNG 数据流**（`struct` 解析就能确认，见
`scripts/gen-linux-icons.py` 的 `read_ico`）—— 于是"生成"退化成字节拷贝：零工具依赖、
零渲染差异、可逐字节复现。

- 只装 hicolor 规范列出的尺寸：`16/24/32/48/64/128/256`。源里还有 20 与 40，
  没有任何桌面环境按目录名查它们，装进去只是让 `dpkg -L` 变长。
- 512 缺失，由同一份 scalable SVG 覆盖（两个目录并存是规范的正常形态）。
- 脚本带 `--check`：把工作区与 ico **逐字节**比对，并核对每个 PNG 的 IHDR 边长与所在目录名一致。
  改 ico 不重跑脚本 ⇒ 门禁 FAIL，不会出现"七个尺寸里有一个是上一版"这种只在视觉上朦胧的现象。
- `read_ico` 遇到非 PNG 压缩的条目直接退出，而不是跳过：老式 32bpp DIB 条目要变成合法 PNG
  需要一个解码器加一个编码器，静默少处理一个尺寸比报错危险得多。
- CMake 侧用 `file(GLOB)` 枚举目录（安装规则跟随文件系统），"数量对不对"交给门禁断言，
  不用 GLOB 做判据。

**决定 2：AppImage 不做成 CPack 生成器，而是 AppDir + `appimagetool`。**

CPack 没有 AppImage 生成器。可选的两条路：`cpack -G External` 挂一个自定义脚本，
或者干脆自己 stage 一个 AppDir 再调 `appimagetool`。选后者 —— `External` 那条要把
"先 `cmake --install` 到临时前缀、再造 AppDir 根布局"这套逻辑塞进 CPack 的脚本钩子里，
换来的只是一层间接；而 AppDir 布局本身就是我们要验的东西，摊开写更好审。

模块照 P6 slice 1 的切法再分一层：

| 函数 | 性质 | 谁能跑 |
|---|---|---|
| `snow_shot_appimage_architecture()` | 纯映射 | `cmake -P`（本机门禁实测） |
| `snow_shot_appimage_layout()` | 纯计算 | 同上 |
| `snow_shot_add_appimage_target()` | 建 target、调 `appimagetool` | 只有真 configure + Linux |

**决定 3：AppImage 只做 x86_64 / aarch64，loong64 与 riscv64 显式拒绝。**

`appimagetool` 内嵌的 type-2 runtime 只有 x86_64 / i686 / armhf / aarch64 四个。给
loong64 生成一个没人能执行的文件，比在 configure 期 `FATAL_ERROR` 并提示"用 deb"糟糕得多。
门禁对 `sparc64` / `loongarch64` / `riscv64` 各跑一次子进程，断言退出码非零。

**决定 4：`AppRun` 里三件事必须自己修，`exec` 必须真的 exec。**

AppImage 把树挂到一个运行时才决定的路径上，装好的系统里那套隐式约定全部失效：

| 补什么 | 不补的症状 |
|---|---|
| `QT_PLUGIN_PATH=$APPDIR/usr/plugins` | `this application failed to start because no Qt platform plugin could be initialized` |
| `LD_LIBRARY_PATH=$APPDIR/usr/lib` | 只能用宿主库；宿主 Qt 版本不对就起不来 |
| `XDG_DATA_DIRS` 前置 `$APPDIR/usr/share` | 我们装的图标与 AppStream 文件永远查不到 |
| `exec`（而不是等子进程） | 托盘与 portal 的 bus 归属挂在包装进程的 pid 上，窗口被算到错误的 `.desktop` |

另外三条容易踩的：

- **`QT_QPA_PLATFORM_PLUGIN_PATH` 明确不设**。它是已废弃的单插件覆盖项，设了会盖住
  `QT_PLUGIN_PATH` 里除 xcb 之外的一切（图像格式、iconengine 全部失效）。门禁只认
  **赋值行**，不认注释里出现这个词 —— 脚本里正好有一段解释为什么不设它。
- **`APPDIR` 要自己兜底**。AppImage 运行时导出它，但解包后的 AppDir（`--appimage-extract`
  之后、以及打包过程中）没有，`readlink -f "$0"` 才能把符号链接的 `AppRun` 解析到 AppDir。
- **`APPIMAGE_EXTRACT_AND_RUN=1`**。没有 FUSE（容器、CI runner）时 `appimagetool` 不启动，
  除非告诉它解包自己的 runtime 再跑。
- **`.DirIcon`** 缺了不会报错，只会让文件管理器里的缩略图空白。

**决定 5：`INSTALL_RPATH=$ORIGIN/../lib`，但不 `INSTALL_RPATH_USE_LINK_PATH`。**

`AppRun` 的 `LD_LIBRARY_PATH` 已经够用，RPATH 是绕开 `AppRun` 直接用 AppDir 里二进制时的
第二道保险。关掉 `USE_LINK_PATH` 是必须的：开着会把**构建机上**的库路径（vcpkg / Qt 安装
目录的绝对路径）写进 RPATH，那台机器之外的路径不存在，等于往二进制里塞噪声。

**验证（本机可跑，不需要 Linux）**：`bash scripts/check-linux-packaging.sh`，三段 **101 项**全绿。

- `cmake -P scripts/check-linux-packaging.cmake`（原 43 项 → 含新段）：appimage 架构映射
  4 组、三个不支持处理器必须拒绝（子进程退出码）、文件名 / AppDir 前缀 / 根图标名、
  根 CMakeLists 真的调用了 `snow_shot_add_appimage_target()`。
- `scripts/check-linux-freedesktop.py`：新增 hicolor 树非空、PNG 文件名与 `Icon=` 同名。
- `scripts/gen-linux-icons.py --check`：7 个尺寸与 ico 逐字节一致 + IHDR 边长与目录名一致。
- `scripts/check-linux-appimage.py`（新，18 项）：`AppRun` 无 CRLF / 无 BOM / POSIX shebang、
  git 索引里是 `100755`、`sh -n` 与 `bash -n` 双解析、三个 export、废弃变量未被赋值、
  `APPDIR` 兜底、`exec` 唯一且指向 `usr/bin/snow_shot`、模块对 AppRun / `ARCH` /
  `APPIMAGE_EXTRACT_AND_RUN` / `.DirIcon` 的引用、根 CMakeLists 的 option 门控。
- **反向验证（5 点，全部命中）**：
  - 删 `48x48/…/snow-shot.png` ⇒ cmake 段 `FAIL missing hicolor icon` + 退出非零；
  - 把 `64x64` 截断到 100 字节 ⇒ `FAIL out of sync`（cmake 段的 PNG 签名检查**不**报错，
    因为前 8 字节还在 —— 两处判据互补，这正是为什么要两个都留）；
  - `AppRun` 注入 CRLF ⇒ `FAIL has no CRLF` + `FAIL POSIX shebang`；
  - 删掉 `export QT_PLUGIN_PATH=` 行 ⇒ `FAIL exports QT_PLUGIN_PATH`；
  - `git update-index --chmod=-x` ⇒ `FAIL executable in the git index: mode is 100644`。
- `windows-qt5-full` configure `EXIT=0`（`Configuring done` / `Generating done`）。

**一个值得记住的判据盲区**：CRLF 的 `AppRun` 在 `sh -n` / `bash -n` 下**语法检查照样通过**
——坏的是内核读 shebang 那一步（`bad interpreter: /bin/sh^M`）。语法门禁抓不到，必须单独比对
字节。这类"工具说没问题、运行环境说不"的检查，和 9.5.1 里 lrelease 对 `unfinished` 沉默是
同一类坑。

**本片未覆盖**：

- **AppImage 从未真正生成**：Windows 上没有 `appimagetool`。`snow_shot_add_appimage_target()`
  的每一条 `COMMAND` 都只在 Linux 上执行过 —— 这是本片最大的盲区，和 P5 的 NEON 同性质
  （语法正确 ≠ 跑过）。
- **`cmake --install --prefix` 与 `CPACK_PACKAGING_INSTALL_PREFIX=/usr` 的交互未验**：
  AppDir 用的是 `--prefix <AppDir>/usr`，CPack 用的是 `/usr`，两者靠命令行参数各自生效，
  但真机上是否如预期（尤其 `CMAKE_INSTALL_DATADIR` 的展开）只能实测。
- **`AppRun` 的三条 export 在真机是否足够**：Qt5 的 xcb 插件还依赖 `libxcb-*` 一族，这些
  是宿主提供的（AppImage 打不进 glibc 那一层，见附录 A.2），真机上缺哪个只能看
  `QT_DEBUG_PLUGINS=1` 的输出。
- 20x20 / 40x40 未安装（判断为无用目录），如果真机上发现某个面板确实查 20x20，加进
  `HICOLOR_SIZES` 重跑脚本即可 —— 代价是十行改动加一次提交。
- `.desktop` 的 `X-AppImage-Version`、`appimagetool` 的 `--updateinformation`（zsync 通道）
  未做：前者要有发布通道才谈得上，后者依赖托管地址。

产出（Linux 上，`-DSNOW_APPS_PACKAGE_APPIMAGE=ON`）：

```
snow-shot-1.2.8-x86_64.AppImage       # 或 -aarch64
<build>/appimage/snow_shot-1.2.8-x86_64.AppDir/   # 落盘中间产物，可直接跑
├── AppRun                            # 0755，POSIX sh
├── com.snowshot.snow_shot.desktop    # 与 usr/share/applications 里同名同内容
├── snow-shot.png                     # 256x256，名字与 Icon= 一致
├── .DirIcon -> snow-shot.png
└── usr/{bin,lib,plugins,share/...}
```

### 9.6 P7 slice 1：Linux CI（2026-10-21）

P6 剩下的每一条都是"只能在 Linux 上回答"。这台机器上能做的最后一件事，就是把那些问题变成
一个每天都会跑的 job。

#### 9.6.1 为什么分成三段，而不是一个 build job

| job | 跑什么 | 需要工具链 | 定位 |
|---|---|---|---|
| `gates` | `scripts/check-linux-packaging.sh`（118 项）+ `cargo test -p snow-linux -p snow-capture --all-features` + 双 Linux 目标 `cargo check` | 否 | **PR 必过**。这一段的每一条在本机 Windows 上都已经跑绿过，所以红了就是改动错了，不是环境缺东西 |
| `build` × {x64, arm64} | apt 装 §9.1 依赖 → `cmake --preset` → build → ctest → `cpack` → 拆包校验 → `dpkg-deb -x` + 两个 freedesktop 校验器 | 是 | **唯一真正编译 aarch64 C++ 的地方** |
| AppImage | — | — | 刻意不做，理由见 9.6.3 |

把 `gates` 独立出来的理由不是速度，是**归因**：一个同时装 Qt、编 Rust、跑打包校验的 job 红了，
第一眼分不清是代码错还是环境错。分开之后，`gates` 绿而 `build` 红，几乎必然是"这棵树第一次
真的在 Linux 上编"这类问题；两边都红，才先看 `gates`。

#### 9.6.2 `build` 补掉的盲区

这是本移植里第一次有东西能真正回答下面这些问题：

1. **aarch64 C++ 到底编不编得过**。§7.3 那条结论（本机没有 aarch64 C++ 编译器 ⇒
   `#if defined(__aarch64__)` 里的代码从未被解析）在这里终结 —— 不是靠人审，是靠
   `ubuntu-24.04-arm` 上的一次 `cmake --build`。
2. **`--all-features` 在 Linux 上是否还是好的**。P5 slice 1 修过一次（`stage-timing` 的 QPC
   re-export 与 dead-code），修完就没有任何东西持续验证它；现在 `gates` 每次 PR 都跑。
3. **`dpkg-shlibdeps` 算出来的 `Depends` 到底是什么**。§9.5.1 决定 1 的整个论证建立在
   "它从 ELF 的 NEEDED 反推"上，但从没看过输出。job 里 `dpkg-deb -f "$deb" Depends` 打出来，
   并且断言里面有 `libqt5` —— 如果 `Depends` 是空的（`SHLIBDEPS` 没生效），这一步会红。
4. **两个 freedesktop 校验器**。`desktop-file-validate` 与 `appstreamcli validate --no-net`
   —— §9.5.1 说"到真机的第一条命令就是这两个"，现在它们在 CI 里，而 `/dev/null` 不是它们的
   输出终点：非零退出就是 job 失败。
5. **`.deb` 的实际布局**。不只是"文件存在"，而是 `dpkg-deb -c` 的输出里逐条 grep：
   `/usr/bin/snow_shot`、入口、metainfo、**七个 hicolor 尺寸加 scalable**。这一步把 9.5.2 的
   图标决定从"仓库里有这七个文件"变成"包里真的有这七个文件"。
6. **`Architecture:` 与 runner 一致**。矩阵里 x64 对应 `amd64`、arm64 对应 `arm64`，
   job 把 `dpkg-deb -f Architecture` 读回来 `grep -qx`。架构名映射错了（P6 slice 1 的正向/
   反向验证覆盖的是函数，这里覆盖的是产物）。

#### 9.6.3 为什么 AppImage 不在这一片里

`appimagetool` 不是发行版包，要从第三方 release 下载。**那个 URL 在本机无法验证** —— 写进
workflow 意味着第一次跑很可能是 404，而 404 会把 `build` job 真正的编译错误盖掉。先让
`build` 绿一次，再往里加 AppImage。这是"宁可少一条路径，不要一条会把别的失败伪装的路径"。

#### 9.6.4 门禁：`scripts/check-linux-ci-workflow.py`（17 项）

workflow 是仓库里唯一"推送之前没人解析"的文件。它的问题读起来全是合法 YAML：

- **preset 名拼错**。`cmake --preset` 用的都是 `${{ matrix.preset }}`，真实名字只在 matrix 段
  出现一次。门禁把 matrix 里的 `preset` / `build-preset` 逐个对照 `CMakePresets.json` 校验，
  并断言 configure preset 不是 `hidden`（隐藏的不能按名配置）。
- **`gates` job 里根本没有调门禁脚本**。job 存在、脚本存在、两者之间没有连线 —— 这种"看起来
  有 CI"的状态最难发现。门禁断言 `gates` 的 `run:` 里确实出现
  `scripts/check-linux-packaging.sh`。
- **漏 `timeout-minutes`**。一次挂住的 Qt configure 会烧满 GitHub 六小时预算。
- **runner 与架构对不上**。`RUNNER_ARCHITECTURES` 表把 `ubuntu-24.04` 映射到 `amd64`、
  `ubuntu-24.04-arm` 映射到 `arm64`，与 job 里读回 `.deb` 的那条断言是同源判据；换 runner
  不更新表 ⇒ 门禁 FAIL，而不是"断言被静默跳过"。
- **apt 列表里重复的包**（通常意味着一次改错的复制粘贴）。

**反向验证（3 点，全部命中）**：

- 把 `preset: snow-shot-linux-x64-release` 改成 `...-relase` ⇒
  `FAIL build: preset '...-relase' exists in CMakePresets.json`。
  （`matches architecture` 那条**不**报错，因为 `-x64-` 还在 —— 两个判据覆盖的是不同错误，
  这正是两条都留的原因。）
- 把 `run: bash scripts/check-linux-packaging.sh` 换成 `run: echo skipped` ⇒
  `FAIL gates job runs the packaging gates`。
- 删掉 `build` 的 `timeout-minutes` ⇒ `FAIL build: sets a timeout`。

#### 9.6.5 一个工具坑：`${{ }}` 与跨行匹配

第一版门禁把 `cmake --preset ${{ matrix.preset }}` 里的表达式替换成空串再匹配，结果报
`configure preset 'cmake' exists: not in CMakePresets.json` —— 因为替换后是
`cmake --preset `，而 `\s+` **能吃掉换行**，于是下一行的 `cmake --build ...` 的第一个词
被当成了 preset 名。两处修：`\s+` 改 `[ \t]+`，名字字符类首字符排除 `-`（选项）。
教训与 9.5.2 的 CRLF 同源：**正则里的空白类默认包含换行，在多行输入上就是 bug**。

#### 9.6.6 尚未验证（这一片最重要的部分）

**这个 workflow 从未在 GitHub 上跑过。** 本机能做的最多是：YAML 能解析、每个引用都能在
checkout 里解析到、`windows-qt5-full` configure 仍然 EXIT=0。以下没有一条有答案：

- `ubuntu-24.04` 上按 §9.1 装完包后，`cmake --preset snow-shot-linux-x64-release` 是否配置成功
  —— 包名在 24.04 与 UOS 20 之间有差异（`libjxl-dev` 在 24.04 是 `libjxl-dev`，UOS 20 可能叫
  别的；`libopencv-dev` 的版本差得更多）。**这是最可能的第一个红灯。**
- 第一次 Linux 编译会不会撞上 Qt5 与 Qt6 的头文件/cfg 差异 —— 本机只编过 Windows。
- `ctest` 在 Linux 上跑起来时，有多少测试是 Windows-only 被条件排除的、有多少真的会跑。
- `appstreamcli validate` 对 `<id>snow-shot.desktop</id>` 的评级。AppStream 规范要求反向域名
  id，而我们按 §9.5.1 决定 4 必须用 `productId()`（`snow-shot`）。**预期它会给出 warning 而
  非 error（`--pedantic` 才会升为失败）**，但这是推断，没跑过。如果它其实报 error，就得在
  "文件名与 productId 同源"和"AppStream 合规"之间做取舍 —— 那是一个需要重新论证的决定，
  不是一个可以打补丁的地方。
  → **这条推断错了，而且错在最关键的那半句。** run 42 的 `appstreamcli validate` 给出
  `W: cid-desktopapp-is-not-rdns` 并 **exit 3**：warning 本身就使命令失败，不需要 `--pedantic`。
  于是取舍变成了"改名字"，而不是"放宽校验"。见 9.6.41。
- `ubuntu-24.04-arm` runner 在私有仓库/免费额度下是否可用。

**下一步**：把这个 workflow 推上去跑一次，按红的地方改。红是预期输出，不是失败。

#### 9.6.7 一个必须先解决的硬事实：仓库在 Gitee，不在 GitHub（2026-10-21 发现）

`git remote -v` 只有一个 `origin`：`https://gitee.com/zhaoweifree/snow-apps.git`。GitHub Actions
的 workflow 文件放在 `.github/workflows/` 下，推到 Gitee **一行都不会执行** —— 它既不会被解析，
也不会报错，页面上什么都没有。9.6.1–9.6.6 描述的一切仍然成立，但"推上去跑一次"这个动作在
当前远程上不存在。

三条出路，成本与收益完全不同：

| 方案 | 代价 | 换到什么 |
|---|---|---|
| 镜像到 GitHub，在那里跑 Actions | 一个 mirror remote + 凭据；仓库若含大文件需注意 LFS/配额 | 9.6 的 workflow **原样可用**，含 `ubuntu-24.04-arm`（公开仓库免费） |
| 自建 runner（一台 amd64 + 一台 UOS/SBC arm64），Gitee 侧只做触发 | 一台常开机器 + 环境维护；arm64 那台顺便能当日用机真机验证 | 不依赖任何第三方 CI，且 arm64 上是**真 UOS**，比 GitHub 的 ubuntu-arm 更接近交付目标 |
| Gitee Go / 其它国内流水线 | 重写 workflow（YAML 方言不同），`gates` 那套断言可复用但 job 语法要重打 | 不需要外部账号 |

**推荐第二条的后半句**：9.4.4 / 9.4.19 / 9.4.20 三片累积下来的"从未在真机跑过"清单（XInput2
投递、Damage 覆盖面、portal 握手、NEON 执行）本来就是一台 UOS arm64 机器一次性可以清掉的东西，
CI 只是顺带的产物。在那台机器到手之前，GitHub 镜像跑 `gates` 也有独立价值：它能证明
`cargo test --all-features` 与 118 项打包门禁在 Linux 上真的过。

这一条不改变 9.4.20 的结论，但改变它的验证顺序：**portal 的第一处真实信号，不会来自 CI**。

#### 9.6.8 方案①落地：GitHub 镜像，workflow 第一次真的跑起来（2026-10-21）

选了 9.6.7 表里的第一行，因为它是唯一当天就能给出答案的：`zhaoweifree/snow-apps`（**public**，
`ubuntu-24.04-arm` 只在公开仓库免费），本地加了一个 `github` 远程，`origin` 仍然是 Gitee。

| 动作 | 内容 |
|---|---|
| 远程 | `origin` = Gitee（日常推送目标，未变）、`github` = 镜像 + 构建农场 |
| `mirror-gitee.yml` | 触发从 `push: branches: [main]` 改为 `workflow_dispatch`。方向反了：镜像推送会推回刚拉下来的那个远程，且仓库没有 `GITEE_TOKEN` 密钥 ⇒ 每次推送必然多一个红 job，和真正要看的 `build` 混在一起 |
| 读取工具 | `.workbuddy/uos-port/gh-ci.py`（`runs` / `jobs` / `watch` / `logs` / `dispatch`）。凭据复用 `git credential fill`，不落盘、不需要装 `gh` |
| 触发 | `push` 到 `github` 就够；第一次 `workflow_dispatch` 是多余的（push 事件的 run 注册有几十秒延迟，看起来像"没触发"） |

**三次运行，三个真红灯**（每一个都是本机永远看不到的）：

| run | 结果 | 第一个红灯 |
|---|---|---|
| 1（`5ddc3561`） | `gates` ✅ / `build` ❌ | `E: Unable to locate package libxrecord1-dev`（apt exit 100，两个架构同时倒） |
| 2（`ce0efc10`） | `gates` ✅ / `build` ❌ | `Could not read presets: "cmakeMinimumRequired" major version 4 must be less than 3` —— 24.04 自带 CMake 3.28 |
| 3（`2c55e097`） | `gates` ✅ / `build` ❌ | `snow_image/CMakeLists.txt:141: PNG support requires zlib-ng compatibility mode` |
| 4（`411aca6c`） | `gates` ✅ / `build` ❌ | `find_package(lunasvg CONFIG REQUIRED)` 找不到包 —— 但**前两步全绿**：CMake 4.2 装好、`build-linux-deps.sh` 在 Linux 上第一次运行就把 zlib-ng 与 libjpeg-turbo 编过并装进前缀（PNG/JPEG 两道关都过了） |
| 5（`a242facc`） | `gates` ✅ / `build` ❌ | `Qt5LinguistTools` 找不到 —— `qttools5-dev-tools` 只给 `lrelease` 二进制，CMake 配置在 `qttools5-dev` 里 |
| 6（`eb0a2c80`） | `gates` ✅ / `build` ❌ | `find_package(onnxruntime CONFIG REQUIRED)` —— SVG/lunasvg 之后一路走到 `snow_shot`，说明 `snow_image` 的 PNG/JPEG/GIF/HEIF/JXL/WebP 依赖全部找到了 |
| 7（`60302201`） | `gates` ✅ / `build` ❌ | `SnowShotTranslationRelease.cmake:59`：找不到 `zh_CN` 的 stock-dialog 目录，报的路径是 `/usr/lib/translations` —— **一个在 Debian 系上根本不存在的目录**，见下 |
| 8（`7fe30ad0`） | `gates` ✅ / `build` ❌ | `SnowQtVersionShims.cmake:58 (include)`：找不到 `ant_design_qt/SnowQt5InstallPaths.cmake` —— **上一片自己新写的代码**，`CMAKE_CURRENT_LIST_DIR` 在函数体里指向调用方，见 9.6.10 |
| 9（`7f42249d`） | `gates` ✅ / `build` ❌ | 翻译目录那关过了；卡在 `ant_design_qt/CMakeLists.txt:106` 与 `snow_shot/CMakeLists.txt:10577`：`Target links to: Qt5::GuiPrivate` —— 缺的不是代码是**私有头**，见 9.6.11 |
| 10（`2eab250d`） | `gates` ✅ / **`Configure` ✅** / `build` ❌ | **整棵树的 Linux + Qt5 配置第一次走通**；`Build` 在 7 秒内倒，`snow_image` 的 EXR/WebP 各报一处 API 不存在 —— 发行版版本比 vcpkg 的 pin 老一代，见 9.6.12 |
| 11（`c03eacf3`） | `gates` ✅ / `Configure` ✅ / `build` ❌ | 自编的 Imath/OpenEXR/libwebp 全过，EXR/WebP 报错消失；换成 `libheif/heif_sequences.h` 缺失与 JXL 0.7 的四个 API —— 同一条规则再扩到 highway/libjxl/libheif，见 9.6.13 |
| 12（`2e10f9d1`） | `gates` ✅ / **deps 步骤 ❌** | 依赖构建这一步自己倒了，而且**什么都没打印**（只有 `Process completed with exit code 1`）—— `die` 的消息被重定向进了日志文件。同时暴露 `HWY_ENABLE_INSTALL` 缺失，见 9.6.14 |
| 13（`9997a16b`） | `gates` ✅ / **`Configure` ✅** / **deps ✅** / `build` ❌ | 固定版本自编的依赖在 Linux 上第一次全绿（`snow_image` 静态库链接成功，内含 jxl/heif 编解码器）；红灯移到 **Rust FFI 的 lockfile**：`snow_rust_ffi/Cargo.lock` 早于 AT-SPI2 重构，`--locked` 拒绝更新，见 9.6.15 |
| 14（`39707846`） | `gates` ✅ / **`Configure` ✅** / **deps ✅** / `build` ❌ | lockfile 修好后红灯落到 **Rust FFI 的 FFmpeg 绑定**：`libavfilter/avfilter.h: No such file or directory` —— 错误指向头文件，实际是**搜索路径被 `FFMPEG_DIR` 劫持**，见 9.6.16 |
| 15（`39715098`） | `gates` ✅ / `Configure` ✅ / **deps ✅** / **build ❌** | FFmpeg 修好后红灯回到 **C++ 侧**，而且落在**被 force-include 进每个 TU 的兼容头**里：`-Wundef`（版本宏先测后定义）+ 4 处 `-Wsign-conversion`（掩码跨有符号/无符号域）。**x64 侧是代码缺陷**，**arm64 侧是 rustup 组件冲突的环境 flake**，见 9.6.17 |
| 16（`39734199`） | `gates` ✅ | `qt_compat.h` 修好后红灯首次越过兼容头；`gates` 一次全绿（含新增门禁）。**x64 的 Build 被新 push 顶掉（concurrency），没跑完** ⇒ build 结果实际来自 run 17，见 9.6.18 |
| 17（`39736061`） | `gates` ✅ / `Configure` ✅ / **build ❌** | 红灯首次越过兼容头、落在 **`QMimeData::retrieveData` 的虚函数签名**上：Qt5 收 `QVariant::Type`、Qt6 收 `QMetaType`，同一份代码写不了两边 ⇒ 见 9.6.19 |
| 18（`39745852`） | `gates` ✅ / `Configure` ✅ / **build ❌** | 红灯落在 **OpenCV 的两处 API 缺口**上：`cv::barcode` 在发行版的 4.6 里根本不存在（它 4.9 才进主仓 `objdetect`），`WeChatQRCode` 的 `vector<uchar>` 构造是**本项目自打的补丁**、上游任何版本都没有 ⇒ 见 9.6.20 |

**run 7 暴露的是本工程的第一个"只在 multiarch 布局下错"的 bug**，不是缺包。Qt5 装在自定义前缀时
`Qt5Core_DIR` 是 `<prefix>/lib/cmake/Qt5Core`，`../../../bin` 正好是 `<prefix>/bin`；但发行版的同名文件
在 `/usr/lib/<triple>/cmake/Qt5Core`，`../../../` 落在 `/usr/lib`，于是工具目录被算成 `/usr/lib/bin`、
翻译目录被算成 `/usr/lib/translations`（真实位置是 `/usr/share/qt5/translations`）。
`cmake/SnowQtVersionShims.cmake` 里那份 lrelease 目录推导是同一个错误 —— 两处都会在下一步才炸。
修法：

1. 新增 `cmake/SnowQt5InstallPaths.cmake`，两个解析器（工具目录 / 翻译目录）。顺序是
   **显式覆盖 → 由 `Qt5Core_DIR` 推的两种形状（前缀形、multiarch 形）→ `qmake -query`**。
   `qmake` 排最后是刻意的：PATH 上的 qmake 可能属于另一个 Qt，它不该压过"从实际找到的那个
   Qt5 包推出来的路径"。工具目录的判据不是"目录存在"而是"目录里真的有 `lrelease`"。
2. `SnowShotTranslationRelease.cmake` 与 `SnowQtVersionShims.cmake` 都改用它，不再各自推导。
3. 新增 `scripts/check-qt5-install-paths.cmake`，**在 Windows 上**用假目录建出两种布局并断言解析结果，
   已接进 `scripts/check-linux-packaging.sh`。反向验证两次：删掉 multiarch 的翻译分支 ⇒ 翻译用例红；
   删掉 multiarch 的工具分支 ⇒ 工具用例红（各自只让对应用例红）。
   这个检查自己也被反向验证抓过一次 —— 第一版把传进来的**值**又当变量名解引用了一次
   （`${${actual}}`），路径因此全变成空串，于是"空 == 空"永远通过。**假门禁的第二例**
   （第一例是 PyYAML 那一处静默跳过）。
4. `qttranslations5-l10n` 补进 apt 清单（翻译目录存在的前提），§9.1 同步。

**`gates` 这一次把"平台无关"从声明变成了事实**：ubuntu-24.04 上
`cargo test -p snow-linux -p snow-capture --lib --all-features` 全绿 —— `snow-capture` 135 通过 + 4 ignored
（共 139），`snow-linux` 232 通过；`cargo check --target {x86_64,aarch64}-unknown-linux-gnu --all-features`
两个目标都过。`snow-linux` 在本机是 233 个，差的那一个是
`text::raster::tests::a_real_installed_font_tree_renders`，`#[cfg(windows)]` —— 它读的是 Windows 字体树，
本来就不该在 Linux 上跑。这条差异以前只能靠读代码推断，现在有两个平台的数字对得上。

**顺带发现的两个真问题（都在本机跑不出来）**：

1. **门禁假绿**。`scripts/check-linux-ci-workflow.py` 缺 PyYAML 时打印一句 note 就 `return None`，
   于是"YAML 能解析""声明了三个 job"等三条检查**从来没在任何地方执行过**，而 job 依然绿。
   已改为硬失败，`gates` 里显式 `pip install pyyaml`，并加一条门禁断言 gates 确实装了它。
   这一类"静默降级"和 9.5 里 `qt_add_lrelease` 垫片是同一个模式。
2. **`libxrecord1-dev` 不存在**。§9.1 清单里猜的包名；源码里 `extensions/record.h`、`XRecordQuery`、
   `libxrecord` 在 `.c/.h/.cpp/.rs/.cmake` 里**零命中**。已从清单和 workflow 一起删除。

**CMake 4.2 是硬前置，不是 CI 细节**。根 `CMakeLists.txt` 与 `CMakePresets.json` 都写着 4.2，
24.04 的 3.28 连 preset 文件都解析不了，错误发生在**读文件阶段**，一行项目代码都还没进去；
UOS 20（Debian 10）自带 3.13，问题只会更早。装法（两种架构通用，不碰系统包管理器）：

```bash
python3 -m venv ~/.local/cmake && ~/.local/cmake/bin/pip install "cmake>=4.2"
export PATH="$HOME/.local/cmake/bin:$PATH"
```

**下一个阻塞要一个决定，不是一个补丁**（run 3 的结论）：PNG 路径要求 `ZLIBNG_VERSION`，
`snow_image/src/compression_backend.cpp` 里还有一条 `#error`，`core_tests.cpp` 断言版本串含 `zlib-ng`；
而 Ubuntu 只有经典 zlib。与此同类的还有一批发行版**没有 CONFIG 包**的依赖：
`libjpeg-turbo 3.1 CONFIG`（24.04 是 2.1.5 且没有 CMake config）、`lunasvg`、`minizip-ng`、
`unofficial-brotli`、`hwy`、`onnxruntime`。Windows/macOS 侧这些全部由 `cmake/vcpkg-overlay-ports/` 提供。
Linux 侧怎么补这一批（vcpkg / 固定版本本地编 / 砍掉可选编解码器），是 §9.2「Linux 不用 vcpkg」这条
决策的第一次真正压力测试，**留给下一片**，见 9.6.9。

#### 9.6.9 决定：Linux 侧第三方库「固定版本本地编」，不进 vcpkg（2026-10-21）

| 方案 | 代价 | 结论 |
|---|---|---|
| Linux 也用 vcpkg | 复用现成 overlay ports，版本与 Windows 同源；但要在两种架构上各编 ~15 个库（ffmpeg / opencv / jxl / heif / onnxruntime），小时级；且本机已实测 vcpkg 驱动会随机挂死子进程 | **不选** |
| 放宽 PNG 门槛 | 删 `compression_backend.cpp` 的 `#error` 与 `core_tests` 的版本断言，Linux 用经典 zlib | **不选**：这是一条产品保证（`THIRD_PARTY_NOTICES` 里就写着随包发布 zlib-ng），不是构建细节 |
| **固定版本本地编** | 多一个 `scripts/build-linux-deps.sh` + 一张版本表 | **选中** |

`scripts/build-linux-deps.sh` 的四条设计：

1. **版本只有一个真源**。`zlib` 的版本从 `cmake/vcpkg-overlay-ports/zlib/vcpkg.json` 读、校验和从
   同目录 `portfile.cmake` 的 `SHA512` 读 —— 改 port 就同时改了 Linux。实测在 Windows 上下载的
   `zlib-ng-2.3.3.tar.gz`，sha512 **与 portfile 里的值逐字节相同**，所以"两边是同一个包"是验证过的，
   不是假设。没有 overlay port 的（`libjpeg-turbo 3.1.0`）pin 写在脚本里，带校验和。
2. **CMake 选项照抄 portfile**，包括 `WITH_NATIVE_INSTRUCTIONS=OFF`：port 关掉 `-march=native`，
   否则在 CI runner 上编出来的库拿到客户机上跑会 `SIGILL`。
3. **判据是产品要求本身**，不是"命令返回 0"：装完必须 `grep ZLIBNG_VERSION include/zlib.h` 命中；
   `libjpeg-turbo` 必须真的落下 `libjpeg-turboConfig.cmake`（`find_package(... CONFIG REQUIRED)`
   读的就是它）。
4. **用系统 cmake 编这两个库**（`CMAKE=` 可覆盖）。它们的 `cmake_minimum_required` 早于 3.5，
   CMake 4 会直接拒绝 —— 所以 workflow 里这一步排在"装 CMake 4.2"**之前**：需要 4.2 的是仓库自己的
   configure（9.6.8），不是这两个第三方项目。

离线 / 内网：把 tarball 放进 `<prefix>/src` 或指向 `SNOW_LINUX_DEPS_SRC`，脚本不再联网。

**这一片没解决的问题**：

- `minizip-ng` 与 `onnxruntime` 已按同一条路子接上（见下），还剩 `unofficial-brotli` 与 `hwy`/`lcms2`
  —— 后三个只在 pkg-config 失败时才会被 `snow_image` 要求（非 Windows 上优先 `pkg_check_modules(JXL)`，
  `libjxl-dev` 提供 `libjxl.pc`），所以先不预先加。
- **`onnxruntime` 走的是官方预编译包，没有校验和**。端口文档 §3/§9.3 本来就是这么定的（CPU EP +
  `libonnxruntime.so`），但微软不发布校验和，而这台机器上 9 MB 的 release 资产拉不动（代理在 300 KB
  处停住）。所以脚本的 pin 是"版本 + 资产名"，第一次使用时把 sha512 **打印出来**，然后回填到
  `onnxruntime_sha512_{x64,aarch64}`；回填之前它是一处诚实的弱点，不是一处假装存在的检查。
  另外预编译包里没有 CMake 配置，脚本生成一个只含 `onnxruntime::onnxruntime` 的
  `onnxruntimeConfig.cmake`（路径按自身位置推导，prefix 可搬）。
- **运行时链接谁，还没定**。若可执行文件链接的是 `$prefix` 里的 `libz.so.1`，`dpkg-shlibdeps` 会按
  soname 把它算成 `zlib1g`；zlib-ng 的 compat 模式保持 zlib ABI，所以装到用户机上实际加载的是发行版
  zlib，`compression_backend_version()` 就不再说 zlib-ng 了。要么静态链进包，要么随包带 `libz-ng`
  并设 rpath —— 这是打包决定，等 `build` 绿了再定。`libonnxruntime.so` 同理：目前没有任何一步把它
  放进 `.deb`。
- `build` job 里的 `ctest` 原来是空的：release 预设 `SNOW_APPS_BUILD_TESTS=OFF`，`--no-tests=error`
  会把"一个测试都没有"报成失败。已改成**打包之后**再在同一个树里打开测试编一遍（增量，只多测试目标），
  理由写在 workflow 注释里：先让 .deb 那条路证明自己，别让测试编译错误把它藏起来。

#### 9.6.10 run 8：`CMAKE_CURRENT_LIST_DIR` 在函数体里指向调用方（2026-10-21）

run 8（`7fe30ad0`）的红灯不是缺包，而是**上一节自己新写的代码**：

```
CMake Error at cmake/SnowQtVersionShims.cmake:58 (include):
  include could not find requested file:
    .../ant_design_qt/SnowQt5InstallPaths.cmake
```

multiarch 修复引入了两处 `include("${CMAKE_CURRENT_LIST_DIR}/SnowQt5InstallPaths.cmake")`，**都写在
function 体内**。`CMAKE_CURRENT_LIST_DIR` 报告的是"当前正在处理的 listfile 的目录"，而函数体在**调用点**
展开 —— 于是它给出的是**调用方**的目录。`_snow_qt_shim_bin_dir()` 由 `ant_design_qt/CMakeLists.txt`
调用，"本文件旁边那个 .cmake"就成了 `ant_design_qt/SnowQt5InstallPaths.cmake`；
`snow_shot_release_translation_catalogs()` 同病（会去找 `snow_shot/SnowQt5InstallPaths.cmake`），
只是被前一个挡住，还没轮到它。

`cmake/SnowQt.cmake:26` 早就为 macro 记过这个坑并固化了 `SNOW_QT_CMAKE_DIR`，这两处没照做。修法统一：
**文件作用域求值一次，函数里只引用变量**（`SNOW_QT5_INSTALL_PATHS_MODULE`）。

**为什么本机七轮门禁全绿**：`scripts/check-qt5-install-paths.cmake` 是 `cmake -P` 夹具，两个解析器都在
**文件作用域**被调用 —— 调用方语义根本不会出现。这与"探针只能覆盖它真跑过的路径"是同一句话，只是缺口
在语义层（**谁**在调）而不是路径层（有没有被调）。补法是**文本门禁**
`scripts/check-cmake-list-dir-scope.py`：

- 扫 `cmake/` 下 74 个 `*.cmake`，跟踪 `function()` / `macro()` 嵌套深度，函数/宏体内出现
  `CMAKE_CURRENT_LIST_DIR` 或 `CMAKE_CURRENT_LIST_FILE` 即报红；
- 注释不算（这两个文件里恰好有讲解该坑的注释），`CMAKE_CURRENT_FUNCTION_LIST_DIR`（CMake ≥ 3.17，
  按定义绑定**定义文件**）放行；
- 已接进 `scripts/check-linux-packaging.sh`。

反向验证：对临时根注入 function 内的一处用法 ⇒ 报红 `exit=1`；同一文件中"文件作用域的那处 / 注释 /
`CMAKE_CURRENT_FUNCTION_LIST_DIR`"三处都不误报。当前树 74 个模块，0 违规。

**可复用的判据**：只要"我的目录"与"调用者的目录"是两个不同概念，就必须在文件作用域求值。CMake 里
`CMAKE_CURRENT_LIST_DIR` / `CMAKE_CURRENT_LIST_FILE` 属于**调用方**，`CMAKE_CURRENT_FUNCTION_LIST_DIR`
属于**定义方** —— `__FILE__` 风格的直觉在这里正好是反的。

**遗留的验证缺口（诚实记一笔）**：这次只有 CI 能证明修复有效，本机没有任何一步会配置 `snow_shot` 的
Qt5 路线（`check-qt5-install-paths.cmake` 只跑解析器，`windows-qt5-smoke` 只开
`ant_design_qt` + `snow_image`），所以"本机全绿"对这类 bug **不构成证据**。文本门禁把这一类挡在门口，
但它挡的是写法，不是行为；行为仍只能由一次真实 configure 证明。

#### 9.6.11 run 9：Qt5 私有头是独立包，`Qt5::GuiPrivate` 因此不存在（2026-10-21）

run 9（`7f42249d`）把翻译目录那关过了（`qt_add_lrelease` 走打包侧合并、`qtbase` 目录找到），
下一个红灯是：

```
CMake Error at ant_design_qt/CMakeLists.txt:106 (target_link_libraries):
  Target "ant_design_qt" links to:
    Qt5::GuiPrivate
CMake Error at snow_shot/CMakeLists.txt:10577 (target_link_libraries):
  Target "snow_shot" links to:
    Qt5::GuiPrivate
```

**根因不是链接写错，是包没装全**：Debian/Ubuntu 的 `qtbase5-dev` 只给公开头，私有头在
`qtbase5-private-dev`（`<include>/QtGui/5.15.13/{private,qpa}`）。`Qt5GuiConfig.cmake` 在私有头缺席时
**根本不建** `Qt5::GuiPrivate`（官方安装包自带私有头，所以 Windows 上这个目标一直在）。而源码是真的要：

| 位置 | 用到的私有头 |
|---|---|
| `ant_design_qt/.../widgets/detail/window_modality.cpp` | `<QtGui/private/qguiapplication_p.h>` |
| `ant_design_qt/.../widgets/platform_compatibility.cpp` | `<QtGui/private/qaccessiblecache_p.h>` |
| `snow_shot/src/platform/physicalcursor.cpp` | `<qpa/qwindowsysteminterface.h>`、`<private/qhighdpiscaling_p.h>` |
| `snow_shot/tests/global_canvas_tests.cpp` 等 | `<qpa/qplatformscreen.h>`、`<private/qwindow_p.h>` |

修法是三层，缺一层就会以别处报错的形式回来：

1. **装包**：`qtbase5-private-dev` 进 workflow 的 apt 清单与 §9.1。`check-linux-ci-workflow.py` 加一条
   断言，防止这条被"精简依赖"时删掉。
2. **解析器补位**：新增 `snow_qt5_private_include_dirs(<out> <module>)`
   （`cmake/SnowQt5InstallPaths.cmake`）。顺序是 **套件自带变量 `Qt5<Module>_PRIVATE_INCLUDE_DIRS` →
   `SNOW_QT5_HEADERS_DIR`（新增覆盖）或 `qmake -query QT_INSTALL_HEADERS` 下 glob `Qt<Module>/<ver>/`**；
   接受判据是"目录里真有 `private`/`qpa`"。两个目录通常都要：版本目录给 `<QtGui/private/…>`，
   它的 `QtGui` 子目录给 `<qpa/…>`、`<private/…>`。
   路径上有个易错点：头树是 `Qt<Module>/`，变量是 `Qt5<Module>_` —— **`Qt` 前缀只存在于路径里**，
   第一版就写成 `${module}` 被本地夹具当场抓住。
3. **补目标而不是改 19 处调用点**：`snow_find_qt_private` 在 Qt5 分支发现目标不存在时，用解析结果
   `add_library(Qt5::<Module>Private INTERFACE IMPORTED GLOBAL)` 造一个同名目标
   （`INTERFACE_INCLUDE_DIRECTORIES` + `INTERFACE_LINK_LIBRARIES Qt5::<Module>`）。
   解析为空 ⇒ **`FATAL_ERROR` 并点名该装的包**，不做"先编着看"的降级 —— 私有头真缺时
   `qpa/` 那批 include 过不去。

本机判据（`scripts/check-qt5-install-paths.cmake`，全部在 Windows 上跑）：

- 伪造 `…/qt5/QtGui/5.15.13/{private,QtGui/qpa}` ⇒ 两个目录按"版本目录在前"返回；
- 套件自带 `Qt5Gui_PRIVATE_INCLUDE_DIRS` 存在时**压过** glob（上游导出值不该被反推值覆盖）；
- 两头都没有 ⇒ 返回空（调用方据此报错）。
- 反向验证：把路径前缀从 `Qt${module}` 改回 `${module}` ⇒ 用例 1 变红。

另加一条**配对**门禁 `scripts/check-qt-private-modules.py`：凡在某个 `CMakeLists.txt` 里
`target_link_libraries(... ${SNOW_QT_NS}::<M>Private)` 的，同一文件必须调用 `snow_find_qt_private(<M>)`
—— 因为 Qt5 上"目标存在"完全取决于那次调用。当前 3 个文件（`ant_design_qt` / `snow_image_viewer` /
`snow_shot`）都配对；拿掉一处调用即报红。

#### 9.6.12 run 10：Configure 通了，Build 倒在"发行版版本太老"（2026-10-21）

run 10（`2eab250d`）是这一片的分水岭：**`: Configure` 第一次整棵绿**（4.1 秒），
`-- Snow Qt: Qt5 private includes for Gui applied to this directory: …/QtGui/5.15.13;…/5.15.13/QtGui`
也在日志里 —— 9.6.11 那条路真的通了。接着 `Build` 7 秒就停：

```
snow_image/src/codecs/exr_codec.cpp:82: error: 'int64_t MemoryIStream::size()' marked 'override',
                                               but does not override
snow_image/src/codecs/exr_codec.cpp:886: error: no matching function for call to
                                               'Imf_3_1::OpaqueAttribute::OpaqueAttribute(const char*, long int, const std::byte*)'
snow_image/src/codecs/webp_codec.cpp:1489: error: 'WebPAnimEncoderSetChunk' was not declared in this scope
```

**这不是"缺包"，是版本代差**。代码是对着 vcpkg 的 pin 写的：

| 库 | vcpkg（Windows 用的） | Ubuntu 24.04 | UOS 20（目标机） |
|---|---|---|---|
| OpenEXR | 3.4.9 | 3.1.5（`size()` 签名不同、`OpaqueAttribute` 无三参构造） | 2.5（连异常命名空间都不一样） |
| Imath | 3.2.2 | 3.1.1 | 无独立包 |
| libwebp | 1.6.0 | 1.3.2（无 `WebPAnimEncoderSetChunk`） | 0.6（没有动画编码器） |

9.6.9 定的「固定版本本地编」原本只针对「发行版没有 CONFIG 包」那一批；这里把同一条规则
**显式扩展**到「版本不足 / API 不兼容」：判据仍是产品要求本身 —— 目标机 UOS 20 上这两个库比
24.04 还老，靠"适配老 API"永远到不了能发货的状态，只能自己带版本。

`scripts/build-linux-deps.sh` 新增三个 builder（`imath` → `openexr` → `libwebp`，顺序即依赖）：

- 选项照抄 portfile：OpenEXR 关掉 tools/examples/testing；libwebp 只留三个库
  （`WEBP_BUILD_LIBWEBPMUX=ON` + 其余工具全 OFF + `CMAKE_DISABLE_FIND_PACKAGE_SDL=ON`）。
- **两处刻意不一致**，都写进注释：① port 用
  `-DCMAKE_REQUIRE_FIND_PACKAGE_libdeflate=ON`，这边不跟 —— 前缀里已经有 compat 模式的
  zlib-ng，那才是 Windows 侧的压缩后端；② libwebp 的 `0002-cmake-config.patch` 只改
  `WebP_LIBRARIES` 这个 vcpkg 专用变量，本项目链的是上游自己的
  `WebP::webp` / `WebP::webpdemux` / `WebP::libwebpmux`，不需要打。
- OpenEXR 额外传 `-DCMAKE_PREFIX_PATH="$prefix"`，否则它会拿系统 3.1 的 Imath 头配自己的 3.4 库。
- 判据：装完必须存在 `ImathConfig.cmake` / `OpenEXRConfig.cmake` / `WebPConfig.cmake`。

**pin 的出处问题**：`zlib`、`minizip-ng` 的版本与校验和是从 overlay port 现场读的，不会漂；
但 `imath`/`openexr`/`libwebp` 的 port 在 `.tools/vcpkg` 这个 vcpkg 克隆里，**CI 上没有**，
所以只能把 pin 抄进脚本。抄写就会过期，于是配一条门禁 `scripts/check-linux-deps-pins.py`：

- 在**这台机器**上把抄来的 `_version` / `_sha512` 与 `.tools/vcpkg/ports/<port>` 逐字段比对
  （当前三条全 ok）；没有克隆时打印一行 skip 并返回 0 —— 它本质是工作站检查。
- 同一条门禁还查管线：`for library in …` 里每个名字都要有 `build_<name>()` 与 version+sha512，
  反向也查（有 builder 却不在循环里 = 这个库永远不会被构建）。**这一类"写了却没接进循环"
  是本仓库第一次出现的假绿形态**：脚本照旧成功，只是少装一个库，然后在 configure 时报"找不到包"。
- 反向验证：把 `openexr_version` 改成 `3.4.8` ⇒ 报 `script pins version '3.4.8' but port 'openexr' is '3.4.9'`。

**下一个红灯大概率是 libjxl**：vcpkg 是 `0.11.2`（overlay port），24.04 是 `0.7.0`，
而 0.7 → 0.11 的编码器 API 是重写过的；`libheif` 也从 `1.21.2` 对 `1.17.6`。两者都还没动手，
理由和这一片一路的做法一致：等 CI 报出**具体哪个符号**，再决定是自己带版本还是适配。
另外 configure 里那条 `libz.so.1 in /usr/lib/… may be hidden by files in <prefix>/lib` 的警告仍在，
它就是 9.6.9 留给打包决定的那个"运行时链接谁"。

#### 9.6.13 run 11：同一个病在 JXL / HEIF 上（2026-10-21）

run 11（`c03eacf3`）的 `Build the pinned Linux dependencies` 全绿（Imath/OpenEXR/libwebp 在 Linux 上
第一次编过），EXR 与 WebP 的报错消失，剩下的正是上一节预判的：

```
snow_image/src/codecs/heif_codec.cpp:8: fatal error: libheif/heif_sequences.h: No such file or directory
snow_image/src/codecs/jxl_codec.cpp:150: error: cannot convert 'JxlColorProfileTarget' to 'const JxlPixelFormat*'
snow_image/src/codecs/jxl_codec.cpp:338: error: 'JXL_DEC_BOX_COMPLETE' was not declared
snow_image/src/codecs/jxl_codec.cpp:1048: error: 'JxlEncoderDistanceFromQuality' was not declared
```

`JXL_DEC_BOX_COMPLETE`、`JxlEncoderDistanceFromQuality`、`JxlDecoderGetColorAs*Profile` 的
`JxlColorProfileTarget` 重载，都是 0.7 之后才有的；`heif_sequences.h` 是 libheif 1.18+ 才拆出来的头。
**两者都不是"补一个符号"能解决的**，于是同一轮回把三个库按固定版本自编：

| 库 | vcpkg | 24.04 | 说明 |
|---|---|---|---|
| highway | 1.3.0 | 1.0.7 | libjxl 的 `JPEGXL_FORCE_SYSTEM_HWY` 后端 |
| libjxl | 0.11.2 | 0.7.0 | 编码器 API 重写过 |
| libheif | 1.21.2（overlay） | 1.17.6 | 缺 `heif_sequences.h` |

三处设计（都在脚本注释里）：

1. **GitHub 归档没有 submodule**，所以 libjxl 必须走 `JPEGXL_FORCE_SYSTEM_*`（hwy / brotli / lcms2）
   且 `JPEGXL_ENABLE_SKCMS=OFF`（skcms 是 submodule），`JPEGXL_BUNDLE_LIBPNG=OFF` 用前缀里的
   libpng。这不是选择，是归档包的硬约束。
2. **libheif 只开 Windows 也开的那套 feature**：manifest 里 `libheif` 两个架构都写着
   `"features": ["aom"]`（overlay port 的 `default-features` 只有 `hevc`，但 manifest 是
   **显式追加** aom 的），所以 Windows 版本来就有 AVIF 编码器 —— 最初按 default-features
   推理成"Windows 也没有 AVIF"，因而把 `WITH_AOM_*` 关掉了，那是**看漏了 manifest**：
   Linux 侧因此能选 AVIF 却在保存时报 `No encoder is available for the requested format`
   （CI run 41418087468 的 `quick-save-export` / `export-artifact` 两条），见下面的修订。
   其余 `WITH_DAV1D`/`WITH_X264`/`WITH_JPEG_*`/`WITH_OpenJPEG_*` 仍一律 OFF。
3. **apt 里删掉 `libheif-dev` / `libjxl-dev`**。两个来源并存时，自编失败会静默回落到发行版老版本，
   然后以一堆看不懂的 API 错误出现在编译期；删掉之后只剩一条路，失败就是 configure 期的
   `find_package` 报错。`libde265`/`x265`/`brotli`/`lcms2` 保留 —— 它们是自编时的后端。

判据按消费者来定，不按"装上了"：libjxl 这条链**不走 CMake config 而走 pkg-config**
（`pkg_check_modules(JXL libjxl)` + `libjxl_threads`），所以脚本断言的是
`libjxl.pc` 与 `libjxl_threads.pc` 两个文件；libheif 断言 `libheif-config.cmake`；
highway 断言 `hwy-config.cmake`。

pin 门禁扩到六条，并顺带修好它自己的一个结构性缺陷：overlay port 在**仓库里**（CI 也有），
builtin port 在 `.tools/vcpkg`（CI 没有），所以查表时先 overlay 后 builtin。现在
`libjxl`/`libheif` 的比对在 CI 上也会真的执行，只有 `imath`/`openexr`/`libwebp`/`highway` 会打印 skip；
并且新增一条"一条都比对不了就失败"，避免整张表哑掉还被当成绿。

#### 9.6.14 run 12：一个静默失败的 `die`，和 highway 少一个开关（2026-10-21）

run 12（`2e10f9d1`）的失败方式比错误本身更值得记：`Build the pinned Linux dependencies`
**退出码 1，一行输出都没有**。日志里最后一条是 libwebp 装好 `WebPConfig.cmake`，
8 秒后就 `##[error]Process completed with exit code 1`。

两个原因叠在一起：

1. 循环里每个 builder 都是 `build_x >"$log" 2>&1 || { cat "$log"; die ...; }` ——
   **`die` 自己也把 stderr 重定向进了 `$log`**，而且它用 `exit` 直接结束 shell，于是
   `cat "$log"` 那一半永远跑不到。消息进了日志文件、进程退出、控制台空白。
   修法：脚本开头 `exec 3>&2`，`die` 写 fd 3 —— 重定向只影响 1/2，fd 3 是通向控制台的逃生口。
   **判据**：凡是"把输出重定向进日志 + 在函数里直接 exit"的脚本，都要有这么一个逃生口，
   否则最关键的失败恰好是最安静的那种。
2. 真正的错误是 highway 的 `hwy-config.cmake` 没装出来。**upstream 默认 `HWY_ENABLE_INSTALL=OFF`**，
   vcpkg 的 portfile 里显式给了 `-DHWY_ENABLE_INSTALL=ON` —— 抄 portfile 时漏了这一条，
   于是库编好了但没装 config，libjxl 那条 `find_package(hwy CONFIG)` 注定失败。
   这说明"照抄 portfile 的 OPTIONS"要抄全，包括那些看着像默认值的开关。

顺带把 9.6.9 留下的最后一个"诚实的弱点"补掉：`onnxruntime` 的两份预编译包从来没有校验和。
run 12 的日志把两份资产的 sha512 都打印出来了（x64 `4191bc61…`、aarch64 `6fb4fbdd…`，
取自 CI 实际下载的那两个文件），现已回填到脚本，`check-linux-deps-pins.py` 也加了一条断言：
任一架构的 onnxruntime 校验和为空即失败。从此这条下载链接是验证过的，不再是"打印出来等你回填"。

#### 9.6.15 run 13：deps 终于绿了，红灯落到 Rust FFI 的 lockfile（2026-10-22）

run 13（`9997a16b`）先证明 9.6.14 的两条修复都成立：deps 步骤在 x64 与 arm64 上**全绿**，
`Configure` 也过了，红灯第一次落到 **`Build` 这一步的 Rust FFI 打包**上：

```
[45/611] cd .../snow_rust_ffi && cargo build --locked --package snow-shot-rust-ffi ...
   Updating crates.io index
error: cannot update the lock file .../snow_rust_ffi/Cargo.lock because --locked was passed to prevent this
```

**这不是编译错误。** C++ 侧的目标在前 44 步里全编过去了，其中
`snow_image` 的静态库在 `[26/611]` 链接成功（里面是 jxl / heif / exr / webp 编解码器）
—— 固定版本自编的那批依赖第一次在 Linux 上真的被编进了产物。

**根因：仓库里有两个 cargo 工程，只有一个的 lockfile 被检查过。**
`snow_rust_ffi` 是独立工程、独立 `Cargo.lock`；AT-SPI2 从 `snow-selected-text` 抽成
`snow-atspi`（`a0034910`，slice 14）、控件级拾取接进 `snow-ui-selector`
（`a20ff192`，slice 15）之后，那份 lock 再没重新生成过 —— lock 里没有 `snow-atspi`，
`snow-selected-text` 的依赖还停在 `accessibility-sys`。它最后一次提交是 `4b7c5e08`，
早于那两次重构。

**Windows 本地为什么没暴露**：没人重编到 FFI 那一步（`windows-qt5-full` 只到 configure），
而 `gates` job 的 `cargo test` 跑在 `snow-crates/` 那个工程上，用的是另一个 lockfile。
**只有真的编 FFI 的 job 才会撞上** —— 这正是 build job 存在的理由。

修：离线重新解析（`cargo metadata --offline`），cargo 保留既有版本、只补缺失项：
新增 `snow-atspi` 依赖树（`async-trait` / `endi` / `futures-core` / `futures-io` /
`hex` / `memoffset` / `ordered-stream` / `pin-project-lite` / `serde_repr`），
`bitflags` 2.13.1→2.13.2、`rustix` 1.1.4→1.1.5 —— 与 2026-10-21 日志里记的
"zbus 硬要求 `bitflags ≥ 2.13.2`"完全对上。

**顺带把这类失败提前一个 job**：`gates` job 加一步 `cargo metadata --locked`，两个工程
都查。lock 过期原本要等十分钟、在一个看起来像编译错误的步骤里才炸；现在是一分钟、
在允许红的那个 job 里炸。`check-linux-ci-workflow.py` 加一条断言
（反向验证：把 `snow_rust_ffi` 换成 `snow-crates` ⇒ FAIL）。

**判据（可复用）**：一个仓库里有几个 cargo 工程，就要有几个 lockfile 检查点。
"只有一个 lockfile 被检查"和"只有一个 lockfile 存在"是同一类错误。

#### 9.6.16 run 14：错误指向头文件，实际是搜索路径被劫持（2026-10-22）

run 14（`39707846`）把 lockfile 修好后，红灯第一次落到 **Rust FFI 的 FFmpeg 绑定**上：

```
cargo:rustc-link-search=native=/home/runner/work/snow-apps/snow-apps/.tools/vcpkg/installed/x64-linux
cargo:rustc-link-lib=dylib=avcodec
cargo:rustc-link-lib=dylib=avfilter
...
check.c:771:18: fatal error: libavfilter/avfilter.h: No such file or directory
```

**第一眼看上去是"CI 机器没装 libavfilter-dev"**。但 CI 的 apt 清单里当时**根本没列** `libav*-dev`
（这是真的漏了），而更关键的是：**就算装了也不会被找到**。错误里的
`rustc-link-search` 指向 `.tools/vcpkg/installed/x64-linux` —— 一个**在 Linux 上根本不存在**的目录。

**根因链（从 `cmake/SnowWorkspace.cmake` 到 bindgen 找不到头文件，四步）**：

1. `cmake/SnowWorkspace.cmake` 无条件设
   `SNOW_FFMPEG_ROOT = ${SNOW_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}` ⇒ Linux 上得到
   `.tools/vcpkg/installed/x64-linux`（不存在）。**vcpkg 在 Linux 上根本不跑**。
2. `cmake/RustStaticLibrary.cmake` 把 `SNOW_FFMPEG_ROOT` 当作 `FFMPEG_DIR` 传给 cargo。
3. `ffmpeg-sys-next` 9.0.0 的 build 脚本**四段式**优先级：
   `build` feature → **`FFMPEG_DIR`** → vcpkg → pkg-config。**`FFMPEG_DIR` 压过 pkg-config**。
4. 走 `FFMPEG_DIR` 分支时，include 目录**只有** `<FFMPEG_DIR>/include`（原样返回
   `vec![ffmpeg_dir.join("include")]`），native lib 目录按 arch 选 `lib/amd64` / `lib/arm64`，
   然后直接发 `cargo:rustc-link-lib` —— **全程不调用 `pkg_config`**。

⇒ 结论：**`FFMPEG_DIR` 一旦被设置，pkg-config 就永远不会被问到。**

**空串陷阱**：`env::var("FFMPEG_DIR")` 对 `FFMPEG_DIR=""` **仍然返回 `Ok`** ⇒ build 脚本照样走
prebuilt 分支，而 `PathBuf::from("")` 会得到**相对路径** `include` / `lib`。
所以正确做法是**完全不导出这个变量**，不是导出空值。

**路线选择（两条都排除）**：

| 路线 | 代价 | 结论 |
|---|---|---|
| Linux 侧门控掉整条 recording 链 | `snow-recording-c` 4157 行 Rust **无任何 Linux 桩**；C++ 侧 **139 处** `snow_recording_*` 引用跨 8 个文件；`snow-recording-export` / `-runtime` **无条件**依赖 ffmpeg-next | 与 9.5 的 A 类归因冲突（这类失败"与 Linux 移植无关"），且规模远超"修一个红灯" ⇒ 排除 |
| 自编 FFmpeg 9.0 | 需要 bin2c host 工具、nasm、16 个 vcpkg patch、双架构各自长编译 | 与 §5.1「Linux 用发行版 `libavcodec-dev`」的既定决策相悖 ⇒ 排除 |
| **Linux 用发行版 `libav*-dev`** | apt 装 6 个包 | **采纳**（§5.1 早已拍板） |

**采纳前逐条核实的事实**（全部从本地 `~/.cargo/registry` 的 crate 源码逐字读出来，不靠文档站）：

1. `ffmpeg-sys-next` 9.0.0 **没有任何最低版本断言**（无 `assert!(version >= ...)`、无
   `pkg_config::atleast_version`）⇒ 6.1 不会被版本门挡住。
2. `ffmpeg-next` 9.0.0 **官方支持 FFmpeg < 7.0**：`src/util/mod.rs` 里
   `#[cfg_attr(feature = "ffmpeg_7_0", path = "channel_layout.rs")]` /
   `#[cfg_attr(not(feature = "ffmpeg_7_0"), path = "legacy_channel_layout.rs")]`。
3. 项目实际用到的 API 在 legacy 路径**全部存在**：`channel_layout()` /
   `set_channel_layout()`（`codec/decoder/audio.rs`、`codec/encoder/audio.rs`）、
   `channel_layouts()`（`codec/audio.rs`，三个 cfg 分支下都有）。
4. Ubuntu 24.04 = FFmpeg **6.1.1**（libavcodec **60.31**）⇒ 命中 `ffmpeg_6_1` 阈值（60.31）、
   **不**命中 `ffmpeg_7_0`（61.3）⇒ 正好走 legacy 分支。
5. pkg-config 探测的模块集合 = `libavutil` + `libavcodec`（**无条件**）+
   按 feature 的 `libavformat` / `libavfilter` / `libswscale` / `libswresample`
   ⇒ 与 C++ 侧 Windows 块原有的列表**逐字相同**，也正是本工程 feature 集对应的集合。
6. `ffmpeg-sys-next` 用 bindgen 生成绑定 ⇒ Linux 上需要 libclang。
   CI 的 apt 日志显示 `libclang1-15t64` 已随 `libopencv-dev` **间接装上**，`clang-sys v1.9.1`
   正常编译 ⇒ **不需要新增包**。

**改了什么（5 个源文件）**：

| 文件 | 改法 |
|---|---|
| `cmake/SnowWorkspace.cmake` | Linux 分支把 `SNOW_FFMPEG_ROOT` 默认成**空**（=「让 pkg-config 找系统 FFmpeg」），不再指 vcpkg 树；非 Linux 分支不变 |
| `cmake/RustStaticLibrary.cmake` | 推导逻辑加 Linux 分支（不回落 vcpkg）；`FFMPEG_DIR` **仅在非空时**才 `list(APPEND)` 到 cargo 环境 |
| `snow_shot/CMakeLists.txt` | ffmpeg 的 `pkg_check_modules` 从 `if(WIN32)` 里**提出来**，变成 `if(WIN32) / elseif(Linux) / endif()` 三段式；链接块由 `if(WIN32)` 改为 `if(TARGET PkgConfig::SNOW_SHOT_FFMPEG)`（跨平台）；DLL 收集块加 `WIN32` 守卫；两处 Windows 专属的 `PATH=` 注入加 `if(WIN32)` 守卫 |
| `snow-crates/native/CMakeLists.txt` | `pkg_check_modules(SNOW_MEDIA_FFMPEG ...)` **补上 `libavfilter`** —— Rust 侧明确 link `avfilter`，C 库漏了它符号就悬空 |
| `.github/workflows/snow-shot-linux.yml` | apt 清单加 6 个 `libav*-dev` 包 |

**C++ 侧的镜像问题**：Rust bundle 是**静态归档**，携带 `av*` 符号 ⇒ 最终可执行文件必须自己把
`libav*` 放上链接行。而 `snow_shot/CMakeLists.txt` 原先把 ffmpeg 的 `pkg_check_modules` 与全部链接语句
**整块关在 `if(WIN32)` 内** ⇒ Linux 上 C++ 侧压根不链接 ffmpeg，链接期才会炸。
Rust 静态库的 bundle 传播机制
（`target_link_libraries(${_snow_rust_bundle} INTERFACE PkgConfig::SNOW_SHOT_FFMPEG)`）
会自动把依赖传给所有链接该 bundle 的 target。

**门禁：`scripts/check-linux-ffmpeg-wiring.py`（28 项）**，已接进 `scripts/check-linux-packaging.sh`
⇒ 自动进 `gates` job。它查 5 个面：SnowWorkspace 的 Linux 默认值 / RustStaticLibrary 的 Linux 推导
与 `FFMPEG_DIR` 条件导出 / snow_shot 的 Linux 探测与模块列表 / native CMake 的 `libavfilter` /
CI apt 与文档 §9.1 的包清单。

**这门禁自己被反向验证改掉了两处 —— 两次都是"假门禁"**：

1. **定位错分支**。第一版用 `re.search(r'elseif\(CMAKE_SYSTEM_NAME STREQUAL "Linux"\)...')`
   找 Linux 分支，但 `re.search` 找的是**文件里第一处**：`RustStaticLibrary.cmake:120` 是
   **Rust target 推导**（`aarch64-unknown-linux-gnu`），`SnowWorkspace.cmake:36` 是 **vcpkg triplet**
   —— 都跟 FFmpeg 无关。门禁当时"全绿"是**蒙对的**：它检查的是那两个分支里没有 `VCPKG_INSTALLED_DIR`。
   ⇒ 改成**解析 if/elseif/else/endif 嵌套**，按分支结构定位。
2. **守卫判据是子串匹配**。`if(TARGET PkgConfig::SNOW_SHOT_FFMPEG)` 在 `snow_shot/CMakeLists.txt` 里
   **出现 5 次**。用 `in` 判断"链接没有被限制在 Windows"⇒ 给其中**任意一处**加 `AND WIN32`，
   另外 4 处仍然满足子串 ⇒ 门禁照样绿。
   ⇒ 改成**记录每条语句所处的守卫链**（scope chain），断言链上没有 `WIN32`。

顺带加了一条**解析器自洽性断言**：三个 CMake 文件的 `if()` / `endif()` 数量必须相等 ——
解析器一旦失步，之后所有分支定位都是虚构的。（这条断言当场抓出了门禁自己的一处 bug：
`_command_balance` 用了未 strip 的行，缩进导致 `if(` 漏计。）

**反向验证：13/13 变异全红、1/1 基线绿**（`.workbuddy/uos-port/reverse-verify-ffmpeg-wiring.py`）。
13 个变异就是**当初让 run 14 变红的那些代码**，不是合成的：

| 变异 | 门禁的判据 |
|---|---|
| Linux 默认值改回 vcpkg 路径 | "the Linux SNOW_FFMPEG_ROOT default does not point at the vcpkg tree" |
| Linux 默认值改成非空路径 | "the Linux SNOW_FFMPEG_ROOT default is empty" |
| Linux 推导回落 vcpkg | "the Linux SNOW_FFMPEG_ROOT derivation does not reach for vcpkg" |
| Linux 推导改成非空 | "the Linux SNOW_FFMPEG_ROOT derivation leaves the prefix empty" |
| `FFMPEG_DIR` 改回无条件导出 | "RustStaticLibrary.cmake only exports FFMPEG_DIR when the prefix is set" |
| Linux 分支的 `PkgConfig` 从 REQUIRED 降级 | "the Linux branch requires pkg-config" |
| Linux 分支删掉 `pkg_check_modules` | "snow_shot/CMakeLists.txt probes FFmpeg on Linux" |
| Linux 模块列表删 `libavfilter` | "every module ffmpeg-sys-next probes is in the CMake module list" |
| 链接块加回 `AND WIN32` | "the Rust FFI bundle carries the FFmpeg dependency on every platform (no WIN32 guard)" |
| bundle 不再 `INTERFACE` FFmpeg | "the Rust FFI bundle carries the FFmpeg dependency on every platform" |
| native CMake 删 `libavfilter` | "the C library probes every module the Rust side links" |
| apt 清单删 `libavfilter-dev` | "the CI workflow installs libavfilter-dev" |
| 文档 §9.1 删 `libavfilter-dev` | "the port document lists the FFmpeg development packages" |

**判据（可复用）**：错误信息指向**头文件缺失**时，先问"搜索路径对不对"，再问"包装没装"。
一个变量（`FFMPEG_DIR`）能静默地把整条 pkg-config 解析链**旁路**掉，
而它的错误表现是"某个头文件找不到" —— 两者之间没有任何提示。

---

#### 9.6.17 run 15：一个"从未执行过的分支"，和四个跨符号域的掩码（2026-10-22）

run 15（`39715098`）把 FFmpeg 修好后，红灯第一次回到 **C++ 侧**。但它落在的位置很说明问题：

```
[344/611] Building CXX object CMakeFiles/snow_shot.dir/snow_shot/src/presentation/services/languagemanager.cpp.o
[346/611] Building CXX object CMakeFiles/snow_shot.dir/snow_shot/src/presentation/services/windowshortcutmanager.cpp.o
```

**只编了 2 个 TU 就停了**（611 步里的第 344、346 步）⇒ 后面 **267 步根本没跑到**，
同类缺陷**很可能还有**。而这 2 个 TU 的报错**全在同一个文件**：

```
snow_shot/include/snow_shot/compat/qt_compat.h:91:5: error: "SNOW_QT5" is not defined, evaluates to 0 [-Werror=undef]
snow_shot/include/snow_shot/compat/qt_compat.h:194:54: error: conversion to 'unsigned int' from 'int' may change the sign of the result [-Werror=sign-conversion]
snow_shot/include/snow_shot/compat/qt_compat.h:195:40: error: conversion to 'unsigned int' from 'int' may change the sign of the result [-Werror=sign-conversion]
snow_shot/include/snow_shot/compat/qt_compat.h:278:51: error: conversion to 'uint' {aka 'unsigned int'} from 'int' may change the sign of the result [-Werror=sign-conversion]
snow_shot/include/snow_shot/compat/qt_compat.h:745:51: error: conversion to 'uint' {aka 'unsigned int'} from 'int' may change the sign of the result [-Werror=sign-conversion]
```

`qt_compat.h` 是**被 force-include 进每一个 C++ TU 的那个头**
（`cmake/SnowQt.cmake`：Qt5 路线下 `-include` / `/FI`）⇒ 它的每一个缺陷都会在
**每一个** TU 上放大，包括第三方 target 和 QtCore-only target。

**第一处：一个从未执行过的分支（`-Wundef`）**

原文件第 91 行是 `#if SNOW_QT5`，而 `#define SNOW_QT5` 在**第 113 / 129 行**。
按 C 的规则，**未定义宏在 `#if` 中按 0 求值** ⇒ 这一整块在**所有平台上**都是死代码。

于是有两条结论，第二条更严重：

1. `#if SNOW_QT5` 里的 `#include <QVector>` **从未生效**；
2. `SnowRoleList` 之所以还能编过，**纯属侥幸** —— 上面第 72 行的 `<QList>`
   经 `qlist.h:48` 传递包含了 `qvector.h`。

**"看起来存在"的分支可能从未执行过**，这比编译错误危险得多：它不会报错，
只会让某段代码悄悄失去意义。而 `-Wundef` 正是把这件事变成硬错误的开关
（`cmake/ProjectWarnings.cmake` 的警告集里有它）⇒ **GCC/Clang 上第一次编就必然红**。

修法：把版本判定整块**上移到第一个 `#if` 之前**。

**第二处：掩码跨越了有符号/无符号域（`-Wsign-conversion`，4 处）**

`Qt::KeyboardModifierMask = 0xfe000000` **装不进有符号 int** ⇒
`Qt::KeyboardModifier` 的底层类型是 `unsigned int`。而更要命的是 `qflags.h`：

```cpp
// C:/Qt/5.15.2/msvc2019_64/include/QtCore/qflags.h:103-112
#if defined(Q_CC_MSVC) || defined(Q_CLANG_QDOC)
    typedef int Int;                        // MSVC 恒为 int
#else
    typedef typename std::conditional<
            std::is_unsigned<typename std::underlying_type<Enum>::type>::value,
            unsigned int, signed int
        >::type Int;                        // GCC/Clang 随枚举底层类型走
#endif
```

⇒ **GCC/Clang 上 `QFlags<KeyboardModifier>::Int` 也是 `unsigned int`**
⇒ `combined & mask` 的常规算术转换把 `int` 提升为 `unsigned int`
⇒ 报 `conversion to 'unsigned int' from 'int'`。

**为什么 Windows 本地永远看不见**：Qt 自己在 `qflags.h:49` 写了
"Microsoft Visual Studio has buggy behavior when it comes to unsigned enums:
even if the enum is unsigned, the enum tags are always signed"
⇒ MSVC 上枚举标签按有符号处理，`Int` 又恒为 `int` ⇒ 整条路径在 MSVC 上是干净的。
**这不是我们代码的运气，是编译器的差异**。

修法与 **Qt 6.12 官方 `qnamespace.h:1945/1950` 的写法对齐**：

```cpp
const int modifierMask = static_cast<int>(Qt::KeyboardModifierMask);
return QKeyCombination(Qt::KeyboardModifiers(combined & modifierMask),
                       Qt::Key(combined & ~modifierMask));
```

官方也是 `~int(Qt::KeyboardModifierMask)` —— 先把掩码拉回有符号域再掩。

**第三处：`QKeySequence::operator[]` 收 `uint`（2 处）**

两版 Qt 的签名一致：

```
Qt 5.15.2  QtGui/qkeysequence.h:187   int operator[](uint i) const;
Qt 6.12.0  QtGui/qkeysequence.h:153   QKeyCombination operator[](uint i) const;
```

⇒ 传 `int index` 触发 `conversion to 'uint' from 'int'`。修法：`static_cast<uint>(index)`。
`uint` 在两版均为全局作用域（Qt5 `qglobal.h:275`、Qt6 `qtypes.h:40` 经 `qglobal.h:36` 引入）。

**本机验证（比推理更硬）**：本机没有 GCC（`which g++ gcc clang++` 全空，`wsl.exe` 命中沙箱黑名单），
所以用 **MSVC 单独编译这个头**，两版 Qt 各一次：

| 验证 | 手段 | 结果 |
|---|---|---|
| 改动没破坏 Windows 构建 | MSVC 19.44 对 Qt 5.15.2 / Qt 6.12.0 各编一个覆盖全部两个分支的 TU | 两版都过（`/W4 /permissive- /std:c++20`） |
| 修法语义等价 | `constexpr` + `static_assert` 比对修复前后两种写法 | 逐位相等（含符号位 `0x80000000`、纯修饰键、高位键码 `0x01000000` 三组输入） |
| Windows 本地确实是盲区 | 把**修复前**的写法原样编进 MSVC `/W4` | **一声不吭** ⇒ 坐实只有 Linux CI 能发现 |

**门禁：`scripts/check-qt-compat-macro-order.py`（7 项）**，已接进
`scripts/check-linux-packaging.sh` ⇒ 自动进 `gates` job。它追踪兼容头自身的
`#define` / `#undef`，拦截"先测后定义"，并要求 `<QVector>` 在 Qt5 分支可达、
两个版本宏同块定义且取 0/1 字面量。

**这门禁自己被反向验证改掉了两处 —— 两次都是"假门禁"**：

1. **头文件名被当成宏**。`__has_include(<QtGlobal>)` 里的 `QtGlobal`
   被裸标识符正则当成"被测试的宏"⇒ 误报 8 条。
   ⇒ 改成**整体移除 `__has_include(...)` 的参数列表**再扫。
2. **`#if` / `#else` 两支各一次 `#define` 被误判为"重复定义"**。
   `#define SNOW_QT5 0` 与 `#define SNOW_QT5 1` 分处两支是**正常正确写法**。
   ⇒ 改成「两个宏在**同一个 `#if` 块**内（行距 < 8）」+「每个定义都是 0/1 字面量」两条判据。

**反向验证：3/3 变异全红、1/1 基线绿**：

| 变异 | 门禁的判据 |
|---|---|
| 把版本宏定义**移回** `<QVector>` include 之后（还原 run 15 的原始形态） | "SNOW_QT5 is not defined before the #if" + "SNOW_QT5 defined at line 105, tested at line 98" |
| 删掉 `#include <QVector>` | "the compat header includes <QVector> for the Qt5 branch" |
| 把 `SNOW_QT5` 定义成 `2` | "every SNOW_QT5/SNOW_QT6 definition is a 0/1 literal" |

每次恢复源文件后都 `touch` 更新 mtime，确认最终 `EXIT=0`。

**arm64 侧是环境 flake，不是代码缺陷**：

```
error: failed to install component: 'clippy-preview-aarch64-unknown-linux-gnu',
       detected conflict: 'bin/cargo-clippy'
```

aarch64 runner 镜像上 `1.97.1-aarch64-unknown-linux-gnu` 处于**半安装状态**
⇒ `dtolnay/rust-toolchain@stable` 装 clippy 时与残留文件冲突。
run 14 用**同一个 action** 是成功的 ⇒ 环境 flake。

> **run 17 的实测结论（2026-10-22 补）**：这一条当时只写到了"是 flake"，
> 没写"那 arm64 到底有没有过"。事实是**run 17 的 arm64 `Configure` 步骤成功了**
> （job 列表里 `9. Configure` 是 success，红灯在 `10. Build`）⇒
> **加固方向是对的，但那是"降窗"而不是"治本"**：
> 把 `dtolnay/rust-toolchain@stable` 换成手写的
> `rustup toolchain install 1.97.1 --profile minimal --no-self-update --force-non-host`
> 加上 `RUSTUP_TOOLCHAIN=1.97.1`，只是让 clippy 组件**不参与那次安装**，
> 从而不撞上镜像里的残留文件；真正的病根是
> [rust-lang/rustup#3530](https://github.com/rust-lang/rustup/issues/3530)
> 那个"非宿主 target 的组件安装会连 `rust-std` 一起回滚"的 bug。
> **含义**：这条 flake 随时可能复发，CI 红了要先看是不是这条，
> 别误当成自己的代码又坏了。
>
> **完整因果链（run 15 arm64 全过程）**：
>
> 1. `dtolnay/rust-toolchain@stable` 装 clippy → 与镜像残留的 `bin/cargo-clippy` 冲突；
> 2. rustup 回滚这次安装，而**回滚把已装好的 `rust-std` 一起删了**（#3530）；
> 3. `snow_detect_rust_target()` 随后探到 target 不存在 ⇒ CMake 报
>    "Rust target ... is not installed or usable"；
> 4. **错误指向了 target，而病根在工具链安装** ⇒ 这正是 9.6.19 那个
>    错误消息门禁要治的病（消息必须给对平台的真正补救办法，
>    并把 cargo 自己的输出带上，而不是丢掉）。

**判据（可复用）**：

- **"未定义宏在 `#if` 中按 0 求值"** —— 一段看起来存在的代码可能从未执行过。
  开了 `-Wundef` 的工程里，这类缺陷**必然在 GCC 上第一次编译就红**。
- **跨有符号/无符号域的掩码**：枚举值超出 `int` 范围 ⇒ 底层类型变 `unsigned`
  ⇒ `int & unsigned` 触发常规算术转换 ⇒ `-Wsign-conversion`。
  **MSVC 上枚举标签永远按有符号处理**，所以这类缺陷在 Windows 本地是隐形的。
- **只在 Linux CI 暴露的缺陷，本地唯一的硬验证手段是"用能复现的那套编译器编"**；
  没有 GCC 时，退而求其次的**编译期 `static_assert` 等价性证明**
  仍然强于纯源码推理。

#### 9.6.18 run 16 / 17：红灯第一次越过兼容头（2026-10-22）

run 16（`39734199355`）与 run 17（`39736061339`）是一次推送的两个连续状态，
把它们放在一起看才有意义 —— **run 16 的 build 根本没跑完**。

| run | SHA | gates | Configure | Build | 说明 |
|---|---|---|---|---|---|
| 16（`39734199`） | `81fe9bf3` | ✅ | ✅ | **cancelled** | `qt_compat.h` 修好后红灯首次越过兼容头；`gates` 一次全绿（含新增门禁）。但 x64 的 Build 在第 10 步被**新 push 顶掉** |
| 17（`39736061`） | `87409bbc` | ✅ | ✅ | ❌ | 红灯落在 `screenshotclipboardservice.cpp:97` 的 `retrieveData` 上，见 9.6.19 |

**"被新 push 顶掉"这件事本身值得记一笔**。GitHub Actions 的 `concurrency` 组是
`${{ github.workflow }}-${{ github.ref }}` ⇒ **同一分支的第二次 push 会取消第一次还在跑的 run**。
于是：

- run 16 的 `gates` 跑完了（绿），`Configure` 跑完了（绿），`Build` 被取消；
- **run 16 的 Build 没有给出任何结论** —— 它不是"通过了"，是"没跑"。

⇒ **"gates ✅" 这一栏对 build 没有背书作用**，两个 run 必须一起读。

run 17 的实际位置：**`[487/611]`（x64）/`[487/608]`（arm64）**，
比 run 15 的 `[346/611]` 前进了 **141 步** ⇒ 兼容头那一关是真的过了，
而且是**两个架构同时过**（arm64 的 `Configure` 也成功了，见 9.6.17 的补注）。

**run 17 的 arm64 有一件 run 15 没有的事**：`Install Rust toolchain` 与 `Configure` 都是
success，红灯同样落在 `10. Build` ⇒ **arm64 的环境 flake 这次没复发**，
印证了 9.6.17 补注里的"那是 flake、加固只是降窗"。

#### 9.6.19 run 17：Qt5 / Qt6 的虚函数签名分歧，与一个平台守卫造成的结构性盲区（2026-10-22）

run 17 的红灯，两条日志（x64 / arm64）**逐字相同**：

```
snow_shot/src/presentation/services/screenshotclipboardservice.cpp:97:14: error:
  'QVariant {anonymous}::PngClipboardMimeData::retrieveData(const QString&, QMetaType) const'
  marked 'override', but does not override
snow_shot/src/presentation/services/screenshotclipboardservice.cpp:103:46: error:
  cannot convert 'QMetaType' to 'QVariant::Type'
```

**根因：两版 Qt 的这个虚函数签名不同。**

| | 第二参数类型 | 声明位置 |
|---|---|---|
| Qt 5.15.2 | `QVariant::Type` | `QtCore/qmimedata.h:88`（`protected`） |
| Qt 6.12.0 | `QMetaType` | `QtCore/qmimedata.h:53`（`protected`） |

**而 Qt 6 根本没有 `QVariant::Type` 这个枚举** —— `QtCore/qvariant.h` 里搜不到 `enum Type`
（它在 Qt 6 被 `QMetaType` 取代了）⇒ **两种拼法不可能在同一个 TU 里共存**，
一份源码必须有一个随版本切换的写法。

**这个缺陷为什么在 Windows 本地看不见（结构性盲区）**：

```cpp
// snow_shot/src/presentation/services/screenshotclipboardservice.cpp
#if !defined(Q_OS_WIN)
    class PngClipboardMimeData final : public QMimeData {   // ← 整个类在守卫里
        ...
        QVariant retrieveData(const QString& mime, QMetaType type) const override { ... }
    };
#endif
```

Windows 本地走 Qt 6，**这个类根本不参与编译** ⇒ 缺陷在本地是隐形的，
只有 Linux CI（Qt 5.15.13）能撞出来。

⇒ **"Windows 编得过"不等于"Linux 编得过"**，尤其当 Linux 侧多了一层平台守卫。
这是 9.6.17 那条判据的同一个家族：**看不见的代码面 = 盲区**。

**修法（三处一起，缺一不可）**：

1. **兼容头加类型别名**（`snow_shot/include/snow_shot/compat/qt_compat.h`）：

   ```cpp
   #if SNOW_QT6
   using SnowMimeDataType = QMetaType;
   #else
   using SnowMimeDataType = QVariant::Type;
   #endif
   ```

2. **两处调用点改用别名** —— 业务代码 `screenshotclipboardservice.cpp:97`
   **和测试代码 `screenshot_clipboard_content_tests.cpp:826`**。
   第二处是**门禁抓出来的**（见下），不是编译器抓的。

3. **兼容头里加 probe 类**（关键一步，见下）。

**probe 类：怎么把结构性盲区变成每条路线都编译到**

`qt_compat.h` 是被 **force-include** 进**每一个** C++ TU 的
（`cmake/SnowQt.cmake:146-150`：Qt5 走 `-include`、Windows 走 `/FI`；
**Qt6 路线也注入**，见 `SnowQt.cmake:125-135` 的中文注释）⇒
**把 probe 类放进兼容头，任何路线都会编译它**：

```cpp
namespace snow_compat_probe {
class MimeDataRetrieveDataProbe final : public QMimeData {
  protected:
    QVariant retrieveData(const QString& mimetype, SnowMimeDataType type) const override {
        return QMimeData::retrieveData(mimetype, type);
    }
};
}
```

⇒ **签名一旦写错，每条路线（Windows/Qt6、Linux/Qt5）都会立刻红**，
而不是等到某个平台守卫的另一侧才暴露。

**`static_assert` 精确锁定别名 == 基类签名**：

```cpp
static_assert(std::is_same_v<decltype(&QMimeData::retrieveData),
                             QVariant (QMimeData::*)(const QString&, SnowMimeDataType) const>,
              "SnowMimeDataType must match QMimeData::retrieveData");
```

**坑**：`&QMimeData::retrieveData` 是 **protected** ⇒ **只能在派生类体内取地址**，
写在类外是 C2248。⇒ 这条 `static_assert` **必须放在派生类里**（本轮就踩了）。

**本机双版本编译验证**（没有 GCC，用 MSVC 19.44 各编一次）：

| 验证 | 手段 | 结果 |
|---|---|---|
| 别名在两版都成立 | MSVC 对 Qt 5.15.2 / Qt 6.12.0 各编一个含两个 probe 的 TU | 两版 `CL_EXIT=0`，`/W4` 无警告 |
| 别名**精确等于**基类签名 | 两个类内 `static_assert` | 两版都过 |
| 盲区确实关上了 | 把 Qt6 分支的别名故意改成 `QVariant::Type` | **Qt6 立刻红**（C3668 + C2664 + `static_assert` 失败），Qt5 仍绿（变异只动 Qt6 分支） |

**反向验证的读法**：变异只动 Qt6 分支 ⇒ **只有 Qt6 那条路红**才是对的。
如果两版都红，说明变异泄漏到了公共部分 ⇒ 判据本身有问题。
报错点在 `qt_compat.h(2149)` ⇒ **证明 probe 确实在每个 TU 里编译**。

**门禁 1：`scripts/check-qt5-qt6-virtual-signatures.py`（6 项）**，
已接进 `scripts/check-linux-packaging.sh` ⇒ 自动进 `gates` job。

它做两件事：**(a)** 要求分歧虚函数在兼容头里有别名，且别名**同时**覆盖两版类型；
**(b)** 扫全源码树，要求**每一处**覆盖都写成版本无关的形式。

**反向验证 4/4 全精准变红**：

| 变异 | 变红的判据 |
|---|---|
| 业务代码改回 `QMetaType` | "every override of a Qt5/Qt6 divergent virtual is version-neutral" |
| 测试代码改回 `QMetaType` | 同上（**这一条是门禁抓出来的真缺陷**，编译器当时没报，因为测试 TU 在报错文件之后才编） |
| 兼容头 Qt5 分支别名写错 | "… shim alias covers both Qt versions" |
| 删掉整个别名定义 | "has a shim alias in the compat header" + "covers both Qt versions" |

**门禁自己踩的三个坑（都是"假门禁"，跟 9.6.17 那两次同一家族）**：

1. **"别名覆盖两版"只匹配单行** ⇒ 别名定义跨两行就漏判
   ⇒ 改成收集**全部** `using <alias> = ...;` 体再判。
2. **版本守卫识别有 bug**（`#if` / `#else` 两支处理不对）
   ⇒ 改成标准栈式逐行扫描（`#if` push、`#endif` pop、判 `any(stack)`）。
3. **`offset` 取到文件里第一个匹配**，不是当前那一处 override
   ⇒ 新增 `offset_of_line()`。

**门禁 2：`scripts/check-rust-target-error.cmake` + `.py`（17 项）** —— 见下。

**方法论：全量 diff 两套本地 Qt 头文件**

不要靠撞编译错误来找同类缺陷。做法是**扫项目实际派生的 Qt 基类**，
把两版头文件里的虚函数签名逐一比对。工具在
`.workbuddy/uos-port/qt-override-scan.py`（本地，不入库）。

扫项目派生的 **41 个** Qt 基类，结果：

| 虚函数 | 结论 |
|---|---|
| `QMimeData::retrieveData` | **真缺陷** ⇒ 已修（本文） |
| `QAbstractNativeEventFilter::nativeEventFilter` | 已是版本无关（既有垫片 `snowNativeEventResult`），`pinnedwindowplatform.cpp:271` 在 `#if QT_VERSION < 6` 分支内用 `long*` ⇒ 正确 |
| `QIODevice::open` | 项目**未覆盖** ⇒ 无影响 |
| `QThread::run` | **假阳性**（两版 `qthread.h` 都有 `virtual void run();`）⇒ 类名归属被内嵌类污染 |

**这个 diff 工具自己也踩了两次假阴性**：

1. **逐行解析漏检跨行声明** —— Qt5 的 `retrieveData` 声明**跨两行**
   ⇒ 改成按 `;` 切分语句。
2. **切分后忘了把 `;` 加回去** ⇒ lookahead `(?=[;{])` 再也匹配不到
   ⇒ **0 差异的假阴性**（比假阳性更危险：它说"没问题"）。
   ⇒ 切完必须把 `;` 补回每个 chunk。

⇒ **判据"没报错"和判据"报对了"要分开验证**。门禁的红是"有差异"，
但**门禁的绿不能证明没有差异** —— 除非做过反向验证。

#### 9.6.19.1 附带修掉的门禁：Rust target 探针的错误消息（run 15 遗留）

run 15 的 arm64 flake（见 9.6.17）让 CMake 报了
"Rust target ... is not installed or usable" —— **错误指向了 target，而病根在工具链安装**。
本轮把 `cmake/RustStaticLibrary.cmake` 的那段消息重写了：

- **Linux 分支给 rustup 命令**（不是 `bootstrap.ps1` —— 那是 Windows 脚本，Linux 上没有）；
- **Windows 分支保留 `bootstrap.ps1`**；
- **把 cargo 自己的输出带上**（原来 `ERROR_QUIET` 直接丢掉了），
  消息里出现 "cargo said:" + cargo 的原文。

**门禁做法**：`scripts/check-rust-target-error.cmake` 从
`cmake/RustStaticLibrary.cmake` 里**真的把那段代码抽出来执行**
（**不是抄一份** —— 抄的那份测的是抄件，不是代码）。

**抽取的实现难度远超预期**：

- CMake **不能捕获 `message(FATAL_ERROR)`** ⇒ 夹具必须**自己被杀掉**，
  文本落到 stderr，退出码非零 ⇒ 断言交给**外部 Python 驱动器**。
  （夹具自调用子进程会触发本机沙箱的 SIGTERM，CI 上同样脆弱。）
- **不能按行切分模块文本** —— 那会破坏块内的多行字符串字面量
  （CMake 靠相邻字面量隐式拼接）。
  ⇒ 改用 `string(FIND)` + `string(SUBSTRING)` 在**原始文本**上定位。
- 找配对的 `endif()` 要**数嵌套**（`if` / `endif` 逐个匹配），
  且**首个 token 就是守卫自己**（初始深度已经是 1）⇒ 要跳过首个 token。
- `list(GET)` 是 0-based，`while(_index LESS _line_count)` + 先自增 ⇒ **越界**。

**修完还暴露了生产代码的一个真缺陷**：`set(_rust_remedy "..." "--target ...")`
是 **`set()` 的列表语义**（两个值用 `;` 连接），不是隐式拼接 ⇒
消息里真的出现了 `;--target x86_64-unknown-linux-gnu`。
⇒ 改成 `string(CONCAT _rust_remedy "..." "--target ${SNOW_RUST_TARGET}")`。

**判据（17 项全绿）** 里专门加了两条抓这个：
"linux message carries the full rustup --target flag" +
"linux message has no list separator in the remedy"。

**反向验证 2/2 精准变红**：

| 变异 | 变红的判据 |
|---|---|
| Linux 分支改回 `bootstrap.ps1` | **恰好 2 条**（linux rustup 命令 + 不含 bootstrap.ps1），Windows 侧不受影响 |
| 恢复 `ERROR_QUIET`（丢 cargo 输出） | **恰好 1 条**（linux 含 cargo stderr 文本） |
| `string(CONCAT` 改回 `set()` 两参数 | **恰好 2 条**（linux + unix 的分号判据） |

**Unix `.sh` 分支在本机也被完整验证**：Windows 上 `execute_process` 不能直接跑 `.sh`
⇒ 找 Git Bash 的 `sh.exe`，用 `.bat` 包装驱动它 ⇒ **CI 的 Unix 分支不再是
"只会在 CI 上跑"的那一条**。

**夹具的落盘位置也修了一处**：`cmake -P` 模式下 `CMAKE_CURRENT_BINARY_DIR` 就是 cwd
⇒ 夹具每次运行都会在**仓库根**扔一个 `fake-cargo/` 目录。
⇒ 改到系统临时目录（`$TMPDIR` / `$TEMP`）。

**判据（可复用）**：

- **"Windows 编得过"不等于"Linux 编得过"** —— 平台守卫会让整类代码在一边完全不编译。
  想关闭这类盲区，就往**每个 TU 都会 force-include 的那个头**里放 probe。
- **CMake 的 `set()` 多参数是列表语义**，`string(CONCAT)` 才是拼接。
  错误消息里出现 `;` 就是这个。
- **"门禁的绿"要靠反向验证来担保**：一个从不报错的判据和一个正确的判据，
  在基线上长得一模一样。
- **工具的假阴性比假阳性更危险**：假阳性会逼你查，假阴性会让你以为安全。

#### 9.6.20 run 18：OpenCV 的两处 API 缺口，与"同一份补丁"这条硬约束（2026-10-22）

run 18（`39745852`）把红灯推到了**条码识别服务**上。x64 与 arm64 同时停在
`[519/611]`，错误只有两类：

```
screenshotqrrecognitionservice.cpp:60:17: error: 'barcode' is not a member of 'cv'
/usr/include/opencv4/opencv2/wechat_qrcode.hpp:36:13: note: candidate:
  'cv::wechat_qrcode::WeChatQRCode::WeChatQRCode(const std::string&, ...)'
/usr/include/opencv4/opencv2/wechat_qrcode.hpp:36:45: note: no known conversion for
  argument 1 from 'const std::vector<unsigned char>' to 'const std::string&'
```

编译命令里的 `-isystem /usr/include/opencv4` 说明用的是**发行版的 OpenCV 4.6**。

**诊断（三个事实，每个都要落到上游源码上，不能靠印象）：**

1. **`cv::barcode` 在 4.12 的主仓 `objdetect` 里**，不在 contrib：
   `modules/objdetect/include/opencv2/objdetect/barcode.hpp` 提供
   `decodeWithType(img, points, decoded_info, decoded_type)` —— 与本工程
   `screenshotqrrecognitionservice.cpp:200` 的调用逐字吻合。它是 **4.8.0** 才从
   contrib 搬进主仓的（4.7.0 的 objdetect 里还没有 `barcode.hpp`），
   所以 4.6 的 contrib `barcode` 是**另一套 API**
   （`decode()` + `BarcodeType` 枚举），住在 `opencv_contrib` 里；
   而 contrib 在 4.12 **已经把 barcode 模块删了**。
2. **`WeChatQRCode` 的 `std::vector<uchar>` 四参构造不是上游 API**：
   它来自本仓库自打的
   `cmake/vcpkg-overlay-ports/opencv4/0024-contrib-wechat-qrcode-in-memory-models.patch`
   （新增四参构造 + `SSDDetector::init(vector,vector)` + `SuperScale::init(vector,vector)`）。
   ⇒ 上一轮"这个构造是 4.12 才有的"的结论**作废**：上游任何版本都没有，
   只有 vcpkg 侧有。
3. 4.12 的 1D 条码解码器是**自实现**的
   （`modules/objdetect/src/barcode_decoder/{abs,ean8,ean13,upcean}_decoder.cpp`），
   `3rdparty/` 里**没有 zxing** ⇒ 本地编 OpenCV 的外部依赖面比预想小。

**为什么不给代码打条件编译（修法 B）：**

- 4.6 下要么**丢掉 1D 条码**，要么改用 `decode()` + 枚举并**手写枚举→字符串映射**；
- 而 `readModelFile()` 用 `QFile` 读模型进 `std::vector<uchar>`，注释写明这是为了
  绕开 OpenCV 的窄字符路径处理（Windows 非 ANSI 路径）——退到 path 构造会
  **只在 Linux 上**丢掉这个能力。

⇒ 两个平台不再是同一个程序，违背本工程"两端同源 ⇒ 行为一致"这条底线。

**决定（修法 A）：Linux 侧本地固定版本编 OpenCV 4.12.0 + opencv_contrib 4.12.0，
打**同一份 0024 补丁**，与既有的 11 个 pinned 库同构。

实现要点（`scripts/build-linux-deps.sh` 的 `build_opencv()`）：

- **两个归档、两个校验和**：opencv 4.12.0 与 opencv_contrib 4.12.0，SHA512 与
  `cmake/vcpkg-overlay-ports/opencv4/portfile.cmake` 里两个 `vcpkg_from_github`
  块**逐字一致**。
- **同一份补丁**：对 contrib 源码树 `patch -p1` 应用 0024（补丁路径就是
  `modules/wechat_qrcode/...`，与 contrib 源码根的布局一致）；没有 `patch` 就退回
  `git apply`。**打不上就要停** —— 没有那个构造，服务根本编不过。
- **模型离线化**：contrib 的 `wechat_qrcode` 在 *configure 期* 下载 4 个模型。
  脚本按同一批 URL / SHA512 预先取回，放进 OpenCV 自己的下载缓存，
  文件名是 `ocv_download` 认的那一套（`<md5>-<filename>`，在
  `<OPENCV_DOWNLOAD_PATH>/wechat_qrcode/` 下）⇒ configure 期**完全不联网**，
  也不会去等那个 600 秒的下载超时。MD5 也一起校验：它只是缓存键，
  错了不会让构建失败，只会让 configure 偷偷回网下载。
- **静态构建**（`-DBUILD_SHARED_LIBS=OFF`），三个理由：
  ① 与 Windows 侧一致（overlay port 是 `vcpkg_check_linkage(ONLY_STATIC_LIBRARY)`）；
  ② .deb 要在 UOS 20 上跑，而 UOS 20 的 OpenCV 更旧 —— 共享的
  `libopencv_core.so.4.12` 只能塞进包里，成为"没有包拥有的共享库"；
  ③ `dpkg-shlibdeps` 从 ELF NEEDED 推导 Depends，一个不属于任何发行版包的
  共享库会让这一步**报错**而不是警告。
- **窄化模块集**：`-DBUILD_LIST=wechat_qrcode`（与 overlay port 一致），
  只拉 core / imgproc / objdetect / dnn / contrib 模块。
- **断言的是产品结果，不是代理**：
  `share/opencv4/OpenCVConfig.cmake`、`opencv2/objdetect.hpp`、
  `opencv2/wechat_qrcode.hpp` 三个文件存在，**并且**
  `objdetect/barcode.hpp` 里有 `decodeWithType`、
  `wechat_qrcode.hpp` 里有 `const std::vector<uchar>& detector_prototxt_buffer`
  —— 4.6 的安装**文件齐全但这两条都没有**，所以"文件存在"这种代理判据不够。

**门禁（两条，都做了反向验证）**：

| 门禁 | 判据 |
|---|---|
| `scripts/check-linux-deps-pins.py` | ① opencv 的两个归档**分别**按 REPO 比对（"portfile 里第一个 SHA512"这条规则在这里是**错的** —— 第一个是补丁文件的校验和）；② 脚本引用的补丁文件与 vcpkg port 应用的是**同一个**；③ 脚本 pin 的 4 个 QR 模型与 `cmake/FetchSnowShotQrModels.cmake` **逐字一致** |
| `scripts/check-linux-ci-workflow.py` | ① apt 列表里**不得**再有 `libopencv-dev`（两个来源 = 构建失败时静默回落）；② 必须装 `patch`；③ `build-linux-deps.sh` 必须构建 opencv、应用那份补丁、且**静态**构建 |

**反向验证（12 处变异，全部精准变红）**：

- pins 门禁 7 处：主仓 SHA512 / contrib SHA512 / 主仓版本 / contrib 版本 /
  补丁引用 / 某个模型的 SHA512 / port 不再应用补丁 —— 每处只红 1 条，
  且红的那条正是被改的那条；
- workflow 门禁 5 处：把 `libopencv-dev` 加回 apt / 去掉 `patch` /
  从构建循环里去掉 `opencv` / 换掉补丁名 / 去掉 `-DBUILD_SHARED_LIBS=OFF`
  —— 同样各红 1 条。

**本机可做的取证（提交前已经做完，不能只靠"印象"）**：

| 待证事实 | 取证方式 | 结果 |
|---|---|---|
| 4 个 QR 模型的 SHA512 与 MD5 | 实下载 4 个文件实算 | SHA512/MD5 **与 pin 逐字一致**，且 MD5 与 contrib `CMakeLists.txt` 里的 `hash_*` 一致 ⇒ 缓存键也对 |
| 0024 补丁能否打在 contrib 4.12 上 | 取 contrib 4.12 的 6 个真实文件，`patch --dry-run -p1` | **6 个 hunk 全部干净应用**（exit 0，无 fuzz） |
| 补丁后的头文件是否含断言要查的那一行 | 打完后 `grep` | `const std::vector<uchar>& detector_prototxt_buffer` **命中** |
| `BUILD_LIST=wechat_qrcode` 会不会把 `objdetect` 排除 | 读 4.12 `cmake/OpenCVModule.cmake` 的白名单展开逻辑 | 白名单沿 `REQ_DEPS`/`PRIVATE_REQ_DEPS` **传递展开**，而 contrib 的 `wechat_qrcode` 显式 `ocv_define_module(... opencv_objdetect ...)` ⇒ objdetect 必在 |
| `cv::barcode` 到底在哪个模块 | 列 4.12 `modules/` 顶层目录 | **没有 `barcode` 模块**；它在 `objdetect` 里（`modules/objdetect/include/opencv2/objdetect/barcode.hpp` + `src/barcode_decoder/`）⇒ 本工程链的 `opencv_objdetect` 就是对的 |
| "4.9 才搬进主仓"这个说法 | 逐版探测 `modules/objdetect/include/opencv2/objdetect/` | **4.7.0 没有 `barcode.hpp`，4.8.0 就有** ⇒ 搬进来的版本是 **4.8.0**，不是 4.9 |
| contrib 4.6 的 barcode 是什么样 | 取 contrib 4.6.0 的 `modules/barcode/include/opencv2/barcode.hpp` | 确有该模块，但是 `decode()` + `enum BarcodeType`（`std::vector<BarcodeType>`），**不是** `decodeWithType` ⇒ 与本工程的调用确实不兼容 |
| contrib 4.12 还有没有 barcode | 探测 contrib 4.12.0 的 `modules/barcode/` | **404（已删除）** ⇒ contrib 4.12 只有 `wechat_qrcode` 一个与扫码相关的模块 |
| 项目侧要不要额外 include | 读 4.12 `objdetect.hpp` 的 include 列表 | 第 879 行 `#include "opencv2/objdetect/barcode.hpp"` ⇒ 现有的 `#include <opencv2/objdetect.hpp>` 已经够 |
| `ocv_download` 的缓存文件名规则 | 读 4.12 `cmake/OpenCVDownload.cmake` | `CACHE_CANDIDATE = ${OPENCV_DOWNLOAD_PATH}/${id 小写}/${MD5}-${FILENAME}` ⇒ 与脚本的 `<md5>-<file>` 逐字一致 |
| protobuf 从哪来 | 读 4.12 `cmake/OpenCVFindProtobuf.cmake` 与 `3rdparty/` 列表 | `BUILD_PROTOBUF=ON` ⇒ 用 OpenCV 自带的 `3rdparty/protobuf`（存在）；`WITH_FLATBUFFERS=OFF` 关掉 TFLite ⇒ 不需要 flatbuffers |

另外两条**修正**：

- `WITH_ZLIB_NG` 在 4.12 的默认值就是 OFF，overlay port 里的
  `-DWITH_ZLIB_NG=OFF` 是显式重申，不是额外要求 ⇒ 脚本不加它也不构成差异。
- `OpenCVConfig.cmake.in` 模板里**没有** `find_dependency(Threads)`；
  overlay port 的 portfile 里有 `find_dependency(Threads)` 是 **vcpkg 侧**的
  依赖声明（vcpkg 会用它拼 `usage`），不是 OpenCV 导出的必需项 ⇒
  Linux 侧不走 vcppkg 也不需要它。（OpenCV 4.12 在 UNIX 上直接把
  `pthread/dl/m/rt` 当普通链接名传给链接器，不产生 `Threads::Threads` 依赖。）

**风险与遗留**：

- **CI 时间**：OpenCV 4.12 + dnn + 自带 protobuf 是那一步里最贵的库。
  run 18 的 x64 build job 已经用了 33 分钟（上限 120），
  ⇒ 超时**从 120 提到 180**。
- **.deb 侧**：静态 OpenCV 不进 ELF NEEDED，`dpkg-shlibdeps` 不受影响；
  但 `libopencv-dev` 从 apt 列表里摘掉后，发行版那份 4.6 也不再存在 ——
  这是有意的（见上面的判据 ①）。
- **静态导出的 imported target**：`find_package(OpenCV CONFIG REQUIRED)` 之后链
  `opencv_core` / `opencv_imgproc` / `opencv_objdetect` / `opencv_wechat_qrcode`
  这四个 imported target —— Windows 侧的 vcpkg overlay port 走的是同一套导出
  且已证明可用，但 Linux 侧是**裸 CMake 安装**，`OpenCVModules.cmake` 由
  `install(EXPORT OpenCVModules ...)` 生成，target 名与链接项仍需 CI 实测。
- 真正的验证仍然只能来自下一次 CI：**本机没有 Linux 编译器**，
  `build_opencv()` 的 configure/build 一次都没在本机跑过。

#### 9.6.21 run 19 / 20：`ENABLE_CONFIG_VERIFICATION` 的 16 项错配，与一条从未执行过的转储（2026-10-22）

run 19（`39764499751`）和 run 18 不是同一个病：x64 与 arm64 都停在**依赖脚本**里，
整段日志只有一行：

```
error: opencv installed no OpenCVConfig.cmake
```

##### 25 秒到底是不是"来不及编译"

| 时刻 | 事件 |
|---|---|
| `10:46:28.924` | libheif 安装完成（`libheif-config-version.cmake`） |
| `10:46:53.926` | `error: opencv installed no OpenCVConfig.cmake` |

中间 **25 秒、零输出**。我最初据此判断"下载+解包+configure+编译装不下 25 秒，
所以失败在下载阶段"——**这个判断是错的**，run 20 的日志把它推翻了：那一轮
`disk before opencv` 到 configure 报错是 `13:45:16.149 → 13:45:39.035`，
**22.9 秒**，而这 22.9 秒里包括了下载 90 MB 主仓、解包、打补丁和完整的 configure。
所以 25 秒恰恰是"configure 一路跑到 `ocv_verify_config()` 然后失败"的正常耗时，
不是"根本来不及"。同一段里真正成立的部分只有一句：`no OpenCVConfig.cmake` 不是
病因，它是 `build_opencv()` 末尾的断言（`scripts/build-linux-deps.sh:961`），
在 configure 早已死掉之后才轮到它说话。

##### 真因：`-DENABLE_CONFIG_VERIFICATION=ON` + 11 项选项错配

这个开关是从 overlay port 抄来的（同一份源码、同一组选项）。打开后，configure
结束前 OpenCV 会跑 `ocv_verify_config()`（`cmake/OpenCVUtils.cmake`）：

```cmake
foreach(var ${OPENCV_VERIFICATIONS})
  set(evaluated FALSE)
  if(${OPENCV_VERIFY_${var}})
    set(evaluated TRUE)
  endif()
  if (${var} AND NOT evaluated)      # 选项 ON，依赖没找到
    list(APPEND broken_options ${var})
  elseif(NOT ${var} AND evaluated)   # 选项 OFF，依赖却被找到
    list(APPEND broken_options ${var})
  endif()
endforeach()
```

两支都会 `FATAL_ERROR`。HEAD 那套选项集（run 19 实际用的）在这台机器上错 11 项：

```
WITH_AVIF  WITH_VTK  WITH_EIGEN  WITH_GSTREAMER  WITH_IPP  WITH_JASPER
WITH_OPENJPEG  WITH_OPENCLAMDFFT  WITH_OPENCLAMDBLAS  WITH_VA  WITH_VA_INTEL
```

run 20（`39788745221`，本轮的修法）终于把这段 cmake 错误转储出来了，只剩一项：

```
CMake Error at cmake/OpenCVUtils.cmake:801 (message):
  Some dependencies have not been found or have been forced, unset
  ENABLE_CONFIG_VERIFICATION option to ignore these failures or change
  following options:

  WITH_TESSERACT
error: opencv build failed (log: .../logs/opencv.log)
```

`WITH_TESSERACT` **不在 OpenCV 自己的树里**，它由
`opencv_contrib/modules/text/cmake/init.cmake` 声明：

```cmake
OCV_OPTION(WITH_TESSERACT "Include Tesseract OCR library support" (NOT CMAKE_CROSSCOMPILING)
  VERIFY HAVE_TESSERACT)
```

默认值 `(NOT CMAKE_CROSSCOMPILING)` 在原生构建下就是 **ON**，而镜像里没有
`libtesseract-dev`。复刻实验漏掉它，是因为实验只喂了**根** `CMakeLists.txt` 的
`OCV_OPTION` 块 —— 而 `ocv_glob_modules()` 的第一遍对**每个**模块 include
`cmake/init.cmake`，contrib 的 `text` 虽然没被构建，它的 init 照样执行。
contrib 里只有三个这样的文件（`julia` / `matlab` / `text`），前两个的选项默认 OFF
且**没有** `VERIFY`，所以真正会咬人的只有这一个。

##### 为什么真正的报错没进日志

`configure_build_install` 在 HEAD 里**没有 `|| die`**，而调用点写的是
`cmd || { cat "$log"; die ...; }` —— `||` 的左侧在 `errexit` 下被放行，
`die` 又直接 `exit 1` 退掉整个脚本，于是 `cat "$log"` 从来没有执行过：
**设计上"失败时转储日志"的那段代码是死代码**，ci 上只剩最末端那句断言。

修法（本轮）：`fetch` / `unpack` / `apply_patch` / `configure_build_install` 各自
`|| die`；每个库在子 shell 里构建，失败时 `dump_log` 把该库的日志尾部打到 fd 3；
`die` 保留 fd 3 而不是 `stderr`（`stderr` 已被 `2>&1 | tee` 收走）。

##### 怎么把"必须显式 OFF 的集合"算准

人工推理在这一步反复出错，所以改成**复刻实验**：把上游的 `OCV_OPTION` 宏与
`ocv_verify_config()` **逐字提取**（不是手抄）到 `lab_real.cmake`，用本机 cmake
4.2.3 跑 `-P`，喂三样东西 —— 根 `CMakeLists.txt` 里全部 `OCV_OPTION` 块（逐字）、
Linux/x86_64 平台变量模型、按 workflow apt 清单推出的 `HAVE_*` 结果集。
让 cmake 自己算出错配集合：

| 输入 | 结果 |
|---|---|
| 不给任何 `-D`（纯默认） | **15 项**：`AVIF EIGEN GSTREAMER GTK IPP ITT JASPER LAPACK OPENCL OPENCLAMDBLAS OPENCLAMDFFT OPENJPEG VA VA_INTEL VTK` |
| run 19 的选项集 | **11 项**（上表） |
| 本轮修后 | `VERIFY_RESULT: OK` |

实验还有一个**结构性盲区**，由 run 20 补上：它只喂了根 `CMakeLists.txt` 的
`OCV_OPTION` 块，因此看不见 contrib 的 `cmake/init.cmake` 声明的那一项。
把 `WITH_TESSERACT` 补进去之后，真正"必须显式 OFF"的集合是 **16 项**，
门禁和脚本都按 16 项收敛。

实验过程本身纠正了两个结论，值得记下来：

- **`VERIFY` 选项是 84 项，不是 70 项。** 根 `CMakeLists.txt` 里有 7 项（`WITH_EIGEN`
  `WITH_FFMPEG` `WITH_IPP` `WITH_VA` `WITH_VA_INTEL` `WITH_OPENCL` `WITH_OPENCLAMDFFT`）
  是**跨行写法**，`OCV_OPTION(WITH_X "..." (EXPR` 换行后才写 `VISIBLE_IF` / `VERIFY`，
  单行正则全部漏掉 —— 漏项会让"默认 ON 却判不出来"。
- **"模块不在 `BUILD_LIST` 里，检测就不会跑"是错的。** `ocv_glob_modules()` 的第一遍
  （`cmake/OpenCVModule.cmake` 的 `_add_modules_1`）对**所有**模块无条件 include
  `modules/<m>/cmake/init.cmake`，videoio / highgui 的后端探测因此照样执行，
  `BUILD_LIST=wechat_qrcode` 挡不住。真正起作用的是 `add_backend(<id> <OPTION>)`
  里那个 `if(${OPTION})`。

由此得到**单向规则**：选项 OFF 永远自洽（不探测、标志位留空），只有"默认 ON 且依赖
不存在"才必须写出来。`WITH_FFMPEG` 正属于相反的一类：CI 装了
`libavcodec/libavformat/libavutil/libswscale`，`detect_ffmpeg.cmake` 用 pkg-config
找得到 ⇒ 标志位为真 ⇒ **`-DWITH_FFMPEG=OFF` 才是错配**，本轮把它去掉。

同一类里还有一条更隐蔽的：`-DWITH_ADE=OFF`。OpenCV 4.12 **没有 `WITH_ADE` 这个选项**
（90 个 `WITH_*` 里没有它），cmake 对未知 `-D` 一声不响 —— 这行等于没写。

##### 门禁：`scripts/check-linux-deps-opencv-options.py`

四条判据：15 项必须写 `=OFF`（缺一行就红）；`build_opencv()` 里每个 `-DWITH_*`
必须在 4.12 的 90 个声明里（防拼错、防上游改名）；`ENABLE_CONFIG_VERIFICATION`
必须与 overlay port 一致；`WITH_IPP` / `OBSENSOR_USE_ORBBEC_SDK` 必须 OFF
（两者都会在 configure 中途联网下载，`cmake/OpenCVFindIPP.cmake`、
`3rdparty/orbbecsdk/orbbecsdk.cmake`）。

反向验证 `.workbuddy/uos-port/revval-opencv-options.sh` 五条：删 `-DWITH_VTK=OFF`、
改 `=ON`、插回 `-DWITH_ADE=OFF`、`-DWITH_IPP` 翻 ON、去掉
`-DENABLE_CONFIG_VERIFICATION` —— 每条都必须**只**为那一件事变红，恢复后必须回绿。

##### 闭环与代价

run 20 的日志给出了答案，而且答案的形状值得记一笔：**11 项错配修完后只剩 1 项**，
而这一项来自模型完全没看的地方（contrib）。这正是"进门禁"而不是"改到绿"的理由 ——
`WITH_TESSERACT` 现在是 `scripts/check-linux-deps-opencv-options.py` 的 16 项之一，
下次谁把 `-DWITH_TESSERACT=OFF` 删掉，本地门禁就会先红。

磁盘也不是瓶颈：run 20 的 `report_free_space` 报的是 **109 GB free**，
`require_free_space` 的 1–4 GB 阈值只是防"树被撑爆"，不是常态。

#### 9.6.22 run 21：deps 第一次全绿，红落到一个没有 `VERIFY` 的选项上（2026-10-22）

run 21（`39793895672`）第一次让 **两个架构的 deps 步骤同时变绿** ——
OpenCV 4.12 的 configure、编译、安装整条链路走通，整个 deps 步骤 3.5 分钟
（`13:42:12` 起，`13:45:16` 进 opencv）。红灯往后挪到我们自己的 Configure：

```
CMake Error at .../share/opencv4/OpenCVModules.cmake:137 (message):
  The imported target "ade" references the file
     ".../lib/manual-link/opencv4_thirdparty/libade.a"
  but this file does not exist.
Call Stack:
  .../share/opencv4/OpenCVConfig.cmake:128 (include)
  snow_draw_engine_qt/CMakeLists.txt:33 (find_package)
```

##### 为什么 `ocv_verify_config()` 看不见它

`modules/gapi/cmake/init.cmake`：

```cmake
OCV_OPTION(WITH_ADE "Enable ADE framework (required for Graph API module)" ON)
...
if(NOT WITH_ADE)
  return()
endif()
if(NOT TARGET ade)
  include("${CMAKE_CURRENT_LIST_DIR}/DownloadADE.cmake")
endif()
```

三个条件叠加出这个坑：

1. **没有 `VERIFY` 子句** ⇒ 不进 `OPENCV_VERIFICATIONS`，verification 一声不响；
2. **默认 ON**，而它是模块级 `init.cmake`，`ocv_glob_modules()` 第一遍照样执行，
   `BUILD_LIST=wechat_qrcode` 挡不住；
3. `DownloadADE.cmake` 会真的把 ADE 下下来并建出 `ade` 目标，而 `install(EXPORT
   OpenCVModules ...)` 把它写进导出表 —— 但**编译安装 `libade.a` 的是 gapi**，
   gapi 不构建，于是归档根本不存在。

结果是一个"构建全绿、包不可用"的失败：`cmake --install` 不报错，
`find_package(OpenCV CONFIG)` 才炸，而且炸在我们自己的 `snow_draw_engine_qt` 里。

##### 修法，与一次自己造成的回退

`-DWITH_ADE=OFF`（overlay port 传的也是这一条，vcpkg 能过正是因为它）。

这条在上一轮被我**删掉过**：新门禁的"死选项"检查拿根 `CMakeLists.txt` 的 90 个
`WITH_*` 当全集，`WITH_ADE` 不在里面，于是被误判为"cmake 会静默忽略的名字"。
真正的事实是它由 **gapi 的 `cmake/init.cmake`** 声明 —— 又是"模块级 init.cmake
也参与"，和 `WITH_TESSERACT` 同一个盲区，只不过一个炸 verification、一个炸导出。

所以门禁也补了两处：

- 声明表 90 → **98**（根 90 + contrib 的 `julia`/`matlab`/`tesseract` +
  gapi 的 `ADE`/`FREETYPE`/`GAPI_ONEVPL`/`OAK`/`PLAIDML`）；
- 新增 `REQUIRED_OFF_UNVERIFIED`：**没有 `VERIFY`、但会让安装出来的包不可用**
  的选项。这一类的判据不可能来自 `ocv_verify_config()`，只能来自"消费者视角"。

反向验证加到 7 条：删 `-DWITH_ADE=OFF` 必须只为"WITH_ADE 必须 OFF"变红。

#### 9.6.23 run 22：`OpenCVConfig.cmake` 从不 `find_dependency`，消费方得自己补（2026-10-22）

run 22（`39798623715`）的日志里 `ade` 那条彻底消失，说明 9.6.22 的修法成立。
新的第一条 cmake 错误换成了：

```
CMake Error at .../share/opencv4/OpenCVModules.cmake:112 (set_target_properties):
  The link interface of target "opencv_wechat_qrcode" contains:

    Iconv::Iconv

  but the target was not found.
Call Stack:
  .../share/opencv4/OpenCVConfig.cmake:128 (include)
  snow_shot/CMakeLists.txt:367 (find_package)
```

##### 为什么只有 Linux 会碰到

`cmake/templates/OpenCVConfig.cmake.in` 里**一个 `find_dependency` 都没有**：
它只负责 include 出 `OpenCVModules.cmake`，而那份文件里每个
`INTERFACE_LINK_LIBRARIES` 引用的第三方导入目标，都要求消费方**事先**定义好。

- Windows / macOS 的 OpenCV 来自包管理器，它的 config 自己把这些目标建好了；
- Linux 这份是我们自己 `cmake --install` 出去的静态构建，`Imagecodecs` 相关模块
  没进 `BUILD_LIST`，真正被带出去的外部导入目标只有 `Iconv::Iconv`
  （由 `wechat_qrcode` 的链接接口带入）。

于是同一行 `find_package(OpenCV CONFIG REQUIRED)` 在 Windows 上一直可用，
在 Linux 上第一次真的把 OpenCV 装出来就炸 —— 又一个"Windows 永不复现"的盲区。

##### 修法

在根 `CMakeLists.txt`、所有 `add_subdirectory` 之前：

```cmake
if(UNIX AND NOT APPLE)
    find_package(Iconv)
    ...
endif()
```

glibc 的 iconv 在 libc 里，`FindIconv` 给的是一个不链任何东西的 INTERFACE 目标，
所以"让目标存在"本身就是全部修法。位置也有讲究：导入目标只对**之后**加入的目录
可见，放在 `add_subdirectory(snow_draw_engine_qt)` 之后就没有用了。

##### 门禁：`scripts/check-linux-opencv-imports.py`

判据是"消费者视角"的三条：`Iconv::Iconv` 必须在根 CMakeLists 里有定义、
必须在两个消费目录之前、必须被 `if(UNIX ...)` 守着。
`REQUIRED_TARGETS` 是登记表 —— 如果静态导出的接口里再冒出一个目标，
往表里加一行即可，不用重新考古。反向验证三条（删掉定义 / 挪到消费目录之后 /
去掉平台守卫），每条都必须只为那一件事变红。

---

#### 9.6.24 run 23：deps 与 Configure 双双全绿，红落到一个 `PRIVATE` 的 Qt 模块上（2026-10-22）

##### 里程碑

run 23（`39801467842`，`b59adff4`）第一次让前两个步骤在两架构同时变绿：

| 步骤 | x64 | arm64 |
|---|---|---|
| 4. Build the pinned Linux dependencies | success | success |
| 9. Configure | success | success |
| 10. Build | **failure** | **failure** |

`WITH_TESSERACT`（9.6.21 / 9.6.22）与 `Iconv::Iconv`（9.6.23）两处修法由此确认成立。
Build 在 540/608 编译单元处停住，两架构错在同一行：

```
snow_shot/include/snow_shot/platform/linux/statusnotifierservice.h:6:10:
fatal error: QDBusObjectPath: No such file or directory
    6 | #include <QDBusObjectPath>
```

##### 定因：编译命令里少了三个字符

失败那行的完整编译命令是一份决定性的证据。它的 Qt 相关标志是：

```
-DQT_CONCURRENT_LIB -DQT_CORE_LIB -DQT_GUI_LIB -DQT_MESSAGE_LOGCONTEXT
-DQT_NETWORK_LIB -DQT_NO_DEBUG -DQT_SVG_LIB -DQT_WIDGETS_LIB
-isystem /usr/include/aarch64-linux-gnu/qt5/QtGui
-isystem /usr/include/aarch64-linux-gnu/qt5/QtWidgets
-isystem /usr/include/aarch64-linux-gnu/qt5/QtCore
-isystem /usr/include/aarch64-linux-gnu/qt5/QtNetwork
-isystem /usr/include/aarch64-linux-gnu/qt5/QtSvg
```

`QT_DBUS_LIB` 没有，`.../qt5/QtDBus` 也没有 —— QtDBus 的**使用需求**（use requirements）
一个都没到。而头文件 `<QDBusObjectPath>` 需要 `-isystem .../qt5/QtDBus`，因为
`QDBusObjectPath` 是 QtDBus 模块独有的头，不在 qtbase 的 `include/` 根目录下。

来源在 `snow_shot/CMakeLists.txt`：

```cmake
target_link_libraries(snow_shot_status_notifier PUBLIC Qt::Core Qt::Gui
    snow_shot_status_notifier_payload
    PRIVATE Qt::DBus)          # ← 错在 PRIVATE
```

`statusnotifierservice.h` 是这个库的**公共头**，第 6 行 `#include <QDBusObjectPath>`；
`systemnotificationservice.h` 同样如此（`#include <QDBusConnection>`）。公共头里的
模块必须是公共依赖。

##### 为什么这个错看起来不像错

静态库的 `PRIVATE` 依赖**在链接期确实会传播**：CMake 把静态库的 PRIVATE 依赖包成
`$<LINK_ONLY:...>` 塞进消费方的 `INTERFACE_LINK_LIBRARIES`，链接命令里仍然会出现
`libQt5DBus.so`。configure 一路绿灯，链接也不会报错 —— 丢掉的只有 `-isystem`、
`-DQT_DBUS_LIB` 这类**编译期**使用需求。于是症状不是"找不到库"，而是"找不到头"。

这一条正是它能在代码里躺这么久的原因：`snow_shot` 是 `PRIVATE` 链的
`snow_shot_status_notifier`，链接期一切正常，编译期的缺口只落在"碰巧包含了这个公共头"
的翻译单元上。

##### 为什么 Windows / macOS 永远复现不了

`snow_shot/include/snow_shot/platform/linux/` 下的两个头只被
`systemtraycontroller.cpp` 与 `system_tray_controller_tests.cpp` 包含，而这两处都在
Linux 守卫里；头本身也是 Linux 分支才编进库的。**能编译它的平台不消费它，
消费它的平台才第一次编译它** —— 与 9.6.19（`QMimeData::retrieveData`）同一个结构性盲区，
只是这次的对象是 `target_link_libraries` 的可见性关键字而不是虚函数签名。

##### 修法

```cmake
# 两个库同样处理：公共头里有 QtDBus 的类型，所以 Qt::DBus 是公共依赖
target_link_libraries(snow_shot_status_notifier PUBLIC Qt::Core Qt::Gui
    snow_shot_status_notifier_payload
    Qt::DBus)
```

`systemnotificationservice.h` 一并修 —— 它在同一个块的下一行，同一类错误，
只是编译顺序让 `QDBusObjectPath` 先暴露出来。

##### 新门禁抓到的第二处：`snow_shot_translation`

新门禁第一次跑起来就在既有代码里抓到一个同类缺口：

```
FAIL: snow_shot_translation: public header
      snow_shot/include/snow_shot/network/snowshotapiclient.h includes ['QImage'],
      but Qt::Gui is not linked at all
```

这个库只链了 `Qt::Network`（它传递 `Core`，但不传递 `Gui`），而它的公共头里有
`<QImage>`。当前没炸是因为所有消费方自己都链了 `Qt::Gui` —— 属于"运气成立"，
不是"契约成立"。修法是往 PUBLIC 里补 `Qt::Gui`。

##### 门禁：`scripts/check-linux-qt-module-visibility.py`

判据一句话：**静态库的公共头包含了某个 Qt 模块的头，那这个模块必须在它的
PUBLIC/INTERFACE 里**。三项实现决定：

| 决定 | 理由 |
|---|---|
| 只查 `STATIC` / `OBJECT` 库 | 可执行文件的 PUBLIC 无人消费，查它只会制造噪音；静态库的公共头才是被多个目标编译的东西 |
| `Core` 算被覆盖 | Qt 自己的模块依赖图：`Gui`→`Core`，`Widgets`→`Gui`+`Core`；要求显式写 `Qt::Core` 是冗余。表在 `QT_MODULE_IMPLIES` |
| 认不出的 Qt 头直接失败 | 静默跳过就是 9.6.21 里那条"从未执行过的转储"的同款：盲区必须响 |

裸形式 `target_link_libraries(t foo)` 按 CMake 语义当作 `PUBLIC`（非 imported 目标），
解析器对此有显式分支 —— 否则每个用裸形式写 `Qt::Core` 的目标都会被误报。

实测规模与输出：

```
Qt module visibility check passed (19 static libraries, 32 public headers, 103 Qt includes classified)
```

反向验证（`.workbuddy/uos-port/revval-qt-module-visibility.sh`）五条变异 + 一条对照：

1. `snow_shot_system_notifications` 的 `Qt::DBus` 退回 `PRIVATE` → 只为它变红；
2. `snow_shot_status_notifier` 同上 → 只为它变红；
3. `snow_shot_translation` 删掉 `Qt::Gui` → 只为它变红；
4. `snow_shot_main_window_skin` 的 `Qt::Widgets` 降为 `PRIVATE` → 它连带 `Gui`/`Core`
   一起失去覆盖，门禁对它报 5 行，全部属于同一个目标（反向验证按目标前缀断言，
   而不是"必须只有一行"，否则等于在断言错误的东西）；
5. 公共头里插入 `#include <QGraphicsScene>` → 命中"认不出"分支，只报这一条；
6. 对照：只加一行注释 → 必须仍然全绿。

**教训**：`PRIVATE` 在静态库上不是"私有"，是"链接期公开、编译期私有"。
这个不对称不会被 configure 发现，也不会被链接发现，只会被"另一个目标恰好编译了
这个公共头"发现 —— 而那个目标往往只在 CI 的另一条架构上存在。

---

#### 9.6.25 P7 slice 6：AppImage 进 CI，runtime 用固定版本 + 校验和（2026-10-22）

##### 为什么之前一直没接

AppImage 是 Linux 打包路径里**唯一一段从未执行过的代码**：工作机上没有
`appimagetool`，`cmake/SnowShotAppImage.cmake` 里每条 `COMMAND` 都只是被读过。
workflow 头部原先写着"等 `build` 转绿再接"，理由是不想把真实的编译错误藏在
一个 404 后面。run 23 之后 deps 与 Configure 两架构全绿，条件成立。

##### 上游选型：不是口味问题

| 上游 | 状况 |
|---|---|
| `AppImage/AppImageKit`（历史） | release 全标 `obsolete-*`；appimagetool 需要 libfuse2，当前发行版已不带 |
| `AppImage/appimagetool`（维护中） | **不自带 type-2 runtime**，不给 `--runtime-file` 就自己联网下一个 |

第二点决定了整个设计：**打包步骤联网既不 hermetic 也不可复现**，而且"它下到的
是哪一版"取决于上游那一刻的状态。所以 runtime 也自己取，并显式交过去。

`scripts/fetch-appimagetool.sh` 因此 pin 两样东西 —— `appimagetool 1.9.1` 与
`type2-runtime 20251108` —— 四份 sha256（两架构各一），下载后校验 ELF
`e_machine` 与 `AI\x02` magic。上游两个仓库都把资产挂在滚动的 `continuous`
tag 下**原地替换**，所以 checksum 不能钉在那个 tag 上：钉了会自己过期，且没有
人改过任何文件。

脚本的 stdout 只输出 `KEY=VALUE`（给 `$GITHUB_ENV`），进度行全部走 stderr ——
一行日志混进 stdout 就会变成一个畸形的环境变量。

##### `--runtime-file` 是可选参数

```cmake
set(_snow_appimage_runtime_args "")
if(SNOW_APPS_APPIMAGETOOL_RUNTIME)
    if(NOT EXISTS "${SNOW_APPS_APPIMAGETOOL_RUNTIME}")
        message(FATAL_ERROR
            "SNOW_APPS_APPIMAGETOOL_RUNTIME does not exist: ...")
    endif()
    set(_snow_appimage_runtime_args --runtime-file "${SNOW_APPS_APPIMAGETOOL_RUNTIME}")
endif()
```

保持可选是因为历史上的 AppImageKit 版自带 runtime；调用方只有那个工具时可以
留空。但路径写错必须 FATAL，"静默降级成联网下载"正是这一段要消灭的东西。

##### workflow 里的顺序是有含义的

```
Upload the package (deb)  →  Fetch appimagetool  →  Build the AppImage
                          →  Validate the AppImage  →  Upload the AppImage
```

`.deb` 是发行版安装路径真正用的产物，所以它先打包、先上传。第三方工具（及其
pin）失效时，当次发布至少还有可安装的产物 —— 这条顺序断言写进了门禁，不是注释。

`Build the AppImage` 那一步重新 configure 时**必须**带上
`-DSNOW_APPS_PACKAGE_SNOW_SHOT=ON`：AppDir 是用 `cmake --install` 铺出来的，
install 规则来自那套打包配置。脚本里 `SNOW_APPS_PACKAGE_APPIMAGE=ON` 只负责
挂上 `snow-shot-appimage` 目标。

apt 补了 `squashfs-tools` —— appimagetool 会去调 `mksquashfs`，它是另一个包。

##### 验产物，不验工具

`scripts/check-linux-appimage-build.py` 的判据全部落在**产物**上：

| 判据 | 它能挡住什么 |
|---|---|
| ELF `e_machine` 等于目标架构 | 嵌入了别架构的 runtime ⇒ 目标机上 `Exec format error`，而不是构建时失败 |
| 字节 8..10 是 `AI\x02` | 拿到的不是 type-2 AppImage |
| `--appimage-extract` 后结构完整 | 缺 AppRun / desktop / icon / 可执行 / 随包 `.so` |
| 包内 AppRun 与仓库审阅版逐字节一致 | 打包步骤改写入口脚本 |

`--appimage-extract` 不需要 FUSE，所以在 runner 上能跑；这也意味着**启动**仍然
没有被证明 —— 真机（UOS 20 / X11）上的 `--appimage-extract-and-run` 是另一件事，
写在 P7 真机清单里。

##### 门禁的扩展

- `check-linux-appimage.py` 增五条：模块接受 runtime、把 runtime 交给
  appimagetool、缺文件时 FATAL；fetch 脚本 `echo` 出两个变量、pin 的不是滚动
  tag、两架构各有 checksum。
  其中"`echo` 出变量"必须匹配 `echo "SNOW_APPS_APPIMAGETOOL_RUNTIME=${runtime_path}"`
  这一整行 —— 两个变量名在脚本的 usage 块里也出现，匹配文档文本会让门禁在
  emit 被删掉后仍然绿（反向验证第 5 条就是这么发现的）。
- `check-linux-ci-workflow.py` 增十一条：四个步骤都在、顺序正确、fetch 写
  `$GITHUB_ENV`、configure 的四个参数、install 规则仍开、验证脚本被调用、
  两架构的 ELF 机器名、`squashfs-tools` 在 apt 里、根 CMakeLists 有那个 option。
  顺带修了它自己的一个解析坑：`cmake --preset` 后面接续行反斜杠时，剥掉
  `${{ }}` 后正则会把 `\` 当成 preset 名 —— 现在 `[^-\\\s]` 排除它。

反向验证：`revval-appimage-workflow.sh` 六条（删验证步骤 / 把 fetch 挪到 .deb
上传之前 / 空掉 runtime 参数 / 关掉 install 规则 / 删 squashfs-tools / 把
aarch64 写成 arm64），`revval-appimage-bundle.sh` 五条（换掉 `--runtime-file` /
弱化 FATAL 文案 / 用滚动 tag / 删一份 checksum / 停掉 emit）。全部按"只为那一件
事变红"断言，恢复后必须重新绿。

---

#### 9.6.26 run 24：QtDBus 修法成立，红推到 594/608 的一个"不存在的平台能力"（2026-10-22）

##### 9.6.24 的修法确认成立

run 24（`39808314409`，`e0174299`）失败那一行的编译命令里，Qt 相关标志多了两个：

```
-DQT_DBUS_LIB ... -isystem /usr/include/aarch64-linux-gnu/qt5/QtDBus
```

`PRIVATE` → `PUBLIC` 之后，QtDBus 的 include 目录确实到了消费方。同一处代码此前
报的是 `QDBusObjectPath: No such file or directory`，现在编过了。

##### 新的红：594/608，两处"Windows 才有的函数"

```
screenrecordingcontroller.cpp:1056: error: 'captureWindowId' is not a member of 'snow_shot::platform'
screenrecordingcontroller.cpp:1820: error: 'setWindowExcludedFromCapture' is not a member of 'snow_shot::platform'
```

这两个函数声明在 `snow_shot/include/snow_shot/platform/windowcaptureexclusion.h`，
实现全在 `src/platform/windows/windowchrome.cpp` 的 `#if defined(Q_OS_WIN)` 里。
那个 TU 自己第 39–44 行是：

```cpp
#if defined(Q_OS_WIN) || defined(_WIN32) || defined(Q_OS_MACOS)
#include "snow_shot/platform/windowcaptureexclusion.h"
...
#endif
```

也就是说**头在 Linux 上根本没被包含**，而调用点写在守卫外面 —— Windows 上能编译
是因为守卫生效，Linux 上连符号都不存在。和 9.6.19、9.6.24 是同一个结构性盲区：
**能编译它的平台不消费它**。

##### 这里是"没有这个能力"，不是"失败"

| 平台 | 机制 |
|---|---|
| Windows | `SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)` |
| macOS | `SCContentFilter` |
| X11 / Wayland | 无等价物 |

所以 Linux 上正确的行为不是"报错并中止录制"，而是**照常录制、toolbar 出现在画面里**。
`guardToolbarSurface()` 原来的失败分支会 `reportToolbarExclusionError()` 然后
`return false`，在 Linux 上那个分支永远不该被走到 —— 因为没有东西"失败"。
移植文档 §11 第 3 条已经写明这是要"在 UI 明确降级说明"的能力，而不是要伪装成错误。

```cpp
#if defined(Q_OS_WIN) || defined(_WIN32) || defined(Q_OS_MACOS)
        if (!surface || !snow_shot::platform::setWindowExcludedFromCapture(surface, true)) {
            reportToolbarExclusionError();
            return false;
        }
        static_cast<void>(captureExclusion.exclude(surface));
#else
        Q_UNUSED(surface);
#endif
```

`excludedWindowIds` 那一处（1056 行）同样包进守卫：Linux 上 `captureExclusion`
是用空 setter 构造的，`exclude()` 从来不会登记任何窗口，这个列表按构造就是空的。

##### 编译器还没走到的那一处

`tests/screen_recording_controller_tests.cpp` 的 `prepareExpectedRecordingExclusions()`
有同一个调用。它不在 Build 步骤里编（`Release` preset 是
`SNOW_APPS_BUILD_TESTS=OFF`），要等到最后的 `Unit tests` 步骤才会红 —— 也就是
"下一个红灯"，只是晚了半小时。

##### 门禁：`scripts/check-linux-windows-only-platform-symbols.py`

判据：**只在 Windows 守卫内定义的 `snow_shot::platform` 符号，任何引用都必须
落在 Windows/macOS 守卫的生效分支里**。三项必要的减法，每一项都对应一类误报：

| 减法 | 不加会怎样 |
|---|---|
| 减去 `src/platform/linux/*.cpp` 与 `src/platform/*.cpp` 里也有定义的符号 | `focusedFullscreenWindowExists` / `focusedFullscreenWindowOnScreen` 三平台各有一份，直接误报三处 |
| 减去 CMake `if(WIN32)` 块里声明的 target 的源文件 | `recapture_cursor_native_e2e_test.cpp` 只在 Windows 构建，它调用那些函数是对的 |
| 认出 `#else` 一侧 | `#if defined(Q_OS_WIN) … #else …` 的 else 分支是"非 Windows"，把它算成守卫会让门禁对一半代码失明 |

`if(NOT WIN32)` 也算"非守卫"，否则 `NOT` 会被读成肯定。

实测：`2 symbol(s)`、`12 guarded use(s)` —— 两个真符号（`captureWindowId`、
`setWindowExcludedFromCapture`），十二处已经被正确守卫的引用。

##### 反向验证

`revval-windows-only-symbols.sh`：拆掉录制控制器里的两处守卫、拆掉测试 helper 里的
守卫、加一行注释做对照。三红一绿，且每条红只指向被改的那一行。

##### 顺带

`check-linux-ci-workflow.py` 在这一轮暴露出自己的一个解析坑：`cmake --preset` 后面
接续行反斜杠时，剥掉 `${{ }}` 之后正则把 `\` 读成 preset 名（9.6.25 的 AppImage
步骤正好是多行 configure）。已把 preset 名的首字符类改成 `[^-\\\s]`。

---

#### 9.6.27 一轮 CI 被文档提交杀掉：`paths-ignore` 漏了渲染出来的 HTML

##### 现象

run 25（`39814151585`，`678f4efe`）刚跑到 `gates` 的第 9 步就变成 `cancelled`。
紧接着 run 26（`39814429526`）出现在队列里，而它的 commit 是 `2fd80f5d` ——
一个只改了 `docs-uos-linux-port.md` 与 `docs-uos-linux-port.html` 的文档提交。

##### 定因

workflow 的过滤条件是：

```yaml
paths-ignore:
  - "docs*/**"
  - "*.md"
  - ".workbuddy/**"
```

`docs-uos-linux-port.html` 在**仓库根目录**，既不是 `docs*/**`（那是目录），
也不匹配 `*.md`。于是文档提交满足"有受影响的路径"，起了一次完整 build；
`concurrency` 组的 `cancel-in-progress: true` 随即把正在跑的那一轮掐掉。

这本身不是第一次：移植文档从一开始就有 `.md` 与 `.html` 两份，`*.html` 只是
一直没被想起来。**能看见的代价是半小时的构建**，看不见的代价是"到底哪一轮才
是有效结果"这件事变得可疑。

##### 修法

`push` 与 `pull_request` 都补 `"*.html"`，并把这个清单写成门禁的一条判据：

```python
IGNORED_DOCUMENTATION_PATTERNS = ("*.md", "*.html")
```

反向验证第 7 条：把 `*.html` 从两个 trigger 里删掉，门禁必须两条都红
（`push:` 与 `pull_request:` 各一条），且只红这两条。

**教训**：`paths-ignore` 是"哪些改动不值得跑"，它的正确性不来自直觉而来自
仓库里实际存在哪些产物 —— 这里恰好是一个"文档双份渲染 + 分支保护 + 并发取消"
三者叠加才会显形的问题。

---

#### 9.6.28 run 27：608/608 编译全过，红落到链接 —— 一个从没人核对过的文件名（2026-10-22）

##### 现象

run 27（`39816795355`，`2fd80f5d`）的 arm64 job 走完了全部编译单元：

```
[595/608] Building CXX object .../screenrecordingcontroller.cpp.o
...
[607/608] Linking CXX executable snow_shot/snow_shot
```

然后死在链接：

```
/usr/bin/ld: cannot find cargo/aarch64-unknown-linux-gnu/release/libsnow_linux_platform.a:
No such file or directory
collect2: error: ld returned 1 exit status
```

这是本轮第一次红在链接而不是编译 —— C++ 侧的编译问题已经清零。

##### 先排除"相对路径"这个假嫌疑

链接行里 `cargo/aarch64-unknown-linux-gnu/release/libsnow_linux_platform.a`
看着像相对路径，但同一条命令行里还有
`cargo/full/aarch64-unknown-linux-gnu/release/libsnow_shot_rust_ffi.a`，那个是
存在的。**ninja 打印的路径本来就相对于 build 根目录**，而链接那一步的 cwd 正是
`build/snow-shot-linux-arm64-release`，所以两者都解析成绝对路径。路径没错。

##### 定因：`OUTPUT_NAME` 是对文件的承诺，而 cargo 用的是另一个名字

`snow_add_rust_static_library` 不编译任何东西，它只是登记一个 IMPORTED STATIC
目标：

```cmake
set_target_properties("${_target_name}" PROPERTIES
    IMPORTED_LOCATION "${SNOW_RUST_CARGO_TARGET_DIR}/${SNOW_RUST_TARGET}/${_profile}/lib${_output_stem}.a")
add_dependencies("${_target_name}" "${batch_name}_build")   # 这条才跑 cargo build
```

`batch_name_build` 里只有两件事：`cargo build` 与（可选）`objcopy
--remove-section=.drectve`。**没有任何复制或改名**。而 cargo 的产物名由 crate 的
库名决定 —— `[lib] name`，缺省是包名把连字符换成下划线：

```toml
# snow-crates/crates/snow-linux-c/Cargo.toml
[package]
name = "snow-linux-c"
[lib]
crate-type = ["staticlib", "rlib"]     # 没有 name
```

于是 cargo 写的是 `libsnow_linux_c.a`，而登记处写的是：

```cmake
snow_add_rust_static_library(snow_linux_platform PACKAGE snow-linux-c
    MANIFEST_DIR "${SNOW_SHOT_CAPTURE_CRATES_DIR}" OUTPUT_NAME snow_linux_platform)
```

`OUTPUT_NAME` 被当成了标签，实际它被拼进 `IMPORTED_LOCATION`。两个 `-c`/`_c`
之差，`ld` 找不到文件。日志也印证 cargo 确实编译过：`Compiling snow-linux` /
`Compiling snow-linux-c` 都在，只是落成了另一个名字。

其余 13 处登记都是对的，因为它们的 `OUTPUT_NAME` 恰好等于包名下划线化
（`snow-capture-c` → `snow_capture_c` …）。**只有这一个 crate 的名字和 CMake
目标名不一致**，而它偏偏只存在于 `if(UNIX AND NOT APPLE)` 里，且是 P3 才加的
目标 —— 直到 run 27 才有第一个 job 走到链接这一步。

##### 修法

```cmake
snow_add_rust_static_library(snow_linux_platform PACKAGE snow-linux-c
    MANIFEST_DIR "${SNOW_SHOT_CAPTURE_CRATES_DIR}" OUTPUT_NAME snow_linux_c)
```

CMake 目标名保持 `snow_linux_platform`（消费方与 INTERFACE 包装 `snow_shot_linux_platform`
都不动），只把"文件叫什么"改成 cargo 实际写出的名字。

##### 新门禁 `scripts/check-rust-archive-names.py`

判据：每一处 `snow_add_rust_static_library` / `snow_add_rust_static_libraries`
登记的 `OUTPUT_NAME`，必须等于该包 `Cargo.toml` 推出的库名（`[lib] name`，缺省
包名，连字符换下划线）。

三项实现决定：

1. **读 `[lib] name`，不只看包名** —— 门禁跟着 cargo 的规则走，将来谁真去改
   `[lib] name`，门禁也跟得上（反向验证第 5 条就是这个方向）。
2. **包找不到 Cargo.toml = 失败**，不是跳过。这一类的盲区正是"跳过"造成的。
3. **变量实参的转发调用跳过** —— `snow_add_rust_static_library` 自己体内那一处
   `PACKAGE "${SNOW_RUST_PACKAGE}"` 是包装器，它的每个调用点各自被检查。

顺带检查批量登记的两条一致性：`OUTPUT_NAMES` 条数与 `PACKAGES` 相等。

实测：`Rust archive name check passed (14 registration(s), 44 crate(s) indexed, 1 forwarding argument(s) skipped)`。

##### 反向验证（4 变异 + 2 对照，全过）

| # | 变异 | 期望 |
|---|---|---|
| 1 | 把 `OUTPUT_NAME snow_linux_c` 改回 `snow_linux_platform` | 只红这一条，且点名 `libsnow_linux_platform.a` |
| 2 | 批量登记里 `snow_ui_selector_c` → `snow_ui_selector` | 只红这一条 |
| 3 | `PACKAGE snow-linux-c` → `snow-linux-cc` | 只红"没有 Cargo.toml" |
| 4 | 批量 `OUTPUT_NAMES` 删掉一条 | 只红"2 entries for 3 packages" |
| 5 | 对照：给 `[lib]` 写上 `name = "snow_linux_c"` | 必须仍绿（证明门禁读的是 `[lib] name`） |
| 6 | 对照：只加行尾注释 | 必须仍绿 |

**教训**：`OUTPUT_NAME` 被读成了"标签"，但它被拼进 `IMPORTED_LOCATION`，是一个
**对文件的承诺**。这个承诺没有任何环节校验：configure 时文件还没生成；编译期
根本用不到它；只有链接会查，而链接只在那条"本机没人跑"的平台上发生。这和
9.6.24 的 `PRIVATE` 是同一类错误 —— **声明与事实之间没有回路**，只能靠门禁
补上。

---

#### 9.6.29 run 29：档案名修好之后，链接一次报出两件"从来没人检查过"的事（2026-10-22）

##### 修法成立

`cannot find ... libsnow_linux_platform.a` 消失了 —— `ld` 真的开始读那两个 Rust
档案。然后它一口气说了两件不相干的事。

##### 一、两份 Rust 运行时：`multiple definition of rust_eh_personality`

```
multiple definition of `rust_eh_personality'
multiple definition of `std::panicking::EMPTY_PANIC'
multiple definition of `std::sys::args::unix::imp::ARGV_INIT_ARRAY'
    (/usr/bin/ld: ... libsnow_shot_rust_ffi.a(...) ... 第一次定义在
     libsnow_linux_c.a(std-55999f89fc36b51f.std.e04eed4b29a65078-cgu.0.rcgu.o))
```

`crate-type = ["staticlib"]` 会把**所有**依赖（包括 `std`）打进归档，所以
"两个 Rust 静态库进同一个镜像"就等于"两份运行时"。三平台里只有 Linux 会撞上：

| 平台 | 一个镜像里的 Rust 归档 | 结果 |
|---|---|---|
| Windows | 1（`snow_shot_rust_ffi`） | MSVC 折叠重复 COMDAT |
| macOS | 1（同上） | ld64 根本看不到这对 |
| Linux | 2（+ `libsnow_linux_c.a`） | GNU ld 直接报重复定义 |

而且两份 `std` 是**同一次构建的同一个目标文件**（`std-55999f89fc36b51f` 的
对象名一模一样）—— 内容相同也照样报错，`ld` 没有"内容一致就放过"这回事。

修法：`-Wl,--allow-multiple-definition`，但**挂在哪**是重点。

```cmake
target_link_options(snow_shot_linux_platform INTERFACE
    "-Wl,--allow-multiple-definition")
```

挂在 `snow_shot_linux_platform` 这个 INTERFACE 目标上，只有"同时拉第二个 Rust
归档"的消费者拿到它；写成目录级 `add_link_options()` 就会顺带把 C++ 侧真正的
重复定义也一起放过 —— 600 个文件的工程里这正是最不该有的静默降级。

结构上的正解是"每个镜像只有一个 Rust 链接单元"，两个 crate 现在分属两个
Cargo workspace（`snow-crates` 与 `snow_rust_ffi`），合并是另一件事，记在这里
而不是混进这次修复。

##### 二、OpenCV 的 NEON 实现从来没被生成过

```
undefined reference to `cv::dnn::opt_NEON::convBlock_F32(int, float const*, ...)'
undefined reference to `cv::dnn::opt_NEON::convBlockMR1_F32(int, ...)'
```

定因链是四步，每一步都有上游源码可指：

1. `scripts/build-linux-deps.sh` 里传了
   `-DOPENCV_SKIP_SYSTEM_PROCESSOR_DETECTION=TRUE`（这个值是从 vcpkg 的
   portfile 抄来的，本仓库没写过理由）。
2. 上游 `cmake/OpenCVDetectCXXCompiler.cmake` 里它长这样：

   ```cmake
   if(OPENCV_SKIP_SYSTEM_PROCESSOR_DETECTION)
     # custom setup: required variables are passed through cache / CMake's command-line
   elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "amd64.*|x86_64.*|AMD64.*")
     set(X86_64 1)
   ...
   elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64.*|AARCH64.*|arm64.*|ARM64.*)")
     set(AARCH64 1)
   ```

   也就是"**你得自己把 `X86_64` / `AARCH64` 传进来**"，那是给交叉编译准备的；
   这里一个都没传。
3. 于是 `cmake/OpenCVCompilerOptimizations.cmake` 里 `if(X86 OR X86_64)` 与
   `elseif(ARM OR AARCH64)` 两支都不进：`CPU_DISPATCH` 保持空，`CPU_BASELINE`
   落到兜底的 `DETECT`。aarch64 上 DETECT 必然选中 NEON（armv8 基线就带），
   于是 `CV_TRY_NEON` 为真，dnn 的卷积代码去调 `opt_NEON::convBlock_F32`。
4. 而 `opt_NEON` 命名空间只存在于**分发翻译单元**（`convolution.neon.cpp`）里，
   它由 `CPU_DISPATCH` 决定生成 —— 空的分发列表意味着一个都没有。

修法：把这个选项显式写成 `FALSE`（写出来而不是删掉，避免又漂回 port 的值）。
打开探测后，aarch64 拿到上游默认 `CPU_BASELINE=DETECT` +
`CPU_DISPATCH=NEON_FP16;NEON_BF16;NEON_DOTPROD`，x64 拿到 `CPU_BASELINE=SSE3`
加完整分发列表 —— 顺带解决了一个之前没人问的问题：原来的配置把 `DETECT`
当基线，等于让发布出去的归档按 **runner 的 CPU** 调优，换台老机器就可能
非法指令。

**为什么十几个 run 都没发现**：静态库不报未解析符号。`ar` 装完就算成功，
`libopencv_dnn.a` 里那个 undefined 一直躺在那里，只有最终可执行文件链接时
才会被追出来 —— 而最终链接直到 run 27 才第一次发生。

##### 新门禁

`scripts/check-linux-rust-runtime-duplication.py` 钉住三件事：标志存在、
挂在 `snow_shot_linux_platform` 的 INTERFACE 上、**全树只出现一次**（注释与
`.workbuddy/` 里的反向验证脚本不算）。外加一条："注释里点名的两个归档对应的
crate 还在" —— 免得门禁守着一个已经消失的前提。

`check-linux-deps-opencv-options.py` 增加第 5 组判据：该选项必须是 `FALSE`，
且必须写出来（缺行也算失败）。

##### 反向验证

| 门禁 | 变异/对照 | 数 |
|---|---|---|
| rust runtime duplication | 删标志、写两遍、写到别的 cmake、移出 `if(UNIX AND NOT APPLE)`、行尾注释对照 | 4 + 1 |
| opencv options | `=TRUE`、删该行（+ 原有 5 条） | 7 |

##### 教训

两次都是"**同一件事在两个层次上同名，但只有一个层次会被检查**"：`OUTPUT_NAME`
是文件名承诺却没人核对；`CPU_DISPATCH` 决定归档内容却没人核对；静态库不报
未解析符号，于是"deps 绿"只证明 `ar` 跑完了，不证明产物可用。

---

#### 9.6.30 `Fetch appimagetool` 一个参数都没传：门禁断言了"写下来了"，没断言"能成立"

上一节的两个修复推上去后，`Fetch appimagetool` 这一步是 AppImage 路径第一次
真正执行。它在第一行就死了：

```
error: --arch is required
```

原因不是脚本写错，而是 workflow 调用它时**没给任何参数**：

```yaml
- name: Fetch appimagetool
  run: bash scripts/fetch-appimagetool.sh >> "$GITHUB_ENV"
```

`--arch` 与 `--dest` 在脚本里都是必填，两者都没有默认值，也都是不能猜的：

* `--arch` 决定取哪两份资产（appimagetool 与 type-2 runtime 各自按 ELF machine
  分档），默认成 host 架构在交叉产物的语境里就是错的；
* `--dest` 决定落点，而脚本 stdout 输出的两个绝对路径会被直接交给
  `-DSNOW_APPS_APPIMAGETOOL=` / `-DSNOW_APPS_APPIMAGETOOL_RUNTIME=`，
  由 `snow_shot_add_appimage_target()` 再拼进 `--runtime-file`。

##### 为什么门禁没抓到

`scripts/check-linux-ci-workflow.py` 当时对这一步的断言只有两条：

```python
check("Fetch appimagetool writes the pinned paths into the job environment",
      "fetch-appimagetool.sh" in fetch and "GITHUB_ENV" in fetch, ...)
```

也就是说它检查的是"**这一步写了脚本名、写了 `$GITHUB_ENV`**"，而唯一会失败
的那件事 —— 调用有没有给参数 —— 不在断言里。这与 9.6.28（`OUTPUT_NAME` 被当
标签、没人核对 cargo 真会写出那个文件）是同一类错误：断言落在"声明存在"上，
而不是"声明能成立"上。

##### 顺带暴露的第二件事：两个词汇表

workflow 用 `x64` / `arm64` 命名产物（artifact 名、preset 名），appimagetool
与 runtime release 用 `x86_64` / `aarch64`。原来只有 `Validate the AppImage`
做了映射，而且是内联三元：

```yaml
--expect-arch ${{ matrix.architecture == 'x64' && 'x86_64' || 'aarch64' }}
```

`Fetch` 步骤没有这行，于是同一个映射"在一处存在、在另一处不存在"，而缺失不会
被任何静态检查发现。改法是把映射搬进 matrix：

```yaml
- architecture: x64
  appimage-arch: x86_64
```

`Fetch` 与 `Validate` 都读 `matrix.appimage-arch`，两个词汇表之间只剩一个来源。

##### 门禁新增四条判据

| 判据 | 为什么 |
|---|---|
| `Fetch` 的 `run` 里出现 `--arch` | 脚本拒绝猜架构 |
| `Fetch` 的 `run` 里出现 `--dest` | 脚本 stdout 输出的是绝对路径，落点不能由它编 |
| matrix 的 `appimage-arch` 逐架构等于 ELF machine 名 | 映射写错（比如写成 `arm64`）与映射缺失一样只在 runner 上显形 |
| `Fetch` 与 `Validate` 都引用 `matrix.appimage-arch` | 一个词汇表一个来源，内联三元回归即红 |

##### 反向验证

4 变异 + 1 对照全过：删 `--arch`、删 `--dest`、把 matrix 的值改回 workflow 的
拼写（`arm64`）、把 `Validate` 改回硬编码 `x86_64`。

变异**改文本不改行数**：删整行会让 YAML 的结构（续行反斜杠）失效，门禁会在
"workflow parses as YAML" 就红，后面的断言根本没跑 —— 那样的"变红"不证明任何事。

##### 教训

"这一步写对了"和"这一步跑起来会成功"是两件事。前者可以用 `in` 断言，后者需要
断言**参数、取值、来源**三者同时成立。凡是"调用一个自己写的脚本"，脚本的必填项
就必须出现在门禁里 —— 因为必填项存在的理由，正是作者当时认为它不能猜。

---

#### 9.6.31 AppImage 暂存的两个源文件指向 `<repo>/packaging`：函数体里的 `CMAKE_CURRENT_SOURCE_DIR` 是"谁在调用"

这一轮不是 CI 报的，是等 run 32 的时候把 AppImage 那段从未执行过的 configure
面读了一遍发现的。它会在 `Build the AppImage` 的 configure 阶段直接
`FATAL_ERROR`，所以无论前面几关多绿，AppImage 都出不来。

##### 机制

`snow_shot_add_appimage_target()` 里这样取两个要拷进 AppDir 的文件：

```cmake
set(_snow_apprun "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/appimage/AppRun")
set(_snow_icon_source "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/hicolor/256x256/apps/snow-shot.png")
```

而它唯一的调用点在**根** `CMakeLists.txt`：

```cmake
if(SNOW_APPS_PACKAGE_APPIMAGE)
    include(cmake/SnowShotAppImage.cmake)
    snow_shot_add_appimage_target()   # ← 这里
endif()
```

`CMAKE_CURRENT_SOURCE_DIR` 是"当前正在处理的 `CMakeLists.txt` 所在目录"，不是
"这个 `.cmake` 文件所在目录"，也不是"函数定义所在的目录"。于是两处都解析成
`<repo>/packaging/...` 与 `<repo>/resources/...`：

| 路径 | 实际位置 | 解析结果 |
|---|---|---|
| `packaging/linux/appimage/AppRun` | `snow_shot/packaging/...` | 不存在 |
| `resources/icons/hicolor/256x256/apps/snow-shot.png` | `snow_shot/resources/...` | 不存在 |

图标那条有 `EXISTS` 检查，所以 configure 会在这里停：

```
Missing <repo>/resources/icons/hicolor/256x256/apps/snow-shot.png.
Run: python3 scripts/gen-linux-icons.py
```

**报错信息本身是误导的**：图标一直在 `snow_shot/resources/` 下，`gen-linux-icons.py`
再跑一遍也不会有任何变化。一个错误的路径配一句"去生成图标"的建议，比不报错更费时间。

##### 为什么门禁没抓到

`scripts/check-linux-packaging.cmake` 第 5 节确实测了这个模块，但测的是三个**纯函数**
（架构映射、布局字符串）。会碰目标、会碰真实文件的是
`snow_shot_add_appimage_target()`，而它需要 `TARGET snow_shot`，在工作站上永远不执行 ——
门禁对它的断言只有一条：`CMakeLists.txt` 里出现过 `snow_shot_add_appimage_target()`。
**"调用点存在"与"调用点能成立"之间的那段，就是这次的 bug 所在。**

这与 9.6.28 / 9.6.30 同一族：断言落在"写下来了"，而失败的机制在"写下来的东西
指向哪里"。

##### 修法

布局函数改成接受 `APP_SOURCE_DIR`，省略时向目标要：

```cmake
if(_snow_APP_SOURCE_DIR)
    set(_snow_app_dir "${_snow_APP_SOURCE_DIR}")
elseif(TARGET snow_shot)
    get_target_property(_snow_app_dir snow_shot SOURCE_DIR)
endif()
```

并新增两个输出 `SNOW_SHOT_APPIMAGE_APPRUN_SOURCE` / `SNOW_SHOT_APPIMAGE_ICON_SOURCE`，
`AppRun` 也补上 `EXISTS` 检查（原来只有图标有，缺 `AppRun` 会一路走到
`cmake -E copy` 才炸，报的是 copy 失败）。

`get_target_property(... SOURCE_DIR)` 而不是写死 `snow_shot/`：目标自己知道自己在
哪个目录，调用方不必知道。**凡是"这文件在哪"，问持有它的那个东西，不要问"我在哪"** ——
`CMAKE_CURRENT_LIST_DIR`（9.6.x 早先那条）与 `CMAKE_CURRENT_SOURCE_DIR` 是同一个坑的
两个变量名。

##### 门禁

`check-linux-packaging.cmake` 用 `APP_SOURCE_DIR "${_snow_repo}/snow_shot"` 显式调用
布局函数，然后**逐个项目检查那两个文件真的存在**。这条检查是功能性的：它不等价于
"文本里有几个变量名"，而是"算出来的路径落在有文件的地方"。

##### 反向验证

3 条 + 1 对照全过：把锚点换回 `CMAKE_CURRENT_SOURCE_DIR`（两条路径同时红）、只把
图标名写错（只有 `ICON_SOURCE` 红，证明两条检查互相独立）、未改动时绿。

脚本另外加了一条**基线守卫**：快照之前先跑一遍门禁，不绿就拒绝快照。这一条是踩
出来的 —— 上一次脚本在"变异后、还原前"被打断，把带 `.png.bak` 的文本留在树里，
下一次运行就把这份污染当成了基线，于是所有用例"通过"，还原时忠实地把坏文本恢复
回去。

##### 同一轮读代码时确认的另一件事：Linux 安装树里没有 Qt

`snow_shot/CMakeLists.txt` 里唯一的 Qt 部署块是：

```cmake
if(WIN32 AND NOT SNOW_SHOT_QT_STATIC)
    qt_generate_deploy_app_script(...)
    install(SCRIPT "${SNOW_SHOT_QT_DEPLOY_SCRIPT}")
```

即：**Linux 上不部署 Qt**。这解释了 .deb 为什么必须靠
`CPACK_DEBIAN_PACKAGE_SHLIBDEPS=ON`（依赖发行版的 Qt 包），但也意味着
`cmake --install` 出来的 AppDir 里没有 `usr/lib/libQt6*.so*`、也没有
`usr/plugins/platforms/libqxcb.so` —— 而 `packaging/linux/appimage/AppRun` 的第一段
注释写的正是"没有 `QT_PLUGIN_PATH` 的包会在第一个窗口上报
`no Qt platform plugin could be initialized`"。

也就是说：AppRun 假定 Qt 是被打包进去的，安装规则并没有这么做。这份 AppImage
只在与构建机同源的 Qt 上能跑，跨发行版（UOS 20 的 Qt 版本不同）不行。

这是 P7 的独立一项，不在本节修：它会改变安装树的内容，需要自己的一轮验证。
在此之前 `Validate the AppImage` 只验结构（ELF、magic、AppRun 字节一致），
**不声称"能启动"** —— 这一点必须写在文档里，而不是留在"应该没问题"里。

##### 与附录 A.2 / A.3 的关系（不要把这条读成"该在 24.04 上打包 Qt"）

附录 A.3 的结论没变：**AppImage 打不进 glibc**，能否在 UOS 20 上跑取决于构建主机的
glibc 基线，而不是取决于 Qt 打没打进去。所以在当前的 `ubuntu-24.04` runner 上把 Qt
塞进 AppDir，只是让一个"必定因 `GLIBC_2.34 not found` 失败"的包看起来更完整。

这条要修的地方在**未来的 2.28 构建根**：那里 Qt 必须来自身边（系统里的 Qt 是老的、
且与自建版本不一致），而安装规则现在是 `if(WIN32 …)`，一份都不会进 AppDir。
两件事分开记：*打包路径已通*（本节与 9.6.30、9.6.32）与 *产物可在目标机运行*
（取决于 2.28 构建根 + Qt 进安装树），后者仍未开始。

---

#### 9.6.32 同一个错锚点还在产物校验器里：`is_file()` 返回 False 时，检查整段被跳过

改完 9.6.31 顺手看 `Validate the AppImage` 调的那个脚本，同一个仓库根锚点又出现一次：

```python
reviewed = REPO / "packaging" / "linux" / "appimage" / "AppRun"
```

仓库根从来没有 `packaging/`。差别在于后果更隐蔽：

```python
check("the reviewed AppRun exists in the repository", reviewed.is_file(), str(reviewed))
if app_run.is_file() and reviewed.is_file():
    check("the packaged AppRun is the reviewed one", ...)
```

`is_file()` 为 False ⇒ 第二条 `check` **整段不执行**。第一条会报 FAIL，但它是
"文件不存在"，读起来像环境问题；真正想说的"包里那份和审过的那份不一致"从未被问过。
而在 CI 上，第一条之前的 `usr/bin/snow_shot` 之类若先红，这一条连 FAIL 都不会被看到。

文件里原来的注释还把这个猜测写成了事实：

> the file lives at the repository root (`packaging/linux/appimage/AppRun`, the path
> `cmake/SnowShotAppImage.cmake` copies from

`cmake/SnowShotAppImage.cmake` 从来没有从仓库根拷过东西 —— 9.6.31 之前它用的是
`${CMAKE_CURRENT_SOURCE_DIR}`，那也不是仓库根。**注释和代码各自猜了一件事，
两件事都错，而且它们互相印证。**

##### 修法，以及一条通用的门禁

锚点改成 `REPO / "snow_shot" / "packaging" / ...`。但只修这一处不够：这个脚本
在 CI 的 runner 上、手里有真 AppImage 时才运行，工作站上没人解析它。

所以 `scripts/check-linux-appimage.py` 加了一条通用检查：用 `ast` 找出
`check-linux-appimage-build.py` 里所有 `REPO / "a" / "b" / ...` 字面量链，
逐条解析到工作树里，必须存在。

用 `ast` 而不是正则是有原因的，第一版就踩了：正则把**注释里**的
`REPO / "..."` 也当成表达式，而 `Path("...")` 在 Windows 上因为尾部点被规范化，
解析成了仓库目录本身，于是"存在"。用 `ast` 只看真实的表达式树。

##### 反向验证

2 变异 + 1 对照：锚点退回仓库根（红在该链上）、链指向不存在的目录（红）、未改动时绿。
第 2 条是"看起来完全正确、只是指错了地方"的形态 —— 任何检查"表达式写对没有"的门禁
都会放过它。

##### 教训

`is_file()` 当守卫是必要的，但**守卫失败时不能静默跳过断言**：要么让它先红并把
"因为前面没找到"写清楚，要么把它变成断言本身的一部分。这次两者都缺 ——
于是"暂存路径错"和"内容不一致"两个不同的问题，在日志里长得一样。

---

#### 9.6.33 run 34：Build 两架构全绿，红落到 `dpkg-shlibdeps` —— §9.6.9 那条"等 build 绿了再定"到期了

run 34（`39843647757`）是个分界：`Build` 步骤在 **x64 与 arm64 上都是 success**。
上一节的链接两处修复成立 —— `-Wl,--allow-multiple-definition` 只挂在
`snow_shot_linux_platform` 的 INTERFACE 上，OpenCV 的 CPU 探测从"抄来的 TRUE"改回
`FALSE`，608 个编译单元之后的那条 `ld` 命令第一次走完。

红落在下一步，仍是两个架构一起：

```
dpkg-shlibdeps: error: cannot find library libjxl.so.0.11 needed by ...
dpkg-shlibdeps: error: cannot find library libjxl_threads.so.0.11 needed by ...
dpkg-shlibdeps: error: cannot find library libOpenEXR-3_4.so.33 needed by ...
dpkg-shlibdeps: error: cannot find library libIex-3_4.so.33 needed by ...
dpkg-shlibdeps: error: cannot find library libImath-3_2.so.30 needed by ...
dpkg-shlibdeps: error: cannot find library libjpeg.so.62 needed by ...
dpkg-shlibdeps: error: cannot find library libturbojpeg.so.0 needed by ...
dpkg-shlibdeps: error: cannot find library liblunasvg.so.3 needed by ...
```

八个 soname，全部来自 `scripts/build-linux-deps.sh` 自建的固定版本（§9.6.9–9.6.13），
没有任何发行版包拥有它们。

##### 这不是新发现，是一条早写下的欠账

§9.6.9 里对 OpenCV 为什么必须静态，第三条理由就是这件事：

> ③ `dpkg-shlibdeps` 从 ELF NEEDED 推导 Depends，一个不属于任何发行版包的共享库会
> 让这一步**报错**而不是警告。

以及同一节末尾：

> 运行时链接谁，还没定。……要么静态链进包，要么随包带 `libz-ng` 并设 rpath —— 这是
> 打包决定，**等 `build` 绿了再定**。

`build` 现在绿了，欠账到期。**这一节的价值不在于新知识，而在于那条"等 X 绿了再定"
的记号真的被兑现了** —— 它没有变成"以后再说"，也没有变成一条没人认领的注释。

##### 两条被否掉的路

| 方案 | 为什么不行 |
|---|---|
| 手写 `Depends:` | 会静默过期：以后多链一个 Qt 模块，包照样装得上，一启动就 `error while loading shared libraries` |
| `SHLIBDEPS=OFF` | 打出一个**装得上、跑不起来**的包，比构建失败更糟 |

所以只剩随包带。三件事必须**同时**成立，缺一件都只在 CPack 真正跑起来时才发现：

1. **把闭包算出来并复制**。`cmake/SnowShotLinuxRuntimeBundle.cmake.in` 作为
   `CPACK_INSTALL_SCRIPTS` 在暂存树上运行，用 `file(GET_RUNTIME_DEPENDENCIES)`
   解析整棵暂存树，只复制"解析结果落在依赖前缀里"的那些。手写清单会在下一轮因为
   `libjxl` 自己需要 `libhwy` 而漏 —— 闭包不是一层。
2. **给安装后的 ELF 加 RPATH**。`snow_shot_apply_linux_private_rpath()`：
   可执行文件 `$ORIGIN/../lib/snow-shot`，库 `$ORIGIN/snow-shot`。
   `DT_RUNPATH` **不继承给间接依赖**，所以随包的库自己也得有 —— 否则
   `libjxl` 找到 `libhwy` 只能靠运气。
3. **`CPACK_DEBIAN_PACKAGE_SHLIBDEPS_PRIVATE_DIRS` 指向依赖前缀**。

##### RPATH 是这三者的关节

`CPackDeb.cmake` 的 `dpkg-shlibdeps` 调用是：

```cmake
execute_process(COMMAND "${SHLIBDEPS_EXECUTABLE}" ${PRIVATE_SEARCH_DIR_OPTIONS}
                        ${IGNORE_MISSING_INFO_FLAG} -O ${CPACK_DEB_BINARY_FILES} ...)
```

`PRIVATE_SEARCH_DIR_OPTIONS` 由两处生成：用户给的
`CPACK_DEBIAN_PACKAGE_SHLIBDEPS_PRIVATE_DIRS`，**以及每个二进制的 RUNPATH 中带
`$ORIGIN` 的条目**（会结合打包目录展开）。也就是说，随包的库要"算进包内"，唯一的
自动渠道就是 RPATH。第一次构建的红，本质是"二进制没有 RUNPATH ⇒ 一个 `-l` 都没有"。

`--ignore-missing-info` 确实会被传入（CPack 先探 `dpkg-shlibdeps --help` 是否支持），
所以"找到了库但没有任何包拥有它"是**不报错**的 —— 这正是随包带能成立的前提。

##### 顺带修掉的一处覆盖

`snow_shot_add_appimage_target()` 里原本自己设
`INSTALL_RPATH "$ORIGIN/../lib"`（比私有目录高一层），而它在打包模块之后执行，
**会把上面那条覆盖掉**。改成调用同一个函数：私有目录与 RPATH 只有一处定义。

##### 门禁（第 2b 节，5 条）

| 判据 | 为什么 |
|---|---|
| `CPACK_INSTALL_SCRIPTS` 非空 | 否则没有任何一步会把自建库放进包里 |
| 生成的脚本存在、无残留 `@…@` | `configure_file` 的占位符没替换时会静默生成一个坏脚本 |
| 剥掉注释后仍出现 `GET_RUNTIME_DEPENDENCIES` / `CPACK_TEMPORARY_DIRECTORY` / `RPATH_SET` | 模板的注释**解释了**这三个名字，直接子串匹配会在调用被删掉时保持绿 |
| `CPACK_DEBIAN_PACKAGE_SHLIBDEPS_PRIVATE_DIRS` 已赋值 | 少一个 `-l` 就可能下一轮才红 |
| AppImage 模块里不再出现 `INSTALL_RPATH` | 两个所有者 = 后执行的那个说了算 |

"剥注释再匹配"是这一节最实际的一条经验：**模板/脚本里解释自己用法的注释，会让
基于函数名的断言永远为真。**

##### 反向验证

4 变异 + 1 对照全过。其中一条变异第一版是错的：`replace(..., 1)` 命中的是模板注释里
那个函数名，真正的调用还在，门禁保持绿 ⇒ 断言"变成红"失败。**"变异没打中目标"必须
当成失败处理**，否则反向验证会退化成一次自我安慰。

##### 途中被自己的门禁抓到的第三次同族错误

`snow_shot_configure_linux_package()` 里 `configure_file("${CMAKE_CURRENT_LIST_DIR}/…")`
—— 函数体里 `CMAKE_CURRENT_LIST_DIR` 是**调用方**的目录（§9.6.10 同一个坑），于是模板
被解析成 `scripts/SnowShotLinuxRuntimeBundle.cmake.in`：

```
CMake Error: File .../scripts/SnowShotLinuxRuntimeBundle.cmake.in does not exist.
```

改用 `CMAKE_CURRENT_FUNCTION_LIST_DIR`。§9.6.31 的 `CMAKE_CURRENT_SOURCE_DIR` 是同一个
坑的第三个变量名 —— 三者可以合成一句：**函数体里任何 `CMAKE_CURRENT_*` 都在回答"谁在
调用我"。**

##### 还有一处：门禁自己的"不存在的构建目录"

同一轮 gates job 也红了（本地绿）：

```
CMake Error: Could not open file for write in copy operation
/nonexistent/snow-build/snow-shot-linux-runtime-bundle.cmake.tmp
```

`scripts/check-linux-packaging.cmake` 把 `CMAKE_BINARY_DIR` 设成字面量
`/nonexistent/snow-build`，因为所有被断言的路径都由它拼出来、值本身无所谓 —— 直到
打包模块开始往它里面 `configure_file`。Windows 把那条路径解析到了可写的地方，Linux
直接失败。改成 `${CMAKE_CURRENT_BINARY_DIR}/snow-packaging-build`，开头清空、结束删除。

**"这个值无所谓"是一个会被后续改动推翻的断言，而且推翻它的人不会去看那行注释。**

#### 9.6.34 run 37 未到 CPack 就已被静态读码拦下：`file(RPATH_SET)` 的签名，与"私有库按 soname 命名"

run 37（`39857689984`，`62faa028`）在 `Build and package arm64` 的
`Build the pinned Linux dependencies` 上跑着，按纪律"等它说话"的同时把 9.6.33 写下的
暂存脚本从头读了一遍 —— 两处都会在第 11 步 `Build the Debian package` 才炸，而那时前面的
依赖编译已经烧掉二十多分钟。

**其一：`file(RPATH_SET)` 不是位置参数。**

```cmake
file(RPATH_SET "${_staged}/${_private_lib_dir}/${_name}" "$ORIGIN")   # 错
```

实测（本机 CMake 4.2.3）：

```
file RPATH_SET given unrecognized argument "/…/libjxl.so.0.11"
```

正确形态是关键字参数：

```cmake
file(RPATH_SET FILE "<file>" NEW_RPATH "<rpath>")     # 对
```

值得记一笔的是 `RPATH_SET` **根本不在 `file()` 的文档列表里**（`cmake --help-command file`
搜 `RPATH` 零命中）—— 它是 2.6 时代遗留下来、4.2 仍在实现的子命令。所以"签名对不对"不是
对文档的断言，而是对解释器的断言，这与 9.6.28 / 9.6.30 / 9.6.31 / 9.6.32 那一族
（"写下来了"vs"能成立"）是同一种病，但检查手段还得再退一格：**文档里查不到的东西，
只能让解释器自己回答**。

于是门禁里那把"检查脚本里有没有 `RPATH_SET`"的文本针，改成把脚本里那次调用**原文抽出来**
真跑一遍：

```cmake
string(REGEX MATCH "file\\(RPATH_SET[^)]*\\)" _rpath_call "${_bundle_code}")
file(WRITE "${_rpath_probe_dir}/probe.cmake"
     "set(_library \"${_rpath_probe_dir}/notelf.bin\")\n${_rpath_call}\n")
```

只读它的**拒绝**，三种结果对应三种病因：

| 探针输出 | 结论 |
|---|---|
| `unrecognized argument` | 写下来的签名这版 CMake 不认 |
| `does not recognize sub-command` | 这版 CMake 已经没有 `RPATH_SET` |
| 其他（非 ELF 文件写不进去） | 调用**解析通过**，只是载荷不对 |

第一版探针是**把签名写死在门禁里**的：它对非 ELF 文件跑一次，绿灯。反向验证第 7 条
（把模板的调用改成 `… NEW_RPATH "$ORIGIN" TRAILING`）把它照出原形 —— **文本针全绿、
探针也绿**，因为它验的是"门禁自己会不会用这个语法"，不是"脚本会不会"。改成提取式后
同一变异立刻红在 `is not a signature this CMake parses`。**这就是"变异没打中目标"的
镜像形态：变异打中了目标，探针没测目标。**

**其二：私有库按 soname 命名，不是按 `REALPATH` 的文件名。**

9.6.33 的复制循环是：

```cmake
get_filename_component(_real "${_dependency}" REALPATH)
get_filename_component(_name "${_real}" NAME)      # ← 错在这一行
```

依赖前缀里 `libjxl.so.0.11` 是指向 `libjxl.so.0.11.0` 的软链，而
`GET_RUNTIME_DEPENDENCIES` 解析的是 NEEDED 里的 **soname**：`_dependency` 的 basename
是 `libjxl.so.0.11`，`REALPATH` 的 basename 是 `libjxl.so.0.11.0`。按后者复制，包里就只有
`libjxl.so.0.11.0`，而 `snow_shot` 的每一个 NEEDED 都写着 `libjxl.so.0.11` ——
**loader 找不到它，`dpkg-shlibdeps` 也不认它**（它按 soname 查 shlibs 与包内容）。
症状是包能装、`dpkg -c` 看着有库、一启动 `cannot open shared object file`，
比 `cannot find library` 难查得多，因为打包步骤全绿。

改成按 `_dependency` 取 NAME，`file(COPY)` 后用 `file(RENAME)` 把 `REALPATH` 那个版本名
改回 soname。门禁加一条来源断言（`string(FIND)` + 中括号参数，避开正则里的 `${`）：

```cmake
set(_soname_needle [=[get_filename_component(_soname "${_dependency}" NAME)]=])
```

反向验证从 5 变异 + 1 对照扩到 **7 变异 + 1 对照**，全部通过：

```
ok    control (committed modules): green
ok    no install script: red on "CPACK_INSTALL_SCRIPTS is empty"
ok    no dependency resolution: red on "never uses GET_RUNTIME_DEPENDENCIES"
ok    private libraries without RPATH: red on "never uses RPATH_SET"
ok    flat-only prefix probe: red on "dependency prefix probe finds"
ok    AppImage module setting its own RPATH: red on "sets INSTALL_RPATH itself"
ok    bundled copy named after the real file: red on "not named after the resolved soname"
ok    RPATH_SET with an argument the parser rejects: red on "is not a signature this CMake parses"
```

一句话概括这一节：**`$ORIGIN` 让 loader 找到库，soname 让 `dpkg-shlibdeps` 认识库，
两者都错在"文件叫什么名字"上，而不是"有没有写"。**

#### 9.6.35 AppDir 里一个 `.so` 都没有：`cmake --install` 不认识 `CPACK_INSTALL_SCRIPTS`

9.6.33 把私有库的复制写成了 **CPack install script**。而 AppImage 的暂存走的是
另一条路：

```cmake
add_custom_target(snow-shot-appimage
    COMMAND cmake --install <build> --prefix <AppDir>/usr    # ← install 规则
    COMMAND ... appimagetool ...)
```

`cmake --install` **只认 `install()` 规则**，`CPACK_INSTALL_SCRIPTS` 是 CPack 自己的
变量。于是 AppDir 里有可执行文件、桌面项、图标，**没有任何 `*.so*`**：
可执行文件的 `$ORIGIN/../lib/snow-shot` RUNPATH 指向一个空目录，
`scripts/check-linux-appimage-build.py` 的 *"the package bundles shared libraries"*
会在第 16 步失败 —— 而那时整个构建已经跑完二十多分钟。

修法不是把 AppImage 也改成走 CPack，而是让**同一个脚本有两个调用方**：

```cmake
# SnowShotLinuxRuntimeBundle.cmake.in
if(DEFINED STAGED_DIR AND NOT STAGED_DIR STREQUAL "")
    set(_staged "${STAGED_DIR}")                        # AppImage 直接给前缀
elseif(DEFINED CPACK_TEMPORARY_DIRECTORY AND NOT CPACK_TEMPORARY_DIRECTORY STREQUAL "")
    set(_staged "${CPACK_TEMPORARY_DIRECTORY}")          # CPack 给暂存树
```

```cmake
# SnowShotAppImage.cmake，紧跟在 --install 之后
COMMAND "${CMAKE_COMMAND}" "-DSTAGED_DIR=${SNOW_SHOT_APPIMAGE_USR}"
        -P "${SNOW_SHOT_LINUX_BUNDLE_SCRIPT}"
```

脚本路径必须**离开** `snow_shot_configure_linux_package()` 的作用域：调用它的是
`CMakeLists.txt`（不是它自己），未导出的变量在那里展开成空串，失败信息是
`cmake -P: no file given`，且发生在构建末尾。所以转发列表里加了
`SNOW_SHOT_LINUX_BUNDLE_SCRIPT`，门禁也改成**调用之后**再读这个变量 ——
读得到本身就是"导出成立"的证明。

**那条被否掉的替代方案值得记下来**：把复制注册成 `install(SCRIPT ...)`，让
`cmake --install` 与 CPack 自动都覆盖到。它更优雅，而且组件语义大概率也过得去
（`install()` 没写 `COMPONENT` 时用 `CMAKE_INSTALL_DEFAULT_COMPONENT_NAME`，仓库在
`snow_shot/CMakeLists.txt:67` 把它设成了 `SnowShot`，正好对上
`CPACK_INSTALL_CMAKE_PROJECTS` 里那个组件名）。否掉它的理由是另外两条：

* **不在同一轮里重写正在被首次验证的路径**。deb 那条链路（CPack → 暂存 →
  `dpkg-shlibdeps`）这一轮才第一次真正跑到，把它的触发方式从
  `CPACK_INSTALL_SCRIPTS` 换成 `install(SCRIPT)`，等于让"能不能打包"和"打包方式变了"
  同时生效，红了分不清是谁的问题；
* **AppImage 不该依赖 CPack 的组件语义**。它本来就不走 CPack，暂存是
  `cmake --install`；让它的正确性取决于一个 CPack 变量和一套组件默认值，是把两条
  独立的路绑在一起。

所以是"同一份逻辑、两个显式调用方"，而不是"一个隐式覆盖两者的注册"。
**正在被验证的路径，不要在同一轮里重写。**

门禁侧新增三条，全部用 `string(FIND)` 打在机制上而不是名字上：

| 断言 | 打的是 |
|---|---|
| AppImage 目标出现 `-DSTAGED_DIR=` | 库真的被复制进 AppDir |
| AppImage 目标出现 `SNOW_SHOT_LINUX_BUNDLE_SCRIPT` | 用的是那一个脚本 |
| 剥注释后仍有 `set(_staged "${STAGED_DIR}")` | 覆盖真的被采纳（只提不采纳会被抓住） |

反向验证扩到 **10 变异 + 1 对照**，新增的三条分别对应"AppDir 没有库""路径没导出"
"提了 STAGED_DIR 却仍用 CPack 目录"，全部红在预期文案上。

#### 9.6.36 `install(PROGRAMS "$<TARGET_FILE:…>")` 不写 RPATH，而这两个 helper 正好是这么装的

`INSTALL_RPATH` 是**目标**的属性，只有在 `install(TARGETS)` 里才会被 CMake 用来重写
装出去那份文件的 RUNPATH。仓库里两个 Rust helper 走的是另一条路：

```cmake
install(PROGRAMS "$<TARGET_FILE:snow_ocr_process>" DESTINATION ${CMAKE_INSTALL_BINDIR})
install(PROGRAMS "$<TARGET_FILE:snow_shot_mcp>"    DESTINATION ${CMAKE_INSTALL_BINDIR})
```

`install(PROGRAMS)` 是**按文件拷贝**，逐字节复制，不做 RUNPATH 调整。而 cargo 可执行
目标本身是 `add_executable(... IMPORTED GLOBAL)` —— 文件由 cargo 产出，
CMake 没有参与链接，也就没有"装的时候改一改"这一步。结果是
`usr/bin/snow-ocr-process` 里**根本没有 `DT_RUNPATH`**：

* 运行时 loader 只查系统目录，包里 `usr/lib/snow-shot/` 的私有库对它不可见；
* `dpkg-shlibdeps` 那边同样吃亏：`CPackDeb.cmake` 的 `-l<dir>` 是从 RUNPATH 里
  带 `$ORIGIN` 的条目推出来的，没有 RUNPATH 就一条都推不出来，只能靠
  `SHLIBDEPS_PRIVATE_DIRS` 兜住。

包能装、主程序能起，OCR 子进程一 spawn 就找不到库 —— 属于"结构校验全绿、产物不能用"
的那一类。

**修法是链接期写一次，不是安装期改。** 在 `snow_add_rust_executable()` 里补一个
Unix 分支：

```cmake
set(_snow_rust_dollar "$")
set(_snow_rust_rpath "${_snow_rust_dollar}ORIGIN/../${_snow_rust_libdir}/snow-shot")
list(INSERT _cargo_environment 0
    "RUSTFLAGS=$ENV{RUSTFLAGS} -C link-arg=-Wl,-rpath,${_snow_rust_rpath}")
```

为什么不走 `file(RPATH_SET)`：它改的是**已存在**的 RPATH 字符串，需要新值不超出
原字符串的位置；给一个"一条 RPATH 都没有"的二进制新增条目不是它擅长的场景
（9.6.34 用它对私有库设 `$ORIGIN` 是另一回事 —— 那些库本来就有 RPATH 字段，
且 `$ORIGIN` 只有 7 个字节）。**同一个工具在不同输入上的可靠性不一样，
"能用"要按输入分别验证。**

`$` 用 `set(_snow_rust_dollar "$")` 拼出来，是为了在源码里不出现 `$` 紧跟 `{`
的序列 —— 那会被读成变量展开。

门禁侧比对的是**两个文件的常量**，不是单看某个字符串：从
`snow_shot_linux_private_lib_dir()` 取到这个名字（`snow-shot`），再要求在
`cmake/RustStaticLibrary.cmake` 剥注释后的正文里出现 `-Wl,-rpath,` 与
`/<这个名字>`。改名只改一处就会红。反向验证第 11 条（删掉那个 `list(INSERT …)`）
红在 `never get -Wl,-rpath,`。

#### 9.6.37 run 38：Build 两架构全绿，`CPACK_INSTALL_SCRIPTS` 在**装之前**跑

run 38 的 `Build` 在 x64 与 arm64 都过了（608 个单元 + 链接），红落在第 11 步
`Build the Debian package`，一行就说完：

```
CPack: Install projects
CPack: - Install scripts: …/snow-shot-linux-runtime-bundle.cmake
CPack: - Install script:  …/snow-shot-linux-runtime-bundle.cmake
CMake Error at …/snow-shot-linux-runtime-bundle.cmake:87 (message):
  snow-shot bundle script: the staged tree has no ELF objects.
```

这句是 9.6.33 特意写下的 FATAL（"暂存树里没有 ELF 对象"）。它成立，原因是
**`CPACK_INSTALL_SCRIPTS` 在安装项目之前运行**：

```text
CPack 的顺序：预安装目标 → CPACK_INSTALL_SCRIPTS → 已安装目录 → CMake 安装项目
```

脚本被调起来时 `${CPACK_TEMPORARY_DIRECTORY}/usr/bin` 还是空的，于是两个 glob 都是空的。
换句话说，**这个钩子问的是"装之前"，而我们做的是"装之后"的事**。
（这也解释了为什么它从未在别人项目里被当成"后处理"用。）

**解法是回到 9.6.35 否掉的那个方案，但理由换了。** 那里否掉 `install(SCRIPT ...)`
是为了不重写正在被首次验证的 deb 路径；现在 run 38 用一条错误证明
`CPACK_INSTALL_SCRIPTS` 本身就是错的钩子，所以换成安装规则，并且**两个消费方同时受益**：

```cmake
if(NOT CMAKE_SCRIPT_MODE_FILE)
    install(SCRIPT "${_snow_bundle_script}" COMPONENT "${CPACK_COMPONENTS_ALL}")
endif()
```

`COMPONENT` 不是修饰。CPack 被 `CPACK_INSTALL_CMAKE_PROJECTS` 告知"只装 `SnowShot`
组件"，而没有 `COMPONENT` 的规则会落到 `CMAKE_INSTALL_DEFAULT_COMPONENT_NAME` ——
这个变量是在 `snow_shot/CMakeLists.txt:67` 设的，**目录作用域**，够不到顶层。
少了这个参数，规则对 .deb 被静默跳过、对 `cmake --install` 照常执行，
两条路会给出不同结果。门禁为此加了一条：`install(SCRIPT … COMPONENT` 必须出现。

脚本的暂存根从 `CPACK_TEMPORARY_DIRECTORY` 改成 `CMAKE_INSTALL_PREFIX` —— 后者是
安装规则唯一能保证存在的那个（CPack 与 `cmake --install --prefix` 都设它）。
`-DSTAGED_DIR=` 覆盖保留：AppImage 目标仍然显式跑一遍同一个脚本，两条路互为兜底。

反向验证扩到 **13 变异 + 1 对照**，新增三条都对着这次的机制：

| 变异 | 红在 |
|---|---|
| 删掉 `install(SCRIPT …)` | `never registers the bundle script as an install rule` |
| 改回 `CPACK_INSTALL_SCRIPTS`（run 38 原形） | `still uses CPACK_INSTALL_SCRIPTS` |
| 去掉 `COMPONENT` 参数 | `has no COMPONENT` |

**这一节的教训与 9.6.30/9.6.28 同源，但方向相反**：那几条是"写下来了但没验证能成立"，
这一条是"验证了内容但没验证时机"。**一个钩子的名字不是它的顺序说明书。**

#### 9.6.38 run 39：`CMAKE_INSTALL_PREFIX` 不是暂存树，是包内的前缀

`Build` 两架构照旧全绿，第 11 步换了个错误，而且比上一次更难看：

```text
CPack: - Install project: snow_apps []
CMake Error at .../snow-shot-linux-runtime-bundle.cmake:97 (file):
  file failed to open for reading (File name too long):

    /usr/bin/[;/usr/bin/aa-enabled;/usr/bin/aa-exec;/usr/bin/aa-features-abi;…（43 447 字符）
```

**两件事同时错。**

**第一件：钩子正确了，路径没正确。** 安装规则里的 `CMAKE_INSTALL_PREFIX` 是**包内**前缀
`/usr`（`CPACK_PACKAGING_INSTALL_PREFIX`），暂存树要通过 **DESTDIR 机制**到达。
`CPACK_SET_DESTDIR` 的文档把这个二分说得很清楚：

> 没有 `CPACK_SET_DESTDIR` 时，CPack 用 `CPACK_PACKAGING_INSTALL_PREFIX` 作为前缀；
> 设了它则用 `CMAKE_INSTALL_PREFIX`。

而 CPack 的本地安装**一律**走 DESTDIR：生成的每条安装规则都把 `$ENV{DESTDIR}` 拼进
destination。一个安装脚本要读暂存树，就得做同样的事：

```cmake
set(_staged "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}")
```

只取后半截的后果不是"路径差一层"，而是**去读构建机自己的 `/usr/bin`**——三千个条目。
真让那个循环跑完，包会把构建机的库打进去，那比失败糟糕得多。

因此 `DESTDIR` 为空、而前缀又等于包内前缀时，脚本**拒绝执行**，不退回前缀。
这是唯一一个"宁可红、不可猜"的取舍点：没有暂存树这件事本身就该被说出来。

**第二件：`file(GLOB)` 的结果会被一个 `[` 弄成单元素列表。** 报错里那 43 KB 是
"文件名"，说明 `foreach(IN LISTS …)` 只跑了一次、`_program` 是整串。本地复现，两行就够：

```cmake
# 目录里两个文件：`[` 和 `aa`
file(GLOB _p ".../bin2/*")
list(LENGTH _p _n)      # => 1
message("${_p}")        # => C:/…/bin2/[;C:/…/bin2/aa
```

`length=1` 而值里仍带 `;` —— 分隔符被转义了，`IN LISTS` 于是把它当一个元素。
`/usr/bin/[` 这个文件在每台 Debian 系机器上都存在（`[` 是 test 的内建），
所以这不是理论问题：**只要 glob 到宿主的 `/usr/bin`，就一定会撞上它**。
换成我们自己的暂存树后不会再有 `[`，但这个坑值得留个守卫：每个元素先查
`EXISTS`，塌缩时给出**一句话**而不是 43 KB。

**第三件（本地门禁的执行探针抓到的，未上 CI）：`cmake -P` 模式下 `CMAKE_INSTALL_BINDIR` 是空的。**
`GNUInstallDirs` 只在 `snow_shot/CMakeLists.txt` 里 include，它填的是**缓存**，所以真实
configure 顶层读得到；但门禁跑的是 `cmake -P`，那个 include 从没执行过，模块于是把空串
替换进模板，glob 变成 `${_staged}//*` —— 暂存树的根，里面全是目录，`LIST_DIRECTORIES false`
一过滤就是零个 ELF 对象。**这一条不是 run 39 的原因，是"修完第一件之后会立刻撞上的下一件"**，
被"真的把脚本跑一遍"抓住，而不是被文本断言抓住。修法是让模块自己 `include(GNUInstallDirs)`
（缓存使真实 configure 里它是空操作），并且只放**一处**——放在 `private_lib_dir` 和
`apply_linux_private_rpath` 里的副本是空操作，而**空操作无法反向验证**，所以不留。

门禁因此多了两条**执行**断言（不只是读文本）：

| 执行 | 期望 |
|---|---|
| 空 DESTDIR + 包内前缀 | `there is no staging tree` |
| 暂存树里有名为 `[` 的文件 | `a glob result is not a file`（一句话，无 ENAMETOOLONG） |

反向验证扩到 **14 变异 + 1 对照**，新增四条：

| 变异 | 红在 |
|---|---|
| 暂存根去掉 `$ENV{DESTDIR}`（run 39 原形） | `does not build its staging tree from DESTDIR` |
| 空 DESTDIR 退回包内前缀 | `there is no staging tree` |
| glob 绕过校验 helper | `was not reported readably` |
| 去掉 `include(GNUInstallDirs)` | `does not name` |

**这一节与 9.6.33 是同一个错误的两次显形**：那一次是"暂存树是空的"，这一次是"暂存树指的
是宿主机"。两次都源于**用一个看起来像路径的变量当路径**，而它回答的是另一个问题。

#### 9.6.39 run 40：`file(RPATH_SET)` 只能改写，不能新增

run 40 的 arm64 停在同一处，但这次的错误**证明前三步都是对的**：

```text
CMake Error at .../snow-shot-linux-runtime-bundle.cmake:200 (file):
  file RPATH_SET could not write new RPATH:
    .../snow-shot-1.2.8-Linux/usr/lib/snow-shot/libjxl.so.0.11
  No valid ELF RPATH or RUNPATH entry exists in the file;
```

路径里 `_CPack_Packages/Linux/DEB/snow-shot-1.2.8-Linux/usr/lib/snow-shot/` 全对
——`DESTDIR` 成立了；文件名是 `libjxl.so.0.11` 而不是 `libjxl.so.0.11.0`
——soname 命名成立了；能列出这个文件说明闭包解析也成立了。红的是**最后一环**。

`file(RPATH_SET)` 的语义是"改写已有的 RPATH 字符串"，不是"给文件加一条"。而
`cmake --install` 默认**剥掉**构建树 RPATH（`INSTALL_RPATH` 为空），所以我们装进
`<prefix>/lib` 的每个依赖库**一条 RPATH 都没有**。于是：

* 这条命令在第一个私有库上就中止整个打包；
* 更要紧的是，即使它不中止，库也拿不到 `$ORIGIN`——而 `libjxl.so.0.11` 必须靠
  **自己的** RUNPATH 找到同目录的 `libhwy.so`，因为 `DT_RUNPATH` 不继承给间接依赖。

**能在哪里加 RUNPATH，就得在哪里加。** 那是依赖构建时：

```bash
'-DCMAKE_INSTALL_RPATH=$ORIGIN'
```

放在 `configure_build_install()` 里，一条覆盖十个 CMake 依赖。此后 `file(COPY)` 复制的是
带正确 RUNPATH 的字节，私有目录内的相互引用自然成立。

打包脚本里那一步**保留**，但只用于"改写指向构建前缀的绝对路径"这种情形，并且把
`No valid ELF RPATH or RUNPATH entry` 当作**正常**结果统计，而不是错误：

```cmake
execute_process(COMMAND "${CMAKE_COMMAND}" "-DRPATH_FILE=${_library}" -P "${_rpath_probe}" ...)
if(_rpath_result EQUAL 0)                       # 改写了
elseif(_rpath_output MATCHES "No valid ELF RPATH or RUNPATH entry")
    list(APPEND _no_entry "${_library}")        # 叶子库，保持原样
else()                                          # 别的错才致命
```

为什么非要开子进程：这条失败在当前进程里是**致命**的，`file(RPATH_SET)` 一出错就终止脚本，
没有"捕获"这种写法。一次打包十几个库、十几次 `cmake -P`，代价可以忽略。

门禁跟着改了两处，第二处值得记：

1. 新的针 `-DCMAKE_INSTALL_RPATH=$ORIGIN`（对着 `scripts/build-linux-deps.sh`），以及
   `-DRPATH_FILE=` / `No valid ELF RPATH or RUNPATH entry`；
2. **原有的 `RPATH_SET` 语法探针必须去掉字符串转义再执行**。调用现在写在
   `file(WRITE ...)` 的字符串里，抽出来的文本带 `"` 和 `\$`；不还原就直接跑，探针
   测的是自己的转义，而不是脚本的签名。

反向验证扩到 **17 变异 + 1 对照**。第 17 条（删掉依赖构建的 `INSTALL_RPATH`）顺带暴露了
反向验证脚本自己的一个洞：快照列表里没有 `build-linux-deps.sh`，一次 SIGTERM 卡在
"改完未复位"的窗口里，下一次运行会因为基线已红而拒绝启动。**快照列表要覆盖所有会被
改的文件，而不是"主要的那个"。**

**这一节的结论可以压成一句**：`file(RPATH_SET)` 和 `$ORIGIN` 是两件事——前者修的是
"已经指向错地方的路径"，后者要求"路径能被写进去"，而后者只有在编译期才可能。

---

#### 9.6.40 run 41：`.deb` 首次成型，红移到 AppStream 的 schema 规则

run 41（`faf62a24`）出现了这个项目在 Linux 上的第一个**真包**：

```text
Run Build Utility from CPack
CPack: - package: /home/runner/work/snow-apps/.../snow-shot_1.2.8_arm64.deb generated.
package-file "snow-shot_1.2.8_arm64.deb" generated
```

`Build` 两架构第五次连绿，`Build the Debian package` 一次通过。这同时确认了前三层：

* `CPACK_DEBIAN_PACKAGE_SHLIBDEPS=ON` 下 `Depends` 是 `dpkg-shlibdeps` 从 ELF 的
  `NEEDED` 推出来的，内容形如 `libqt5core5t64 …, libqt5gui5t64 …, libqt5widgets5t64 …`
  ——**没有**任何一条是手写的（§9.6.31 那条铁律成立）；
* 私有库按 soname 落进 `usr/lib/snow-shot/`：`libIex-3_4.so.33`、`libImath-3_2.so.30`、
  `libhwy.so.1`、`libjpeg.so.62`、`libjxl.so.0.11`……**后缀与 DT_SONAME 一致**，
  这是 §9.6.37 的 soname 解析在真机器上的验证；
* `Architecture:` 由矩阵推出 `arm64`/`amd64`，与 runner 一致（§9.6.25）。

红在一个从未被任何本地门禁覆盖的地方：

```text
E: snow-shot.desktop:37: release-time-missing date
Validation failed: errors: 1, warnings: 1, infos: 2
```

**`release-time-missing` 不是 XML 良构性问题，也不是 `desktop-file-validate` 的规则。**
它是 AppStream schema 的一条约束：每个 `<release>` 元素必须带 `date`，格式 `YYYY-MM-DD`。
仓库自己的 `check-linux-freedesktop.py` 只检查结构（良构、id 一致、图标命名），
`desktop-file-validate` 只看 `.desktop`，所以这个错**只能在 `appstreamcli` 里出现**——
而 `appstreamcli` 原本只装在 build job 里，代价是"整套构建跑完、打完包之后"才发现。

日期不能编。取法是把版本号的引入点找出来：

```bash
git log -S "1.2.8" --oneline
# 7e4347d7 build(release): bump snow shot to 1.2.8
```

于是 `<release version="1.2.8" date="2026-10-13"/>`。防线加两层：

1. `check-linux-freedesktop.py` 增加"每个 `<release>` 必须有 `YYYY-MM-DD` 的 `date`"，
   注释里写下 `release-time-missing` 的原文，免得后来的人以为这条规则是自造的；
2. `gates` job 新增 `Freedesktop metadata with the real validators`：装
   `appstream desktop-file-utils`，用与 build job 的 `Validate the packaged metadata`
   **完全相同的参数**跑两个校验器。不涉及编译器，几秒钟，红在允许红的 job 里。

##### 新步骤让门禁自己红了三条，而那是门禁的错

加完上面这两层，`check-linux-ci-workflow.py` 立刻报：

```text
FAIL gates: installs dpkg-dev: missing from apt list
FAIL gates: installs qtbase5-private-dev: Qt5::GuiPrivate would be missing and 19 link lines would fail
FAIL gates: installs patch: the OpenCV in-memory-model patch could not be applied
```

不是 workflow 缺包。`gates` 的定义写在它的 `name` 里：**`Linux gates (no toolchain)`**
——它只跑 `cmake -P` 脚本模式、cargo 和两个元数据校验器，连编译器都没有。
原来的断言之所以一直绿，是因为它长这样：

```python
packages = apt_packages(text)
if not packages:
    continue                       # ← gates 没有 apt-get install，整个循环跳过
for tool in ("desktop-file-utils", "appstream", "dpkg-dev"):
    check(...)
check(... "qtbase5-private-dev" ...)
check(... "patch" ...)
```

**"没有 apt 列表就跳过"**掩盖了"这套要求本来只对构建 job 成立"。给 `gates` 加第一个
`apt-get install` 的那一刻，它就开始要求一个不编译任何东西的 job 去装 Qt 私有头。

修法不是把三个包塞进 `gates`（那是把门禁的错当成流程的错），而是**让要求从 job 自己做什么
推出来**：

| 要求 | 依据（读 job 的 `run` 文本） |
|---|---|
| `desktop-file-utils` / `appstream` | 该 job 调用 `desktop-file-validate` 或 `appstreamcli` |
| `dpkg-dev` | 该 job 调用 `cpack --config` 或 `dpkg-deb` |
| `qtbase5-private-dev` / `patch` / `squashfs-tools` | 该 job 调用 `cmake --build --preset` 或 `build-linux-deps.sh` |
| `libopencv-dev` 必须缺席 | 所有 job（任何 job 装它都是两个来源） |

`gates` 现在只被要求前两组里的第一组，`build` 全中——两边的错都会红。

##### 同一个循环里另外两个洞

1. **`apt_packages` 只读第一个 `apt-get install` 块。**`break` 出去的写法让 job 里的第二个
   安装块**完全隐形**：把一个包从第一块挪到第二块，"缺包"就变成"装好了"。改成逐行扫完
   整个 job 的 `run` 文本，遇非续行只结束当前块。差分实测：把 `build` 的两个校验器拆到
   独立安装块，新读法 `sees=True`、旧读法 `sees=False`，而门禁仍绿——差别是真的。
2. **"校验器步骤不见了，包检查就变空转。"**上面的包断言是条件式的（job 调了才要求），
   于是删掉那个 step，断言连同它守的东西一起消失、还报 ok。所以另加
   `metadata_validator_checks()`，直接按名字锚定两个 step 并断言它们各自调用两个校验器，
   参数里都带 `--no-net`——两处不许各说各话。

反向验证四条变异全部变红、恢复全部变绿：`gates` 少 `appstream`、`gates` 不再调
`appstreamcli`、`build` 少 `qtbase5-private-dev`、`build` 的 step 改名。

**这一节的结论**：`Build` 和打包容器的绿灯是靠"每一步只信机器给的证据"攒出来的；
而门禁一旦把 A job 的要求套到 B job 上，它红得比 CI 还早——那时该改的是门禁的**依据**，
不是被它指着的那个文件。

#### 9.6.41 run 42：`gates` 秒级红在 `cid-desktopapp-is-not-rdns`

run 42（`39881231963`，`be7f2da7`）里，`Build the Debian package` 继续绿，**新加的那一步
在 `gates` 里几秒钟就红了**——这正是它存在的理由：同样一条规则如果留给
`Validate the packaged metadata`，要等半小时编译之后才说出来。

```text
W: snow-shot.desktop:3: cid-desktopapp-is-not-rdns snow-shot.desktop
I: snow-shot.desktop:21: developer-name-tag-deprecated
I: snow-shot.desktop:~: developer-info-missing
✘ Validation failed: warnings: 1, infos: 2                     (exit 3)
```

上一轮的 `release-time-missing`（error）消失了，露出的是**警告级**的失败：
`appstreamcli validate` 把 warning 也算失败，只有 info 可以容忍。所以这条不能"忽略"。

##### 规则本身（读源码，不靠印象）

`as_validator_validate_component_id()`（appstream `src/as-validator.c`）对
`desktop-application` 组件做的事，逐条翻译成中文：

| 判据 | 触发的问题 id | 级别 |
|---|---|---|
| 点分段数 < 3 | `cid-desktopapp-is-not-rdns` | warning |
| 首段不是真实 TLD（`as_utils_is_tld()` 查内建 IANA 列表） | `cid-maybe-not-rdns` | warning |
| 前两段含大写 | `cid-domain-not-lowercase` | warning |
| 非末段含连字符 | `cid-rdns-contains-hyphen` | warning |
| 任意位置含大写 | `cid-contains-uppercase-letter` | warning |
| 字符集超出 `[A-Za-z0-9._-]` | `cid-invalid-character` | error |
| 任一段以数字开头 | `cid-has-number-prefix` | warning |

`cid-desktopapp-is-not-rdns` 只在**段数**上触发，注释写得很直白：因为 desktop app 的 id
与 `.desktop` 文件名是绑在一起的，不能把它升级成 error。但 warning 已经足以让命令失败。

##### 取值：`com.snowshot.snow_shot`

不是随手取的，它同时满足七条判据，并且**与 macOS 的 `bundleId()` 同源**
（`"com.snowshot." + applicationName()`）：

- 三段：`com` / `snowshot` / `snow_shot`；
- 首段 `com` 是真实 TLD；
- 前两段全小写；
- 非末段无连字符——这条排除了 `com.snowshot.snow-shot`（也就是 `productId()` 的写法，
  它每一段都"看起来对"，但在 id 里是不合法的）；
- 下划线在字符集内（源码里显式允许 `.` `-` `_`），所以 `snow_shot` 没问题；
- 没有段以数字开头。

##### 改名要落到四个不同的地方，而且两个地方**不能**跟着改

| 落点 | 值 | 为什么 |
|---|---|---|
| `snow_shot/packaging/linux/com.snowshot.snow_shot.desktop` | 文件本身 | AppStream 要文件名 = `<id>.desktop` |
| `<id>` / `<launchable>` | `com.snowshot.snow_shot` / `…snow_shot.desktop` | 组件 id 与启动目标 |
| `QGuiApplication::setDesktopFileName()` | `desktopEntryId() + ".desktop"` | 不匹配就多一个无图标槽位 |
| `Icon=` / 安装的 SVG/PNG 名 | `snow-shot`（**不改**） | 图标名没有反向域名要求 |
| `AutoStartRegistration` 的 `~/.config/autostart/<name>.desktop` | `snow-shot.desktop`（**不改**） | 用户的既有覆盖文件就叫这个名，改了等于把所有人关掉的开机自启重新打开 |

新增 `app::edition::desktopEntryId()`，注释里写明它**不是** `productId()` 以及为什么。
`edition.h` 里因此出现两个"应用名"：一个给可执行文件/设置目录/自启条目，一个给桌面条目。

##### 门禁当场抓到的那个错：`set(... PARENT_SCOPE)` 不定义本地变量

AppImage 布局函数里先写成：

```cmake
set(SNOW_SHOT_APPIMAGE_DESKTOP_NAME "com.snowshot.snow_shot.desktop" PARENT_SCOPE)
set(SNOW_SHOT_APPIMAGE_DESKTOP
    "${_snow_dir}/usr/share/applications/${SNOW_SHOT_APPIMAGE_DESKTOP_NAME}" PARENT_SCOPE)
```

第二行的 `${SNOW_SHOT_APPIMAGE_DESKTOP_NAME}` 在**本作用域**里是空的——`PARENT_SCOPE`
只写出到调用方，不定义自己。于是路径变成 `…/applications/`，文件名为空。
`check-linux-packaging.cmake` 的指纹断言立刻打印：

```text
FAIL appimage installed entry: expected '…/applications/com.snowshot.snow_shot.desktop',
                               got '…/applications/'
```

修法是先设本地变量再导出。这和 9.6.38 的 `CMAKE_INSTALL_PREFIX`、9.6.10 的
`CMAKE_CURRENT_LIST_DIR` 是同一族错误：**CMake 的作用域是显式的，不能靠"它应该也在"。**

##### 新加的三组断言（都能反向验证）

1. **`check-linux-freedesktop.py`**：把上表七条判据全部写成断言，外加
   `metainfo file is named after the id` 与 `metainfo id is the entry file name without
   its suffix`。本地内建 `KNOWN_TLDS` 集合，注释说明它是"门槛"而不是 IANA 镜像。
2. **`check-linux-ci-workflow.py`**：两个校验步骤必须指向**同一批**文件——
   `named == shipped`，`shipped` 取自 `snow_shot/packaging/linux/` 的实际文件。改名只改
   一处、另一处留着旧路径，这条会红（`appstreamcli` 对不存在的文件同样 exit≠0）。
3. **`check-linux-packaging.cmake`**：`cmake/SnowShotAppImage.cmake` 里
   `*.desktop` 字面量只能出现**一次**（定义处），AppDir 根副本必须经
   `${SNOW_SHOT_APPIMAGE_DESKTOP_NAME}`。否则同一个名字写两遍，改一处就够红一次。

反向验证共 10 条变异，全部变红、恢复全部变绿：

```text
id 掉到两段               → cid-desktopapp-is-not-rdns（5 条断言同时红）
id 换了不存在的 TLD        → 已知 TLD
id 首段大写               → 小写 / 无大写
id 在非末段插连字符         → 非末段无连字符
id 与文件名不一致           → without its suffix
desktopEntryId 改回 productId → entry file name
一个校验步骤留旧路径        → validates the files that ship
AppDir 根副本写字面量       → entry name is written once
布局名置空 / 改成别的        → appimage installed entry
```

##### 一条没动的 info

`I: developer-name-tag-deprecated` 说明 `<developer_name>` 在 AppStream 1.0 里已被
`<developer><name>` 取代。**没改**，因为 UOS 20 自带的 appstream 版本比 CI 的旧，老版本
不认识 `<developer>` 元素，会把 info 变成 error。info 不致命，先留；真机验证时用 UOS 20
的 `appstreamcli` 复核一次再决定。

---

#### 9.6.42 run 43：`.deb` 全绿，`cmake -E chmod` 与一个"在树里却报缺"的依赖

run 43（`40897097354`，`1b259db0`）第一次把整条 deb 路径走完，并在第一步**真正执行**的
AppImage 上停住。

```text
Linux gates (no toolchain)          all steps  success   ← 含两步元数据校验
Build and package arm64
  10. Build                          success
  11. Build the Debian package       success
  12. Validate the packaged metadata success             ← 首次
  13. Upload the package             success
  14. Fetch appimagetool             success             ← 首次
  15. Build the AppImage             failure             ← 首次，死因见下
  16/17/18                           skipped
```

##### 死因一：`cmake -E` 里没有 `chmod`

命令链最后一条自写出那天起就是：

```cmake
COMMAND "${CMAKE_COMMAND}" -E chmod +x "${SNOW_SHOT_APPIMAGE_DIR}/AppRun"
```

`cmake -E` 是一张**固定的命令表**（`Source/cmcmd.cxx`，4.4 上是 63 项），`chmod` 不在里面。
未知子命令不是警告而是硬错误，并且会把四十行 usage 一起打出来：

```text
CMake Error: cmake version 4.4.4
Usage: cmake -E <command> [arguments...]
  bin2c / capabilities / cat / chdir / compare_files / copy / … / true / false
```

在那一长串里找 `chmod` 是找不到的——这正是它一直没被发现的原因：**这条命令只能失败，
不能成功**，而它所在的 target 在 run 43 之前从未被执行过（AppImage 的三步一直排在
`Validate the packaged metadata` 之后）。

而且它本来就是**多余**的。`_cmake_E_copy` 走的是 `cmSystemTools::CopyFileAlways`：

```cpp
mode_t perm = 0;
Status perms = SystemTools::GetPermissions(source, perm);
...
if (perms) {
  status = CopyStatus{ SystemTools::SetPermissions(real_destination, perm), ... };
}
```

源文件 `AppRun` 在索引里是 `100755`，所以 `-E copy` 已经把执行位带过去。修法是**删掉那行**，
而不是找个替代的 chmod。

门禁因此加了一条更通用的断言：把 AppImage 模块里**每一个** `-E <sub>` 抽出来，逐个对照
CMake 真实的命令表。这样下一个 `-E mkdir` / `-E ln`（都是"听起来应该有"的名字）也会在秒级
被发现，而不是等三十分钟构建。注释先被剥掉再匹配——文档里现在写着 `cmake -E chmod`，
不剥就会匹配到自己的解释（§9.6.6 那个"门禁匹配到自己注释"的老坑）。

##### 死因二：`unresolved libsnow_shot_image_codec_backend.so`

bundle 脚本同一次运行里报了它：

```text
-- snow-shot bundle: 201 resolved dependency file(s), 1 unresolved
-- snow-shot bundle: unresolved libsnow_shot_image_codec_backend.so
```

不是缺库，是**搜索路径没覆盖到它**。这个库（图像编解码后端）是项目自己的产物，装在
`usr/lib`，**不在**私有目录 `usr/lib/snow-shot`：

```cmake
install(TARGETS snow_shot_image_codec_backend
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} ...)
```

这个位置是对的：`.deb` 里 `/usr/lib` 是加载器默认路径；AppImage 里 `AppRun` 有
`LD_LIBRARY_PATH=$APPDIR/usr/lib`。但 `file(GET_RUNTIME_DEPENDENCIES)` 只查
`DIRECTORIES` 加上**宿主**系统路径，暂存树里的 `usr/lib` 不在其中，于是它把"在树里的库"
报成缺失。

这种假缺失比噪音更坏：下一条**真的**缺失会藏在它后面。修法是把暂存库目录也交给求解器：

```cmake
DIRECTORIES "${_deps_lib_dir}" "${_staged}/${_lib_subdir}"
```

放进这个目录并不会多打包一个文件——决定"是否打包"的是下面那句 `realpath` 目录比较，只认
依赖前缀下的条目。

##### 第二条运行日志的 0 个库不是回归

同一次 `cmake --install` 里脚本被调了两次：一次是安装规则（走 `DESTDIR`），一次是 AppImage
target 手动的 `-DSTAGED_DIR=`。第二次打印 `$ORIGIN written into 0`、`nothing was bundled`：

第一次已经把 13 个私有库复制进 `lib/snow-shot`，第二次求解器顺着刚写进去的
`$ORIGIN/../lib/snow-shot` 就在暂存树里找到了它们，`realpath` 不在依赖前缀下 ⇒ 不再算"需要
打包"。库是在的，`check-linux-appimage-build.py` 的 "the package bundles shared libraries"
会证实。那句 `WARNING` 的措辞改成了三种情形的枚举，免得下次读日志的人以为坏了。

##### 反向验证

| 变异 | 结果 |
|---|---|
| 把 `-E chmod` 加回去 | 红：`'cmake -E chmod' … not one of the 63 sub-commands` |
| 换成一个"应该有"的 `-E cp` | 红：同上 |
| 从 `DIRECTORIES` 里去掉暂存库目录 | 红：`searches only the dependency prefix` |
| 删掉 gates 里的 AppRun 检查 | 红：`gates job checks the AppRun bit` |
| 把 `test -x` 改成 `test -f` | 红：同上 |

五条全红、恢复全绿。另外 gates job 新增一步 `test -x AppRun`：执行位来自索引，而
`-E copy` 只是把它带过去——万一某次重新添加文件把模式丢了，这一步一秒钟就能说，不必等
`appimagetool` 在半小时后报错。

##### 下一步的真正缺口

`Build the AppImage` 通过之后，产物**仍然起不来**：`AppRun` 把 `QT_PLUGIN_PATH` 指向
`$APPDIR/usr/plugins`，而 Linux 侧没有任何规则往那儿拷 Qt 插件（唯一的 plugin 部署块只有
Windows 分支，只挑 `platforms/qwindows.dll`）。`.deb` 靠 `Depends: libqt5gui5` 用宿主的 Qt
所以能跑，AppImage 不打包 Qt 就不行。`Validate the AppImage` 只做结构校验，看不出来。这是
§9.6.44 要做的事。

---

#### 9.6.43 `Unit tests` 的环境前提：183 个测试源要一个显示

`Unit tests` 是 workflow 里最后一道关卡，也是唯一一条**从没执行过**的：

```yaml
cmake --preset ${{ matrix.preset }} -DSNOW_APPS_BUILD_TESTS=ON
cmake --build --preset ${{ matrix.build-preset }} --parallel 4
ctest --test-dir build/${{ matrix.preset }} --output-on-failure --no-tests=error
```

先确认了三件不成立会立刻失败的事：

| 疑问 | 结论（按源码查，不是印象） |
|---|---|
| 根目录 `ctest --test-dir build/<preset>` 找得到子目录里的测试吗 | `CMakeLists.txt:176` 在 `if(SNOW_APPS_BUILD_TESTS OR …)` 里 `enable_testing()`，而 CI 正是带 `-DSNOW_APPS_BUILD_TESTS=ON` 重新配置，根 `CTestTestfile.cmake` 会有 `subdirs("snow_shot")` |
| 测试可执行文件会不会被 `--build` 跳过 | `snow_shot/CMakeLists.txt` 里 `EXCLUDE_FROM_ALL` 出现 **0** 次，默认目标包含全部测试 |
| 会不会有 Windows/macOS 名字的测试注册到 Linux | 660 处 `add_test` 里，名字带 `windows`/`macos` 的 68 处**全部**在 `if(WIN32)`/`if(APPLE)` 内 |

##### 第四件事：runner 没有显示，而 183 个测试源要 `QApplication`

`python` 扫一遍 `snow_shot/tests`：**289 个测试源里 183 个**引用 `QApplication` /
`QWidget` / `QGuiApplication`。在无显示的 runner 上，这些用例会在
`QApplication` 构造时就 abort：

```text
qt.qpa.xcb: could not connect to display
This application failed to start because no Qt platform plugin could be initialized.
```

那不是一百个产品缺陷，是**一个环境缺陷**。修法是把 `ctest` 放进虚拟显示：

```yaml
xvfb-run -a --server-args="-screen 0 1280x1024x24" \
  ctest --test-dir build/${{ matrix.preset }} --output-on-failure --no-tests=error
```

选 `xvfb-run` 而不是 `QT_QPA_PLATFORM=offscreen`：项目里确实有 `snow_shot_offscreen_qpa_environment()`
给少数用例用，但 offscreen 是**另一个 platform plugin**，而这个移植的产物是 X11 应用——
用 xcb 跑测试，测的才是发货时走的那条路。`xvfb` 进 build job 的 apt 列表。

门禁加三条，缺一条就是把环境缺陷留给 CI 去"证明"：`ctest` 出现的地方必须有 `xvfb` 包、
必须有 `xauth` 包，并且 `ctest` 必须在 `xvfb-run` 之后。变异（删 `xvfb`、删 `xauth`、去掉包装）全红。

`xauth` 单独列出来是因为它不是依赖：`xvfb` 包只 **Recommends** `xauth`，而 `xvfb-run`
每次都要用它生成授权文件，缺了就直接失败（Debian bug #830661 的结论是"加 Depends 才是错的"）。
apt 默认装 Recommends，所以今天能过——但这是"默认值恰好对"，不是"依赖成立"。

##### 与 Windows 侧的差异：不是照抄，是预算

`release.yml` 的 Windows CI **不构建全部测试**，它只 `--target` 几个聚焦用例再 `-R` 跑。
Linux 这条步骤构建全部。估算过成本：Linux 可见的测试可执行文件 **214 个**，列出的源条目
**1099** 个（`${VAR}` 展开后会更多），每个 target 独立编译，4 核下约十几到几十分钟——在
180 min 预算内。所以先保持"全跑"，不用 `-R` 收窄：收窄会把跨平台逻辑的编译错误藏起来，
而那正是这次移植要发现的东西。

#### 9.6.44 run 44：`Validate the AppImage` 红在"门禁自己换了目录"

run 44 是第一次让 AppImage 打完就验：`Fetch appimagetool` 过、`Build the AppImage`
过、`Validate the AppImage` 红，**两个架构红在同一行**：

```text
== build/snow-shot-linux-x64-release/snow-shot-1.2.8-x86_64.AppImage
-rwxr-xr-x 1 runner runner 55871992 Oct 29 16:43 … snow-shot-1.2.8-x86_64.AppImage
ok   the AppImage is an ELF file
ok   the AppImage carries the type-2 magic
ok   the AppImage is built for x86_64
FileNotFoundError: [Errno 2] No such file or directory:
    'build/snow-shot-linux-x64-release/snow-shot-1.2.8-x86_64.AppImage'
```

同一屏里，`ls -l` 刚把这个 55.8 MB 的文件列出来，`is_file()`、`read_bytes()` 也都读过它，
然后 `subprocess.run` 说不存在。**看见的文件 execve 看不见**——这说明问题不在文件，
在解析文件名的那个 `cwd`。

##### 机制：execve 的相对路径按子进程的 cwd 解析

校验脚本要在 FUSE 缺席的机器上解包，所以它 `cwd=workdir` 切到临时目录再跑
`[str(appimage), "--appimage-extract"]`；而 runner 传进来的 `--appimage` 是相对仓库根的
`build/<preset>/…AppImage`。`chdir` 之后这个相对路径指向了 `tempdir/build/…`，内核
`execve` 直接 ENOENT，Python 把它包成 `FileNotFoundError`（抛在
`subprocess._execute_child`，所以没有"文件不存在"的 Python 栈内上下文）。

```python
# 修法：进门就绝对化，后面谁改 cwd 都无所谓
appimage = Path(args.appimage).resolve()
```

**这个错误在 Windows 工作站上复现不出来**，这不是"没试"：实测同一个探针
（相对路径 + `cwd=`）在 Windows 上返回的是 `WinError 193`（不是有效的 Win32 应用程序），
因为 `CreateProcess` 按**父进程**的 cwd 解析相对路径，`execve` 按**子进程**的 cwd 解析。
平台差异正好把这类错封在 Linux 侧——它只能由"在 Linux 上真跑"暴露，或由文本门禁配对断言。

##### 门禁：两条断言成对出现，缺一条就没有意义

`scripts/check-linux-appimage.py` 新增：

| 断言 | 为什么 |
|---|---|
| 校验器在临时 `cwd` 里跑这个文件（`cwd\s*=\s*(?:str\()?\s*workdir`） | 这是"必须先绝对化"的**前提**；前提没了，下面那条就不必再要求 |
| 校验器先把 AppImage 路径绝对化（`Path(args.appimage).resolve()`） | 相对路径一换目录就解析不到 |

三条变异：删掉 `.resolve()` → 红；把 `cwd=workdir` 改成等价的 `cwd=str(workdir)` → **仍绿**
（这正是要的：门禁咬的是行为，不是某一行的写法）；把 `cwd` 参数整个删掉 → 红。

##### 副产品：产物本身第一次被证明，而不只是"命令返回 0"

这次红之前的四行 `ok` 是**真的过了**，两个架构都过：ELF、`AI\x02` type-2 magic、
`e_machine` 分别是 `0x3E` / `0xB7`。也就是说 appimagetool 拿固定版本的 type-2 runtime
打出来的，确实是**目标架构**的 type-2 AppImage——之前只有"`COMMAND` 没报错"。

##### 教训

门禁脚本自己出错时，报出来的形状和产品缺陷一模一样。区分判据是：
**同一目录下 `ls` 看得见、`execve` 看不见 → 先怀疑路径解析，别怀疑产物。**
另外把 `check()` 的输出加了 `flush=True`：这次日志里 traceback 和 `ok` 行是交错的，
stdout 块缓冲、stderr 不缓冲，而恰恰是出 traceback 的时候最需要顺序可读。

#### 9.6.45 AppImage 不自带 Qt —— 一个被自家注释说反了的事实

run 44 的 `Build the AppImage` 顺手给出了一个此前只是猜测的答案。闭包脚本把
AppDir 里复制到的私有库全列了出来：

```text
snow-shot bundle: private libraries (lib/snow-shot)
  libIex-3_4.so.33  libIlmThread-3_4.so.33  libImath-3_2.so.30  libOpenEXR-3_4.so.33
  libOpenEXRCore-3_4.so.33  libhwy.so.1  libjpeg.so.62  libjxl.so.0.11
  libjxl_cms.so.0.11  libjxl_threads.so.0.11  liblunasvg.so.3  libplutovg.so.1
  libturbojpeg.so.0
```

十三个，全是 `scripts/build-linux-deps.sh` 自建的依赖。**没有一个 `libQt5*`** ——
AppImage 里没有 Qt。原因在代码里很清楚：唯一的 Qt 部署块是
`snow_shot/CMakeLists.txt` 的 `if(WIN32 AND NOT SNOW_SHOT_QT_STATIC)`，Linux 的安装树
从来没有 plugins 目录；而闭包脚本只从*依赖前缀*里复制，Qt 来自 apt，不在前缀里。

##### 注释说反了

`AppRun` 原文写着「A bundle without it dies on the first window」。按 qtbase 5.15 的源码，
这句是错的：

```cpp
// src/corelib/plugin/qfactoryloader.cpp, QFactoryLoader::update()
if (!QDir(path).exists(QLatin1String(".")))
    continue;
```

搜索路径里不存在的目录是**跳过**，不是致命。所以 `export QT_PLUGIN_PATH="$APPDIR/usr/plugins"`
在今天的产物上是纯粹的空操作 —— 危险的不是它会崩，而是它让下一个读的人相信插件在包里。
（`QT_PLUGIN_PATH` 为空/不设时，Qt 用编译期内置路径，也就是宿主发行版那套。）

##### 为什么不是「把插件拷进去」了事

Qt5 在装载插件时按元数据判版本（`src/corelib/plugin/qlibrary.cpp`，
`QLibraryPrivate::updatePluginState()`）：

```cpp
if ((qt_version & 0x00ff00) > (QT_VERSION & 0x00ff00)
    || (qt_version & 0xff0000) != (QT_VERSION & 0xff0000)) {
    errorString = QLibrary::tr("The plugin '%1' uses incompatible Qt library. (%2.%3.%4) [%5]")
```

major 必须相等，插件的 minor 不得高于运行库的 minor，patch 不参与。于是构建机的
5.15.13 插件在宿主的 5.15.10 上能装，在 5.12（老 UOS）上被拒——**而被拒的插件会挡住宿主
本来能用的那一个**，把一台本来能跑的机器弄成不能跑。只拷插件、不拷库，比不拷更糟。

##### 为什么也不是「连 Qt 库整闭包拷」

Debian/Ubuntu 把 Qt5 的库放在通用的多架构目录（`/usr/lib/x86_64-linux-gnu`），和
`libGL`、`libX11` 同一个目录。闭包脚本的筛选判据是「依赖的文件在不在依赖前缀里」，
多目录一混，按目录筛就会把 `libGL` 一起拷走——那是 A.6.2 明令禁止的（GPU 驱动失配）。
改成按名字筛 `libQt5*` 又躲不开 `libQt5Core` 的 NEEDED：`libicu*`，体积与版本耦合再上一层。
这活儿正是 `linuxdeploy-plugin-qt` 干的，不是二十行 CMake 能糊过去的。

##### 落地的决定

**AppImage 的 Qt 依赖 = 发行版 Qt 5.15**，与 `.deb` 通过 `dpkg-shlibdeps` 声明的是同一件事；
信创渠道是 deb，而 A.7 本来也要求优先用系统基础库。代码上只做两件小事：

1. `AppRun` 改成条件导出，并把上面两条源码事实写进注释（否则下一个人还会「修」回去）：

   ```sh
   if [ -d "$APPDIR/usr/plugins" ]; then
       export QT_PLUGIN_PATH="$APPDIR/usr/plugins${QT_PLUGIN_PATH:+:$QT_PLUGIN_PATH}"
   fi
   ```

2. 两处门禁守住这个配对，缺一半就没有意义：
   * 宿主侧 `check-linux-appimage.py`：导出的 `$APPDIR/...` 路径必须有 `-d` 守卫；
   * 产物侧 `check-linux-appimage-build.py`（`check_plugin_path()`）：解包树里**确实**有那个
     目录，或者导出被守卫。这条只有在真产物上才有意义——将来真开始打插件，守卫变真、目录
     必须出现；去掉守卫而没打插件，立刻红。

反向验证：宿主侧删守卫 → 1 条红（干净变异，只改这一处）；产物侧用探针跑四种入参组合
（无条件+无目录 / 有守卫+无目录 / 无条件+有目录 / 完全不导出）→ 期望值与实际全部一致。

##### 未决

真要「一个文件拷到任何机器就能跑」，只有两条路，都不在今天这一步的预算里：

| 路线 | 代价 | 备注 |
|---|---|---|
| `SNOW_APPS_QT_STATIC`（A.6.10） | 需在 `build-linux-deps.sh` 里源码构建 Qt5 静态版 | GPL-3.0 下静态链接成立；插件路径问题整体消失 |
| 引入 `linuxdeploy-plugin-qt` | 打包链多一个外部工具，需固定版本 + 校验和 | 与「不联网、可复现」的既定纪律要一起解决 |

在这两条之前，AppImage 的定位是「发行版已有 Qt 5.15 时免安装试用」，deb 是信创交付渠道。
这句话要写进发布说明，否则用户拿到 AppImage 在没装 Qt 的机器上会看到 loader 的
`libQt5Core.so.5: cannot open shared object file`，而那不是 bug，是没声明的前提。

#### 9.6.46 run 45：校验器把 `Icon=` 当成了文件名

run 45 是 `Validate the AppImage` 第一次真的跑完：

```text
ok   the AppImage unpacks
ok   the package has an AppRun
ok   the AppRun is executable
ok   the package has one desktop entry at its root
ok   the root entry is named after a reverse-DNS id
ok   the desktop entry names an icon
FAIL the named icon is at the root of the package: Icon=snow-shot
ok   the package has the executable
ok   the package bundles shared libraries
ok   the packaged AppRun is the reviewed one
```

`unpack`、`AppRun`、命名、可执行位、依赖库、以及「包里的 AppRun 就是仓库里审过的那份」全过。
`Validate the AppImage` 剩下的唯一一条红，还是校验器自己的错：`Icon=` 是**图标主题里的名字**，
不是文件名；appimagetool 把它映射成 AppDir 根下的 `<name>.<ext>`。`squashfs-root/snow-shot.png`
就在那里，检查却在找 `squashfs-root/snow-shot`。

修法不是「加个 .png」，是把判据换成语义：

* 按 appimagetool 接受的扩展名找（`.png .svg .svgz .xpm`），裸名也算合法；
* 补两条从来没检查过的事：
  * `.DirIcon` 必须存在**且能解析**（它是文件管理器画缩略图用的那个符号链接；
    悬空链接意味着图标压根没进包）；
  * `.DirIcon` 必须是**本应用**的图标——符号链接比名字，普通文件（copy 形态）比字节。

八组探针把边界钉住：只有 png 没有 `.DirIcon` / png+链接 / 只有 svg / 只有裸名 /
什么都没有 / 悬空链接 / `.DirIcon` 是副本 / `.DirIcon` 指向别的应用。期望值与实际全部一致。

##### 顺带看见了 appimagetool 的两句话（以前从没人读到过）

```text
WARNING: AppStream upstream metadata is missing, please consider creating it
         in usr/share/metainfo/com.snowshot.snow_shot.appdata.xml
...com.snowshot.snow_shot.desktop: hint: value "Graphics;Utility;" for key "Categories"
   contains more than one main category
```

1. AppStream 那条是**假阴性**：appimagetool 只找 `*.appdata.xml`，而项目装的是
   `com.snowshot.snow_shot.metainfo.xml`——AppStream 现行推荐的写法，`appstreamcli validate`
   也只认这个。改回 `.appdata.xml` 会同时把 deb 的元数据校验搞砸，所以**不改文件名**；
   AppImage 的 AppStream 集成不是它能启动的前提，记录在此。
2. `Categories=Graphics;Utility;` 的多主分类：freedesktop 允许，`desktop-file-validate`
   不报，只是菜单里可能出现两次。保持现状。

#### 9.6.47 run 46：AppImage 链路全绿，`Unit tests` 红在 375 个 genex

run 46 是打包这条线第一次**整条走通**：

```text
15. Build the AppImage      success     (x86_64 与 aarch64)
16. Validate the AppImage   success
17. Upload the AppImage     success
18. Unit tests              failure
```

`Unit tests` 的红不在测试里，在 **configure 的 generate 阶段**，两个架构同一个原因：

```text
CMake Error at snow_shot/CMakeLists.txt:11302 (add_test):
  Error evaluating generator expression:
    $<TARGET_FILE_DIR:Qt::Core>
  Target "Qt::Core" is not an executable or library.
```

`$<TARGET_FILE_DIR:…>` 这类"要文件"的表达式，参数必须是可以落盘的 target。Qt 5.15 的
versionless 目标（`Qt::Core`）只是 `Qt5::Core` 的 **INTERFACE 包装**——链接、头文件都靠它，
但它没有文件。Qt6 那边 `Qt::Core` 是真正的 IMPORTED SHARED 目标，所以 Windows 一侧
（Qt6）从来没暴露过；Linux（Qt5）第一次带 `-DSNOW_APPS_BUILD_TESTS=ON` 配置就全线崩。

这些表达式的作用是把 Qt 的库目录塞进测试的 `PATH`——**Windows 找 DLL 的需要**。
共 **375 处**，分布在 6 个文件：

| 文件 | 处数 |
|---|---|
| `snow_shot/CMakeLists.txt` | 335 |
| `snow_draw_engine_qt/CMakeLists.txt` | 15 |
| `ant_design_qt/CMakeLists.txt` | 11 |
| `cmake/SnowShotRuntimeTests.cmake` | 7 |
| `snow_image_viewer/tests/CMakeLists.txt` | 4 |
| `cmake/SnowShotMini.cmake` | 3 |

改法不是新造机制，是沿用工程里已有的写法：`${SNOW_QT_NS}::Core`
（`cmake/SnowQtOffscreenTest.cmake` 一直这么用，`SNOW_QT_NS` 就是 `Qt6`/`Qt5`）。
375 处一并替换，Qt6 路线展开成 `Qt6::Core`，行为不变。

##### 门禁 `scripts/check-qt-versionless-genex.py`

两条，缺一条都留着半个洞：

1. 仓库里（排除 `build/`、`.tools/`）的 327 个 CMake 文件，不得出现把**文件表达式**用在
   versionless Qt 目标上的写法：`$<TARGET_FILE*:Qt::…>`、`$<TARGET_LINKER_FILE*:Qt::…>`、
   `$<TARGET_SONAME_FILE*:Qt::…>`、`$<TARGET_PDB_FILE*:Qt::…>`；
2. `cmake/SnowQt.cmake` 必须给两条路线都设 `SNOW_QT_NS` —— 替换后的 `${SNOW_QT_NS}`
   若展开为空，会变成 `$<TARGET_FILE_DIR::Core>`，同一类错往后挪一层，更难读。

**门禁自己先错了一次**：第一版正则写成 `\$(?:TARGET_FILE|…)`，漏掉了 `$<` 里的 `<`，
于是"放回一处 `Qt::Core`"的变异**没有变红**——假绿。补上 `<` 后变异立刻红。这是这条
移植工作里第五次"尺子错"，也再一次说明：**门禁的第一次运行必须伴随一个已知会失败的输入**。

##### 一个反复出现的形状

run 42→46 五次红，全部落在"我们自己的工具"里：`.desktop` 的 rdns、不存在的
`cmake -E chmod`、门禁 `chdir` 后的相对路径、把 `Icon=` 当文件名、genex 用 versionless 目标。
产品侧每一步都在前进：deb 全绿 → AppImage 打得出来 → 打得对 → 布局齐全 → **整条链路绿**。
红在哪一行，先问那行是产品还是尺子。

---

#### 9.6.48 run 41418087468：AVIF 在 Linux 上"能选不能存"——被 default-features 骗了

run 41418087468（`73132118`）把失败从 113 条压到 99 条（arm64），剩下的失败里有两条是
**同一个确定性的功能缺口**：

```
AVIF (libheif/AOM) encode failed: No encoder is available for the requested format.
quick save must create the configured directory and use format/name settings:
  format=avif stage=5 error=No encoder is available for the requested format.
```

`stage=5` 是编码阶段，`format=avif` 是测试里那一轮循环的当前格式 —— 也就是"设置里选得出
AVIF、文件对话框里列得出 AVIF、MCP 的格式表里也有 AVIF，但真去存就报没有编码器"。

**根因在 9.6.13 自己的注释里**：当时写的是"overlay port 的 default-features 只有 `hevc`，
所以 Windows 也没有 aom，Linux 上补 aom 就不一致了"，于是 `WITH_AOM_DECODER=OFF` /
`WITH_AOM_ENCODER=OFF`。但 `vcpkg.json` 的两个架构条目都是显式的

```json
{ "name": "libheif", "default-features": false, "features": ["aom"] }
```

`default-features: false` + `features: ["aom"]` —— Windows 侧**只有 aom**，连 hevc 都没有。
只看 port 的 `default-features` 而不看 manifest 的调用点，得出的结论正好相反。

修法（三处一起，缺一不可）：

1. **`libaom-dev` 进 apt 清单**（§9.1 同步）。aom 走发行版，和 brotli / lcms2 同一档：
   libheif 用它自己的 `cmake/modules/FindAOM.cmake`，那是 pkg-config 查找，`.deb` 提供的
   `aom.pc` 就够，不需要 CMake config。libheif 1.21 要求 aom ≥ 3.0，24.04 是 3.8.2。
2. **`WITH_AOM_DECODER=ON` / `WITH_AOM_ENCODER=ON`**，并**必须**同时应用 overlay port 的
   `aom-encoder-context-lifetime.patch`。带 alpha 的静图会让 libheif 在同一个 encoder 对象上
   先编彩色再编 alpha，上游 1.21 会对一个仍然活着的 context 再调一次 `aom_codec_enc_init`
   —— Windows 从有 AVIF 那天起就带着这个补丁。linux-deps 脚本走的是未打补丁的 release
   tarball，所以补丁要显式应用（`apply_patch`，与 OpenCV 那条同一个函数）。
3. **两条断言，因为"编解码器没编进去"是静默的**：配置前 `pkg-config --exists aom`；安装后
   `ldd libheif.so.* | grep libaom` —— 后者才是"AVIF 真的在库里"的证据。libheif 跳过某个
   codec 时不打印任何东西。

另外把 libheif 的 stamp key 从 `1.21.2` 改成 `1.21.2+aom`：**只按版本号打戳的话，改开关
对已经构建过的工作站完全无效** —— stamp 命中就跳过，AVIF 永远缺着，而 CI 每次都从零构建，
两边症状不一致。

判据：产物里 `libheif.so` 的 NEEDED 里有 `libaom.so.*`；`quick-save-export` 与
`export-artifact` 的 avif 轮次通过（本地 Windows 上同一条循环一直通过，因为 vcpkg 侧
`aom` 特性是开着的 —— 这正是"两个平台的同一个程序"应当表现一致的地方）。

**一个遗留风险，记在这里而不是等它变成安装失败**：aom 走发行版就等于走 `Depends`，
`dpkg-shlibdeps` 会把它写成 24.04 的 `libaom3`；而 UOS 20（Debian 10 底）只有 `libaom0`，
soname 不同。libde265/x265 早就是这个处境，所以这不是新问题，只是 AVIF 让它多了一个包。
真要在"裸 UOS 20"上装 deb，出路是把 aom 也当私有库带进包（与 libjxl 同一套闭包机制）；
CI 目前只验证 24.04，这条留到真机验证阶段一起定。

---

#### 9.6.49 `#ifndef Q_OS_MACOS` 不是 `#ifdef Q_OS_WIN`（设置项版）

第二轮把 `settingscatalog.cpp` 里 4 个采集项的平台期望改成三分（macOS / Windows / 其他），
但那只是**让测试承认现状**：Linux 上确实挂着这些项。现状本身是错的。

四个项分别是 `screenshot.api-mode`（Auto/DXGI/WGC/GDI）、`screen-recording.api-mode`、
`screenshot.window-element-api`（MSAA/UIA）、`screenshot.restore-original-screen-colors`。
它们描述的是 Windows 的采集后端与色彩滤镜，而守卫写的是 `#ifndef Q_OS_MACOS` ——
读起来像"非苹果"，实际在 Linux 上同样成立。于是 Linux 版在一条根本不读这些键的
X11/Wayland 管线上摆出了"抓屏 API：DXGI"，还给一个后端是 stub、`hasService()` 恒 false 的
选择器（C.12）摆出了窗口元素 API。

修法是让守卫说真话：4 处 `#ifndef Q_OS_MACOS` → `#ifdef Q_OS_WIN`。**Windows 与 macOS 的
真值完全不变**，所以这条改动只影响 Linux：`Advanced capture` 整节消失，录制页少一行 API 选择；
两个键的存储值与 `SettingsSelectBinding` 映射都保留（只是不再提供入口）。

连带的三处 bookkeeping：Linux 的 `sectionCount` 52 → 51（少的就是 `capture-compatibility`）；
测试里 `nonMacCaptureItems` 这个"非 mac 但也有"的集合删掉，改成 macOS/Linux 都断言缺席；
`settings_page_loading_tests` 的 `ScreenshotCaptureCompatibility` 重置用例从
`#ifndef Q_OS_MACOS` 改成 `#ifdef Q_OS_WIN`（Linux 上那节已经不存在，重置自然是空操作）。

**门禁**：新增 `scripts/check-linux-windows-only-settings.py`，进
`check-linux-packaging.sh`。它盯两件事 —— 定义行所在分支、以及**每个调用点**所在分支，
都必须有显式的 Windows 条件（`#ifdef` / `#if defined` / `#elif`）。只盯定义是不够的：
把调用点单方面改回 `#ifndef Q_OS_MACOS`，定义仍在 Windows 分支里，项就重新出现在 Linux 上，
而这正是最初那个 bug 的形状。反向验证：只把调用点改回去 → 门禁红（3 条），还原 → 绿。

---

## 10. 工作量 / 优先级矩阵

| 编号 | 工作项 | 规模 | 风险 | 优先级 |
|---|---|---|---|---|
| W1 | Qt5 兼容层 + CMake 双版本 | 大 | 高 | **P0** |
| W2 | `snow_image_viewer` RHI 重写/裁剪 | 大 | 高 | P1 |
| W3 | Rust 屏幕采集（X11 / PipeWire） | 大 | 高 | **P0** |
| W4 | C++ `platform/linux/*` | 大 | 中高 | **P0** |
| W5 | 音频 / 输入钩子 / AT-SPI | 中 | 中 | P1 |
| W6 | NEON SIMD 补强 | 中 | 中 | P1 |
| W7 | 打包（deb/AppImage/.desktop） | 小 | 低 | P1 |
| W8 | 全局快捷键 + portal 适配 | 中 | 中高 | P1 |
| W9 | onnxruntime CPU EP 接入 | 小 | 低 | P1 |
| W10 | crashpad 移除/替换 | 小 | 中 | P2 |

---

## 11. 未决问题与建议

1. **Qt5 下限**：建议 **5.15 LTS**。若必须 5.12 或更低，W1 规模再增约 30%–50%。
2. **查看器取舍**：建议 Linux 首版不交付 `snow_image_viewer`，把 RHI 重写排到 P5 之后。
3. **防截屏排除能力**：Linux 无系统级等价 API，需在 UI 明确降级说明（避免误导性提示）。
4. **crashpad**：Linux 下建议直接移除，避免引入不稳定第三方崩溃链路；或后续评估 Breakpad。
5. **Wayland vs X11**：UOS 20 默认 X11 可先行；Deepin 23 / UOS 后续版本 Wayland 占比上升，
   建议从一开始就把采集/输入抽象成 `LinuxBackend` 接口下的 `x11` / `wayland` 两个实现，避免二次重构。
6. **真机验证**：arm64（飞腾/鲲鹏/麒麟 990）必须真机终验，交叉编译不可替代。

---

## 附录 A：AppImage 打包路线与「保留 Qt6」可行性

### A.1 直接回答

**技术上可行，但约束只是换了地方。**

AppImage 把应用**连同 Qt 一起**打进单个可执行文件，运行时由 `AppRun` 设置 `LD_LIBRARY_PATH` /
`QT_PLUGIN_PATH`，因此**不依赖宿主系统的 Qt 版本**——这也意味着**不再需要把代码降级到 Qt5**：
`QKeyCombination`、`QNativeInterface`，乃至 `snow_image_viewer` 的 `QRhi` 都可以原样保留。

但它同时引入一个**比 Qt5 更硬的约束：glibc 基线**。AppImage **无法打包 glibc**
（glibc 不可重定位，官方亦明确不支持声明最低 glibc 要求），宿主的 glibc 必须 ≥ 构建基线。

> 结论：**能否在 UOS 上保留 Qt6，不取决于 AppImage，而取决于「目标机 glibc 版本」与「本工程 Qt 6.12 的构建基线」的相对关系。**

### A.2 AppImage 能打包什么 / 不能打包什么

| 能打进 AppImage | 必须由宿主提供（打不进） |
|---|---|
| Qt6 全套库 + 平台插件（`libqxcb.so` / `libqwayland-*.so`） | **glibc**（不可重定位） |
| `libstdc++` / `libgcc_s` | **GPU 驱动**：`libGL` / `libEGL` / `libdrm`（必须用宿主的） |
| FFmpeg / OpenCV / onnxruntime / 图像编解码库 | **xdg-desktop-portal**（Wayland 采集入口） |
| 自研 Rust 静态库、`.qm` 翻译、字体 | **PipeWire / PulseAudio 守护进程** |
| 应用本体与资源 | D-Bus session bus、AT-SPI、polkit、CUPS、X11/Wayland 合成器 |

> ⚠️ 关键推论：**AppImage 解决不了本方案的主要工作量（L2 平台后端）。**
> 屏幕采集（portal + PipeWire）、音频、全局输入钩子、UI 元素拾取全部依赖宿主服务与内核接口，
> 打包行为对此毫无帮助——**第 8 章的 P3 / P4 工作量一项都省不掉。**

### A.3 核心约束：glibc 基线（决定性因素）

Qt 官方数据（[Qt for Linux](https://doc-snapshots.qt.io/qt6-dev//linux.html)）：

| Qt 版本 | 官方预编译包最低 glibc |
|---|---|
| Qt 6.8+ | glibc **2.28** |
| Qt 6.10+ | glibc **2.34** |
| Qt 6.11+ | 官方在线安装包基于 Ubuntu 24.04（glibc **2.39**） |

目标系统基线（已核实）：

| 目标系统 | 基础发行版 | glibc | 内核 | Qt 6.12 预编译包 | 说明 |
|---|---|---|---|---|---|
| **UOS 20（V20 1070）** | Debian 10 buster | **2.28** | 4.19 | **✗ 不可用** | 会报 `GLIBC_2.34 not found` |
| Deepin 20 | Debian 10 | 2.28 | 4.19 | **✗ 不可用** | 同上 |
| Deepin 23 | 自研 | 2.38 | 6.6 | ✓ 可用 | |
| UOS 25 | 较新 | 需实测 | 较新 | 大概率可用 | 以实机 `ldd --version` 为准 |

**本工程锁定 Qt 6.12.0**，其官方预编译基线为 glibc ≥ 2.34（实测构建于 2.39），
而 **UOS 20 只有 glibc 2.28** → 预编译 Qt6 AppImage 在 UOS 20 上**直接跑不起来**。

> 注意：Qt 官方称"**从源码构建则不受 glibc 版本限制**"——但真实限制会转移到*构建主机*：
> 在 glibc 2.38 / 2.39 的机器上编出的 Qt 会引用高版本符号，同样跑不了 2.28。
> 必须**在 glibc ≤ 2.28 的环境内构建**（UOS 20 / Debian 10 容器，或使用老 sysroot）。

### A.4 三条可行路线对比

| | 路线 A：AppImage + Qt6 预编译 | 路线 B：AppImage + Qt6 自建于 2.28 环境 | 路线 C：维持 Qt5 路线（原方案） |
|---|---|---|---|
| 覆盖 UOS 20（glibc 2.28） | ✗ | ✓（需实测验证） | ✓ |
| 覆盖 Deepin 23 / UOS 25 | ✓ | ✓ | ✓ |
| 是否保留 Qt6 | ✓ | ✓ | ✗（须降级） |
| 关键难点 | 无（但 UOS 20 用不了） | Qt 6.12 需 CMake ≥ 3.22 + 较新编译器，而 Debian 10 仅 GCC 8.3 → 需"老 glibc + 新编译器"组合（老 sysroot / 容器） | Qt5 兼容层 + RHI 重写 |
| 主要风险 | glibc 不达标 | 构建链复杂；符号版本需实测 | 代码改写量大 |

> **路线 B 的隐藏地雷**：在 glibc ≥ 2.38 且 GCC 14 下构建会引入 `__isoc23_*` 符号
> （如 `__isoc23_strtol`），该符号在旧 glibc 上不存在。必须**使用旧版 glibc 头文件**编译，
> 而不是只换一个旧 linker。

### A.5 对本工程工作量的连锁影响

若最终选择「AppImage + Qt6」，原方案中两大战线**整体消失**：

| 原方案工作项 | AppImage + Qt6 后 | 变化 |
|---|---|---|
| W1 Qt5 兼容层 + CMake 双版本（P1） | **取消** | ↓↓↓ |
| W2 `snow_image_viewer` RHI 重写 | **取消**（Qt6 下 `QRhi` 原生可用） | ↓↓↓ |
| 第 3 章全部 Qt6→Qt5 API 改写清单 | **取消** | ↓↓↓ |
| W3 Rust 平台后端（采集 / 光标 / 音频 / 输入） | 保留 | — |
| W4 C++ `platform/linux/*` | 保留 | — |
| W5 / W6 / W8 / W9 / W10 | 保留 | — |
| AppImage 打包 + glibc 基线控制 | **新增** | ↑ |

**净效果**：工程总量显著下降，但**前置条件变为"目标机 glibc ≥ 构建基线"**，
且**信创合规风险上升**（见 A.7）。

### A.6 AppImage 落地注意事项（与本工程相关）

1. **FUSE2 依赖**：AppImage 需要 `libfuse2`（**不是** fuse3）。UOS 可能未预装，
   需 `sudo apt install libfuse2`；否则用 `--appimage-extract-and-run` 兜底（体验下降，需解压约 1GB）。
2. **切勿打包 `libGL` / `libEGL` / `libdrm`**：`linuxdeploy` 默认排除这些是正确行为——
   打包会导致 GPU 驱动失配、渲染异常。
3. **Qt 平台插件链必须完整**：`platforms/libqxcb.so` 依赖一长串 `libxcb-*`、`libxkbcommon`
   与 `xcbglintegrations/*`，建议用 `linuxdeploy-plugin-qt` 自动收集，并逐个 `ldd` 核对。
4. **Wayland 会话**：需同时打包 `libqwayland-*.so` 与 `libwayland-client`；
   但**采集仍须走宿主 portal**，打包无济于事。
5. **桌面集成缺口**：AppImage 不会自动创建菜单项、托盘、开机自启。本工程的"开机自启"会写
   `~/.config/autostart/*.desktop` 并指向 AppImage **绝对路径**——用户一旦移动文件即失效，
   需在 UI 中显式提示（避免"设了自启却没生效"的误导）。
6. **单实例**：`QLocalServer`（`snow_shot/src/app/singleinstancecoordinator.cpp`）在 AppImage 下正常，无需改动。
7. **提权**：`pkexec` 走宿主 polkit，正常工作。
8. **更新**：AppImage 不受系统包管理器管理，需自建更新通道（`AppImageUpdate` / zsync 或应用内自更新）。
9. **arm64**：需 aarch64 版 `appimagetool` 与 arm64 `libfuse2`；同样准备 `--appimage-extract-and-run` 兜底。
10. **静态 Qt 变体**：本工程已支持 `SNOW_APPS_QT_STATIC`。`snow_shot` 为 **GPL-3.0**，
    静态链接 Qt 在许可上成立，且能规避插件路径问题——可作为路线 B 的简化形态。

### A.7 合规提醒（重要）

在**信创采购 / 等保 / 国产化适配认证**场景中，AppImage 通常**不被认可**：
它不在信创软件目录内，且认证审查一般要求使用**系统提供的基础库**而非自带副本。
若本项目的 Qt5 约束来自认证要求，则 **AppImage 打包 Qt6 极可能通不过评审**，
必须维持 Qt5 路线（折中方案：采用 `deepin-shared-libs` 提供的 Qt 5.15.10）。

### A.8 基线验证方法（动手前必做）

```bash
# 1) 确认目标机 glibc
ldd --version | head -1

# 2) 解包后检查打包库引用的最高 GLIBC 符号版本（必须 <= 目标机）
./SnowShot.AppImage --appimage-extract
readelf -V squashfs-root/usr/lib/libQt6Core.so.6 \
  | grep -o 'GLIBC_[0-9.]*' | sort -Vu | tail -3
# 期望 <= GLIBC_2.28（UOS 20）

# 3) 复查全部打包 .so
find squashfs-root -name '*.so*' -exec sh -c \
  'readelf -V "$1" 2>/dev/null | grep -o "GLIBC_[0-9.]*"' _ {} \; | sort -Vu | tail -3

# 4) 确认未引入 __isoc23_* 符号
nm -D squashfs-root/usr/lib/libQt6Core.so.6 | grep isoc23 || echo "OK: no __isoc23"
```

---

*本文档基于对 `snow-apps` 仓库（`SNOW_SHOT_VERSION 1.2.8`）的静态分析编写，所有文件路径与行号均对应当前快照。*

## 附录 B：arm64 单元测试失败基线（run 41264674368，31c214fe）

x64 作业在 180 分钟上限处被取消，arm64 作业跑完全部 832 个用例：**699 通过 / 133 失败**，
整个 Unit tests 步骤（重新配置 + 编译 289 个测试 TU + ctest）在 arm64 上装得进 180 分钟。
也就是说 Linux 上「编译 + 打包 + 跑测试」全链路已经打通，
剩下的全是断言差异，而且几乎没有成簇的单一根因——133 个失败落在 108 个互不相同的首行断言上。

| 失败数 | 首条非噪声输出 | 代表用例 |
| --- | --- | --- |
| 11 | `(无输出)` | adqt-context-menu-lifecycle-tests, adqt-value-widget-lifetime-tests, snow-shot-custom-ai-models-tests … |
| 3 | `OCR cached/direct mismatch: mode=0 dpr=1.5 pixels=12 max_channel_difference=67 first=88,69 cached=#f` | snow-shot-screenshot-canvas-renderer-tests, snow-shot-ocr-canvas-renderer-tests, snow-shot-ocr-line-box-rendering-tests |
| 3 | `Traceback (most recent call last):` | snow-shot-mcp-capabilities-tests, snow-shot-mcp-fixture-tests, snow-shot-mcp-application-fixture-tests |
| 2 | `font database changes must invalidate worker layouts on their thread` | snow-canvas-text-draft-tests, snow-canvas-text-render-cache-tests |
| 2 | `offscreen serial scenes must preserve the incoming font and disabled text antialiasing for custom ba` | snow-canvas-filter-render-tests, snow-canvas-serial-number-font-tests |
| 2 | `hovering More opens the shared widget menu` | snow-shot-pinned-window-management-page-tests, snow-shot-collage-pinned-selection-tests |
| 2 | `managed decoding must preserve the reader's fallback scaling result` | snow-shot-main-window-skin-codec-tests, snow-shot-managed-image-reader-tests |
| 2 | `initial selector refresh failed` | snow-shot-window-element-api-tests, snow-shot-selector-phased-tests |
| 2 | `pinned text bounds match the exact client extent at the current DPR and zoom` | snow-shot-pinned-text-wrap-tests, snow-shot-pinned-text-wrap-fractional-tests |
| 2 | `quick save must create the configured directory and use format/name settings` | snow-shot-export-artifact-tests, snow-shot-quick-save-export-tests |
| 2 | `Move options must group region actions at the far left, before capture actions and hide` | snow-shot-move-region-toolbar-tests, snow-shot-screenshot-tool-palette-tests |
| 2 | `the screenshot move tool must retain Edit selection and its screenshot shortcut` | snow-shot-screenshot-action-shortcut-display-tests, snow-shot-screenshot-toolbar-i18n-tests |
| 2 | `screenshot settings body fits its content without vertical blank space` | snow-shot-translation-page-tests, snow-shot-screenshot-translation-settings-dialog-tests |
| 2 | `an OCR stitching source accounts for its raster before joining the import queue` | snow-shot-pinned-stitching-snapshot-tests, snow-shot-pinned-multi-selection-tests |
| 1 | `Menu must publish populated rows before returning from setModel` | adqt-widget-diagnostics-tests |
| 1 | `unpadded text and link buttons must fit their content` | adqt-button-lifetime-tests |
| 1 | `********* Start testing of ButtonSignalLifetimeTests *********` | adqt-button-signal-lifetime-tests |
| 1 | `********* Start testing of SelectTest *********` | adqt-select-tests |
| 1 | `simulated primary display must use 150 percent scaling` | adqt-popup-hover-geometry-tests |
| 1 | `the bundled Ant Design catalog should translate pagination text` | adqt-locale-manager-tests |
| 1 | `tablet samples preserve subpixel positions, time and normalized actual pressure` | snow-canvas-tablet-input-tests |
| 1 | `gradient opacity must increase from 16% to 42% and multiply every alpha` | snow-canvas-magnifier-render-tests |
| 1 | `sibling filters cannot consume or propagate each other's changed output` | snow-canvas-magnifier-interaction-tests |
| 1 | `bounded color conversion must exactly match Qt` | snow-canvas-image-tests |
| 1 | `serial rendering restores the caller's font state` | snow-canvas-serial-number-render-tests |
| 1 | `sparse dpr=1.25 rotation=-0.47 zoom=1.37 selected=1` | snow-canvas-text-overlay-tests |
| 1 | `long image tiles must remain on one page and preserve current image order` | snow-shot-collage-pdf-export-tests |
| 1 | `captured pixels own their admission without copying or losing image metadata` | snow-shot-collage-document-tests |
| 1 | `Storage initialization: 2 ms` | snow-shot-collage-window-tests |
| 1 | `Storage initialization: 1 ms` | snow-shot-collage-live-pinned-import-tests |
| 1 | `an empty-state drop must request import and clear the highlight` | snow-shot-image-split-editor-tests |
| 1 | `Image split filename policy tests passed` | snow-shot-image-split-export-tests |
| 1 | `guarded image must retain the protected source storage` | snow-shot-image-split-preview-tests |
| 1 | `incremental overlays must match fresh renders across DPI, languages, and clipped rulers` | snow-shot-image-split-canvas-tests |
| 1 | `FFF` | snow-shot-recording-build-profiles-tests |
| 1 | `libpng error: PNG output write failed.` | snow-shot-image-codec-backend-smoke-tests |
| 1 | `parallel requests start` | snow-shot-text-translation-tests |
| 1 | `a silent Retina warp must deliver fractional hover movement to the child` | snow-shot-physical-cursor-tests |
| 1 | `an existing link error retranslates and preserves its destination` | snow-shot-about-page-tests |
| 1 | `administrator controls must exist in the settings catalog` | snow-shot-settings-catalog-tests |
| 1 | `Checking interface.theme-primary-color` | snow-shot-settings-color-commit-tests |
| 1 | `System default is visibly selected when the application font is unset` | snow-shot-settings-select-tests |
| 1 | `related buttons size to their text without built-in padding` | snow-shot-settings-navigation-tests |
| 1 | `only settings that actually differ must announce changes` | snow-shot-configuration-archive-tests |
| 1 | `selector service geometry refresh failed` | snow-shot-selector-lifecycle-tests |
| 1 | `a disabled tray must not replace the last balloon with an update notice` | snow-shot-system-tray-controller-tests |
| 1 | `disabled legacy tray notifications cannot open files` | snow-shot-recording-export-tray-tests |
| 1 | `a new recording session must save the new physical position` | snow-shot-shortcut-key-row-tests |
| 1 | `direct capture lost the full desktop snapshot` | snow-shot-direct-capture-native-tests |
| 1 | `cursor patch must map through image pixels into desktop canvas points` | snow-shot-screenshot-cursor-tests |
| 1 | `crop guide must cross the selection center at one physical pixel` | snow-shot-scrolling-overlay-tests |
| 1 | `blit-format mean_ms argb32=1.12802 rgb32=0.0925955` | snow-shot-overlay-frame-presenter-tests |
| 1 | `each independent file window must retain its centered geometry` | snow-shot-file-pin-presentation-tests |
| 1 | `stationary controls must apply opacity immediately` | snow-shot-pinned-click-through-tests |
| 1 | `restored always-on-top fixture needs the pinned menu` | snow-shot-pinned-always-on-top-tests |
| 1 | `thumbnail transitions must preserve the current mouse anchor` | snow-shot-pinned-double-click-tests |
| 1 | `Text wheel input should increase pinned font size` | snow-shot-pinned-toolbar-parity-tests |
| 1 | `close merged translation pin` | snow-shot-pinned-translation-tests |
| 1 | `an advertised Save As format could not be encoded` | snow-shot-screenshot-image-file-service-tests |
| 1 | `the first presentation should consume the prepared shell` | snow-shot-pinned-window-pool-tests |
| 1 | `persisted recognition must keep the Qt 6.11 binary format` | snow-shot-pinned-restored-ocr-tests |
| 1 | `recognition setup must restore the overlay without changing or re-requesting OCR` | snow-shot-pinned-early-ocr-snapshot-tests |
| 1 | `existing pinned toolbar must reload its custom layout` | snow-shot-pinned-toolbar-layout-tests |
| 1 | `the flip or half-turn must persist without changing the window size` | snow-shot-pinned-transformed-ocr-tests |
| 1 | `enabling window buttons must immediately reveal both on a hovered pin` | snow-shot-pinned-pointer-presence-offscreen-tests |
| 1 | `final encoding dimensions differ from form` | snow-shot-export-dialog-workflow-tests |
| 1 | `bounded codec preview encoding failed` | snow-shot-export-dialog-pipeline-tests |
| 1 | `the save dialog must remain above its toolbar after a queued raise` | snow-shot-export-dialog-placement-tests |
| 1 | `upload succeeds once` | snow-shot-cloud-upload-tests |
| 1 | `changing cloud destination synchronizes selection without recreating rows` | snow-shot-cloud-upload-settings-tests |
| 1 | `invalid Excel workbook package` | snow-shot-pinned-recognition-save-tests |
| 1 | `failed file copy leaves clipboard untouched` | snow-shot-pinned-shared-image-export-tests |
| 1 | `automatic save failure preserves the successful pin` | snow-shot-pin-execution-autosave-tests |
| 1 | `quick-save annotation was not committed` | snow-shot-pinned-quick-save-tests |
| 1 | `legacy default saves must upgrade to an idempotent quick-save stack` | snow-shot-quick-save-toolbar-tests |
| 1 | `Arrow should remain a direct button when Line is unavailable` | snow-shot-screenshot-tool-palette-distance-tests |
| 1 | `Table and QR recognition should occupy one main toolbar slot` | snow-shot-screenshot-action-toolbar-layout-tests |
| 1 | `default drawing group should expose all configured tools` | snow-shot-screenshot-shortcut-tooltip-tests |
| 1 | `storage compatibility coverage should follow the engine's current template version` | snow-shot-styled-template-storage-tests |
| 1 | `closing a ratio popup must restore the previous screenshot keyboard owner` | snow-shot-screenshot-selection-toolbar-hover-tests |
| 1 | `save test template` | snow-shot-global-canvas-tests |
| 1 | `clicking active Select must return to Export Settings` | snow-shot-screen-recording-shortcut-tests |
| 1 | `repeating a recording tool shortcut must return to Export Settings like a button click` | snow-shot-recording-history-shortcut-tests |
| 1 | `initial capture must exclude the interface` | snow-shot-scrolling-capture-settings-tests |
| 1 | `toolbar movement must forward the event's global coordinates` | snow-shot-toolbar-guide-hover-tests |
| 1 | `the screenshot window should live-reload every independent action slot` | snow-shot-floating-toolbar-action-layout-tests |
| 1 | `screenshot reset should be enabled without selection` | snow-shot-screenshot-selection-reset-command-tests |
| 1 | `unavailable native exclusion keeps drawing popup hidden; capture-on shows it` | snow-shot-recording-capture-exclusion-tests |
| 1 | `OCR integration must execute the selected runtime` | snow-shot-ocr-cpu-recognition-service-tests |
| 1 | `the managed OCR runtime must be staged` | snow-shot-ocr-managed-runtime-tests |
| 1 | `save failed: avif: output_failed` | snow-shot-mcp-document-tests |
| 1 | `local API test server timed out waiting for a request` | snow-shot-api-client-tests |
| 1 | `all resize handles must preserve aspect ratio` | snow-shot-pinned-controlled-interaction-tests |
| 1 | `passive publication correction must restore geometry without discarding pixels` | snow-shot-pinned-passive-publication-tests |
| 1 | `live pin pixels, alpha and source mapping must match sRGB conversion` | snow-shot-pinned-color-space-tests |
| 1 | `pinned drawing editor must serialize and save the selected canvas elements` | snow-shot-pinned-template-dialog-tests |
| 1 | `failed export must not start native drag` | snow-shot-pinned-drag-export-tests |
| 1 | `rapid source switching rejects a stale preview before retirement completes` | snow-shot-recording-audio-controller-tests |
| 1 | `dragging out of a hover-opened editor preserves gain editing` | snow-shot-recording-audio-gain-popover-tests |
| 1 | `a narrow desktop needs overflow without allocating its menu` | snow-shot-floating-toolbar-menu-lifecycle-tests |
| 1 | `the mixed-DPI fixture screen is missing` | snow-shot-original-image-preview-mixed-dpi-tests |
| 1 | `hidden recording hotkeys retain their Export Settings toggle` | snow-shot-recording-hidden-tool-shortcut-tests |
| 1 | `........FF....` | snow-shot-release-optimization-policy-tests |
| 1 | `empty document fast path changed projection, alpha, or color metadata` | snow-shot-print-export-snapshot-tests |
| 1 | `published backing store clears stale shadow pixels on every toggle` | snow-shot-pinned-show-shadow-tests |
| 1 | `dense tiled union and hole dpr=1.000000 transform=1: maximum premultiplied channel difference=34 fir` | snow-shot-region-shadow-renderer-tests |
| 1 | `tile mismatch dpr=1.3 at 642,408 actual=#8594a7 expected=#8292a6` | snow-shot-selection-background-preview-tests |
| 1 | `batched pixels must retain source samples and exact coverage` | snow-shot-layout-compaction-tests |

按性质归类的处理方向：

1. **平台门控（最高性价比）**：`administrator controls must exist in the settings catalog` 一类断言在
   Linux 上没有对应页面，属于用例缺 `Q_OS_WIN` 条件，不是产品缺陷。
2. **字度量**：`unpadded text and link buttons must fit their content`、`pinned text bounds`、
   `Move options must group…` 都建立在具体字体度量上，Qt 5.15.13 arm64 的 fallback 字体与 Qt6 不同，
   需要在测试里固定测试字体，而不是照抄 Qt6 的像素期望。
3. **色彩/像素管线**：`OCR cached/direct mismatch`、`bounded color conversion must exactly match Qt`、
   `tile mismatch dpr=1.3`、`maximum premultiplied channel difference=34` 是 Qt5 色彩转换与dithering
   的实现差异，需要按「两条产品路径互相一致」而不是「与 Qt6 像素相等」来断言。
4. **屏幕夹具**：`the mixed-DPI fixture screen is missing`、`simulated primary display must use 150 percent scaling`
   依赖 QPA 提供的多屏/DPR 能力，offscreen 插件给不出，需要在 Linux 上换夹具实现。
5. **崩溃（13 个 SEGFAULT + 1 个 SIGBUS）**：故障地址集中在 Qt 5 内部同一条 abort 路径上，见 `.workbuddy/qt5-port/dump-on-crash.ps1`
   与本机复现记录；`adqt-button-signal-lifetime-tests` 在 arm64 上是 SIGBUS（signal 7）而非 AV，需单独看。

复跑方法（任一 Linux 机器）：

```bash
cmake --preset snow-shot-linux-x64-release -DSNOW_APPS_BUILD_TESTS=ON
cmake --build --preset snow-shot-linux-x64-release --parallel 4 -- -k 0
xvfb-run -a --server-args="-screen 0 1280x1024x24" \
  ctest --test-dir build/snow-shot-linux-x64-release --output-on-failure
```



## 附录 B.1：run 41383823090 基线——用双架构交集把失败切成两堆

上一次基线（附录 B，133 失败）里 x64 作业被 180 分钟上限取消，只能看 arm64 一家。
这次两个架构都跑完了，于是第一次能做**集合运算**，而不是逐个读断言：

| | 失败数 |
| --- | --- |
| x64（这次跑完了，03:34 开工、06:42 出结果；上一次在 180 分钟上限被取消） | 109 / 832 |
| arm64（03:34 → 05:41） | 113 / 832 |
| **交集**（两边都红） | **109** |
| 只在 arm64 红 | 4（`canvas-text-overlay`、`image-split-export`、`screenshot-cursor`、`scrolling-overlay`） |
| 只在 x64 红 | 0 |

**统计口径的坑**：不能 grep 行内的 `***Failed` 来数——x64 那样只数到 96 条，少 12 条，
而 ctest 汇总的 `The following tests FAILED` 段是 109 条。下载日志用 `--tail` 截断开头时，
前段测试的 `***Failed` 行会一起丢掉，所以**唯一可信的口径是末尾那段汇总清单**
（`grep -A140 "The following tests FAILED"` 再取 `^\s+[0-9]+ - name`）。

这个切法是这次最有用的结论：**109 条与架构无关**，它们只说明"Linux 上的行为和 Windows 不一样"，
所以任何"再去 x64 上试一次"的排查都是浪费；剩下 4 条才是 aarch64 相关，而且四条的断言形态
是同一类——**差一个最低位 / 一个物理像素**：

```
pixel (4, 0) is #8aaa022b, expected #8aaa022a      ← 蓝色通道差 1
crop guide must cross the selection center at one physical pixel
cursor patch must map through image pixels into desktop canvas points
sparse dpr=1.25 rotation=-0.47 zoom=1.37 selected=1 / sparse repaints must preserve pixels outside the exposed region
```

这是 NEON 与 SSE 在 `round`/`toInt` 上的取整方向差异，不是产品逻辑错。要在 aarch64 上绿，
得让断言按"两条产品路径互相一致"或"允许 ±1"来写，而不是钉死某个值——和附录 B 第 3 类同一个处置。

### 这轮在 CI 上变绿的条目，以及一条"修了还红"的

| ctest 条目 | 上一次 | 这一次 |
| --- | --- | --- |
| `snow-shot-global-canvas-tests`（#579） | `***Failed`（`save test template`） | **Passed** |
| `snow-shot-global-canvas-color-sampling-tests`（#581） | `***SegmentFault` | **Passed** |
| `snow-shot-pinned-template-dialog-tests`（#748） | `***Failed` | **Passed** |
| `snow-shot-styled-template-storage-tests`（#560） | `***Failed` | 仍红 → 本次已修（见下） |

`#560` 修完仍红的原因值得记一笔：它红在**测试自身的漂移守卫**上——

```
storage compatibility coverage should follow the engine's current template version
```

这条断言把"引擎写出的版本号"钉成 `6`，同时接受 `1..6`、把 `7` 列为"未来格式"。
c4c8108c 把 C++ 上限提到 7 之后，这三处字面量就全错了。**光改 schema 侧会永远红，
光改第一条断言又会让它接受一个校验器其实拒绝的版本**，所以三处必须从同一个常量派生
（`engineTemplateVersion`，对应 Rust 的 `DRAW_TEMPLATE_SCHEMA_VERSION`）。
反向验证：常量退回 6 时恰好红在这一条，改回 7 后
`--styled-template-storage-only -platform offscreen` 绿。

### 失败总数 133 → 113

比上一轮少 20 条。分族看（同一口径，ctest 汇总清单）：

| 族 | 附录 B（run 41264674368） | 本次（run 41383823090） |
| --- | --- | --- |
| `adqt-*` | 11 | **5**（button-lifetime、button-signal-lifetime、popup-hover-geometry、select、widget-diagnostics） |
| `snow-shot-mcp-*` | 3 | **4**（多了 `mcp-application-tests`） |
| `snow-canvas-*` | 10 | 10（同 10 条，一条不多一条不少） |

所以不是"上一轮的失败都是连带现象"。`adqt-*` 少了 6 条、`mcp-*` 还多了 1 条，说明这批
（附录 B 里首行是 `(无输出)` 或 `Traceback` 的那些）**状态不稳定，会随运行而变**：
它们失败时连断言都不打印，更像是进程级问题（崩溃、超时、Python 夹具没起来）而不是产品断言。
处置这类用例的正确顺序是先让它稳定复现，而不是去猜断言；逐条"修" 133 条里每一条的性价比，
远低于先把交集 109 条按附录 B 的 5 类批量处置。

## 附录 C：Qt 5 崩溃栈的符号化与三条已确认的框架差异

Linux 上 13 个 `***Exception: SegFault` 之前一直是黑箱：Qt 5 开源版不带 PDB，
`Qt5Widgetsd.dll+0x52F9A` 只能被解析到最近的导出符号，于是十几条完全不同的栈都被印成
`QStyleAnimation::frameRate+0x…`。这个假名字让排查卡了两轮。

**装一次 PDB 就解决了。** Qt 官方把 debug info 单独发布在
`download.qt.io/online/qtsdkrepository/windows_x86/desktop/qt5_5152/` 下，
`qt.qt5.5152.debug_info.win64_msvc2019_64/…qtbase…-debug-symbols.7z` 是 90 MB，
解压到 `.tools/qt5-symbols/extracted/5.15.2/msvc2019_64/bin` 后把该目录挂到
`_NT_SYMBOL_PATH`（必须在 `SymInitialize` 之前），
`.workbuddy/qt5-port/stack-any-dump.py` 就会自动使用它。同一个 dump 的同一段栈，
符号化前后相差一个数量级的信息量。

### C.1 `QAbstractButton` 在 emit 之后还会写自己的私有状态

Qt 5 的 `QAbstractButtonPrivate::click()` 在发完 pressed/released/toggled/clicked **之后**
还会调 `refresh()`；`QAbstractButton::mouseReleaseEvent()` 在 `click()` 返回后还会写
`d->repeat`。因此「观察者在按钮的信号里 `delete` 按钮」在 Qt 5 上必然访问已释放的
`QAbstractButtonPrivate`。Qt 6 删掉了这两条语句，所以只有 Qt 5 的跑到这里崩。

`adqt-value-widget-lifetime-tests` 的两个鼠标/键盘用例因此按 `QT_VERSION >= 6` 门控。
同一文件里 `AdTagGroup::applySelectionToTags()` 仍然可以在 Qt 5 上安全地允许观察者删除整组：
把状态改动放进 `QSignalBlocker`，再用 `QMetaObject::invokeMethod` 补发 `toggled`，
此时按钮已经没有任何后续工作，删除就是安全的。

### C.2 `QMenu` 的 sloppy state 无法吸收「先重开、后销毁旧 shell」

`AdContextMenu` 的懒加载子菜单在每次重开时换一个新 shell。若旧 shell 的 `deleteLater()`
等到**下一轮 popup 之后**才执行，Qt 5 会在 `setActiveAction()` 里沿着 sloppy state 把那个已销毁
的菜单再 `QMenu::popup()` 一次：

```
setActiveAction -> setCurrentAction -> popupAction -> internalDelayedPopup
  -> QMenuSloppyState::setSubMenuPopup -> QMenu::popup -> QMenuPrivate::get -> d_func   [fault]
```

把销毁放在每轮末尾（`persistentParentsRestoreLazySubmenus(true)`）就正常，所以这是顺序问题。
产品侧试过两种修法都不改变故障地址：在 `LazySubMenu::installShell()` 里同步删掉旧 shell、
以及销毁前先 `setParent(nullptr)` —— 失效的引用在 Qt 的 sloppy state 里，不在控件自己手上。
该顺序用例按 `QT_VERSION >= 6` 门控，并在 stderr 上明说。

### C.3 静态库里的 Qt 资源必须有人引用才存在

`rcc` 生成的翻译单元只有 `qInitResources_<target>_translations` 及其清理函数，
注册动作依赖文件内的静态初始化器；而静态库成员只有在有符号被引用时才会被链接器拉进来。
`libant_design_qt.a` 里没有任何东西引用它，于是整个 `.qm` 资源被丢弃，
`catalogPathFor()` 列出一个空目录，`refreshTranslator()` 静默不装任何 translator，
界面全量退回英文且不报错。修法是从 `catalogPathFor()` 里显式点名该入口。

注意 `Q_INIT_RESOURCE` 必须写在**全局命名空间**的具名函数里：宏展开只是 `extern int qInitResources_x()`，
这条声明会落进最内层封闭命名空间，在 `adqt::locale` 或匿名命名空间里都会解析到不存在的符号
（MSVC 直接把修饰名打出来了）。


### C.4 崩溃栈现在能给出「源文件:行号」

`SymFromAddr` 在本机仍然静默失败，但 **`SymGetLineFromAddr64` 能用**：Qt 官方 debug-symbols
的 PDB 和我们自己的 PDB 都带行信息。`stack-any-dump.py` 已经接上，于是每条栈帧长这样：

```
  [0118] 0x00007FFFBD365496  QAction::activate+0x36  (qaction.cpp:1149)
  [0198] 0x00007FF66841B1F1  pageActionsAndLifecycle+0x1DA1  (standalone_translation_tests.cpp:425)
```

代价是十几行 ctypes：`IMAGEHLP_LINE64` 的 `SizeOfStruct` 必须先填好，否则 dbghelp 同样
「返回 FALSE 且 GetLastError 为 0」。有了行号，"崩在 QAction::trigger 里" 就直接落到
**测试的第 425 行**，不必再猜是哪个调用点。

### C.5 弹出菜单持有自己的动作：懒创建 + 隐藏即销毁

`ActionPopupMenu` 的工厂每次 open 都造一个新菜单，菜单 `setDeleteOnHide()`，动作是菜单的
子对象。于是：

- 菜单没打开过 ⇒ `findChild<QAction*>("translationCopy")` 返回 **nullptr**；
- 菜单一旦 hide（复制、点别处、模态关闭）⇒ 动作**随之销毁**，先前抓到的 `QAction*`
  变成悬垂指针。

Qt5 上这两种情况都是 `QAction::activate` 里 `QPointer<QObject>` → `getAndRef` 的 C0000005
（`qaction.cpp:1149`）。`standalone_translation_tests` 两个用例都踩在这里，修法：先
`click()` 浮动按钮把菜单打开、再取动作；复制之后菜单已销毁，取"复制并关闭"必须**重新
打开一次**。`snow-shot-translation-page-streaming-tests` 更彻底——页面从未 show，菜单
根本打不开，只能改用结果区常驻的复制按钮（同一个 `copyResult()`）。

### C.6 `AdPopover::takeContentWidget()` 曾经"拿不走"

弹层视图（`PopoverContentView`）自己也存了一份 widget 指针，并且**每次 rebuild 都会把它
re-parent 回自己的 host**。`takeContentWidget()` 只清了 popover 这边那份，`setParent(nullptr)`
之后又立刻 `syncPopupContent()` ⇒ 视图把 widget 原样塞回弹层。调用方以为拿走了，其实还挂在
弹层里（`AdPopconfirm` 只好再 `setParent(nullptr)` 一遍兜底）。

调用方是析构函数时这会致命：`~AdPopconfirmPrivate` 取回面板随即 `delete`，而 Qt5 的
`QWidget::setParent → QWidgetPrivate::reparentFocusWidgets` 在"半销毁"的 widget 上崩掉
（`snow-shot-translation-settings-tests` 的 C0000005，地址 `Qt5Widgetsd+0x52F9A`）。
修法是先让视图忘掉它（`popupContentView_->setContentWidget(nullptr)`），再摘走。

### C.7 断言失败不能走静态析构（一条 SELinux 式的整类 SEGFAULT）

所有 `require()` 助手都是「`std::cerr << message` 然后 `std::exit(EXIT_FAILURE)`」。`exit()`
会**在测试刚造出来的窗口还活着的时候**跑静态析构：`adqt` 的几个单例（`TimingHub`、图标缓存
的 `QMutex`）此时已经没了，于是一个控件收到 `QEvent::Hide` 走到 `cancelTimingTask()` /
`renderIconPixmap()`，就在已销毁的互斥量上取址崩溃。

后果不是"多一个崩溃"，而是**判据不可见**：CI 记录的是 SIGSEGV 而不是那条断言消息，
十来个测试因此根本没法定位。改成 `std::_Exit(EXIT_FAILURE)`（跳过析构即可，`std::cerr`
是 unit-buffered，消息不会丢）后，同一批用例立刻变成老老实实的失败退出码 + 首行断言。

结论：**测试的失败路径必须无副作用**。否则"崩溃"这个信号会被大量误报淹没。

### C.8 `QApplication::widgetAt()` 在 Wayland 与 Qt5 offscreen 上没有答案

`pointerHoverPositionWithin()` 用 `widgetAt(全局点)` 做"是不是被别的窗口盖住了"的命中测试，
拿不到就一律拒绝。但 Wayland 客户端**根本无法查询全局指针**，Qt5 的 offscreen 插件同样返回空，
于是这两个平台上"悬停展开菜单"从来没生效过（`translation_page_tests` 的
`trigger reveals actions` 就是这么红了一整天）。

修法：只有平台**确实指出**了一个不属于接收者的控件时才拒绝；没有答案时，事件本身携带的
位置就是唯一证据，而它已经通过了矩形与可见子树检查。这条同时修好了 UOS/Wayland 上的产品行为，
不只是测试。

### C.9 主题信号打到一个"没有画布"的窗口上（`nullptr->update()`）

`snow-shot-pinned-hide-to-top-tests` 的崩溃花了一轮才定性：RIP 落在
`QWidget::rect()`（`qwidget.h:860`），访问地址是 **0x28**——也就是从空指针上读成员。
栈上明明是 `m_canvas->update()` 的那一行，但真正为空的不是 `this`：

`destroyCanvas()` 会把 `m_canvas` 置空（内容重新物化前一直是空），而钉图窗口仍然挂在
`themeChanged` 上，主题一变就被要求重绘，于是 `nullptr->update()` 在 `QWidget::update()`
里读 `d->data->crect` 直接崩。**空指针上调非虚成员函数是 UB，这条和 Qt5 无关，Qt6 一样会崩**，
只是要"主题在窗口没有画布的那一刻正好变了"才触发。

两个教训：
1. 报错行号（PDB 行信息）能指到 `update()` 这一句，但**指不到"哪个指针是空的"**。
   `dump-on-crash.ps1` 现在会把 `ExceptionInformation[1]` 一起打出来——`0x28` 这种小地址
   一看就知道是空成员访问，而不是野指针。
2. 主题/字体这类全局信号的回调里，成员指针一律先判空。`m_destroying` 这种"正在销毁"标志
   也要放在最前面返回，否则析构途中的重入会把半销毁的控件再画一遍。

### C.10 offscreen 平台不会在隐藏时撤销激活

`window.hide()` 之后 `isVisible()` 立刻为假，但 offscreen 插件**不会**像真实窗口管理器那样
再发一次"取消激活"，所以 `isActiveWindow()` 仍是真。这类断言在 offscreen 上不是产品缺陷，
而是平台根本没有这个答案（和 C.8 的 `widgetAt()` 同一类）。

判据写法：可见性要求保留给所有平台，激活要求只留给真实平台（本仓库里就是
`--hide-to-top-native-only -platform windows` 那个变体）。

### C.11 已修：`snow-shot-global-canvas-color-sampling-tests` 在 offscreen 上爆栈

状态：**已修**（`0d52a58d`）。根因是 `applyPlacementScreen()` 缺重入闸，见下面「C.11 结论」。
本节保留当时的判据、排除过程与工具，因为里面有一半假设是被**证伪**的，值得留档。

修前的现象与判据：

| 平台 | 结果 |
| --- | --- |
| `-platform windows`（本机） | 通过，EXIT=0 |
| `-platform offscreen`（本机） | `0xC00000FD` 栈溢出，退出码 127，崩前刷 661 次 `does not support raise()` |
| Linux arm64 CI（xcb + `-platform offscreen`） | `***Exception: SegFault`，日志里同样是 storage 初始化后紧跟一条 `does not support raise()` |

也就是说：**这不是 Windows-offscreen 的本地怪癖，CI 上同一个用例同样崩**，只是 Linux 上
表现为 SIGSEGV 而不是 `STATUS_STACK_OVERFLOW`。所以它必须修，不能按"只在本机复现"跳过。

已排除的假设（都做过实测探针，探针已还原）：

1. **不是 `QApplication::notify` 嵌套**。把测试里的 `QApplication` 换成
   `notify()` 带深度计数的子类，整轮下来深度最大只有 2。所以事件重入不是元凶。
2. **不是控制器/浮动工具栏自我重入**。在 `placeToolbar` / `updateGeometry` /
   `resizeEvent` / `Session::event` / `prepareForDisplay` / `commitGeometryUpdate` /
   `refreshGeometryForVisibleContent` / `handlePaletteContentChange` 上加共享深度计数器，
   最大深度 3；再给 `moveContentTo` / `setPlacementContext` 加"深度>6 就返回"的硬闸，
   一次都没触发，崩溃照旧。
3. **不是采样窗口自己重入**。`ScreenshotCanvasColorSamplerWindow::updateSample` 与
   `paintEvent` 的深度计数一次都没超过 1。
4. 栈指针采样（下面这套工具）显示栈是**一路下降到接近栈底**才撞上守卫页，也就是真·深递归，
   不是单帧巨型 `alloca`。

排查工具（都在 `.workbuddy/qt5-port/`，不入库）：`dump-on-crash.ps1` 现在能打出
`EXCEPTION_RECORD.ExceptionInformation[0..1]`（读/写 + 目标地址），
`stack-any-dump.py` 能按线程号和 fault rip 符号化栈。**这些都不够用**，因为栈溢出的那个
dump 没有可用的展开信息（`MiniDumpWriteDump` 带异常流时会返回
`0x80070006 ERROR_INVALID_HANDLE`，只能退化成不带异常流的 dump）。

原本以为"最深栈点采样"是下一步：在测试里起一个线程，`SuspendThread` +
`GetThreadContext` 采主线程，**只在 RSP 创新低时**把 `(rsp, rip)` 写进文件，进程崩掉之后
再离线符号化。这条路走通了采样（~21 个下降点、深度约 500 KB）但符号化对不准，而且它只能
给出 RIP，看不出一层的**形状**。**已经证明不需要它**：栈溢出的 dump 里线程栈内存是完整的，
线性扫栈 + `stack-from-minidump.py` 就能把 700 层周期照出来，见下一节。

复现命令（本机）：

```
cd build/windows-qt5-tests/snow_shot/test-bin
PATH="/c/Qt/5.15.2/msvc2019_64/bin:$PATH" \
QT_PLUGIN_PATH="C:/Qt/5.15.2/msvc2019_64/plugins" \
QT_QPA_PLATFORM_PLUGIN_PATH="C:/Qt/5.15.2/msvc2019_64/plugins/platforms" \
./snow-shot-global-canvas-tests.exe --color-sampling-only -platform offscreen
```

### C.11 补充：先修掉的是另一个 bug（模板版本），爆栈仍在

`--color-sampling-only` 之外，不带参数的 `snow-shot-global-canvas-tests` 在本机 offscreen 上
也会挂；它挂的位置更靠后，而且**在 `-platform windows` 上同样复现**：

```
rc=1  save test template        # 断言失败，不是崩
```

`save test template` 对应 `DrawTemplateSettings::setTemplates()` 返回 false。根因不是平台，
是**跨语言的版本常量漂移**：Rust 侧
`DRAW_TEMPLATE_SCHEMA_VERSION = 7`
（`snow_draw_engine_qt/crates/snow-draw-engine-editor/src/draw_template.rs`），
而 C++ 侧 `normalizeDrawTemplates()` 的上限还写着 6
（`snow_shot/src/storage/configurationschema.cpp`）。于是引擎刚写出来的模板被 schema
判成"不支持的未来格式"整条丢掉，`setTemplates` 只能返回 false。这条同时解释了 CI 里
`560 snow-shot-styled-template-storage-tests` 的失败。

修掉之后（`c4c8108c`）同一个套件不再停在这句断言上，而是继续往下跑到**同一个栈溢出**，
说明爆栈和模板问题是两件事。修完的模板用例顺带提供了一个更好的复现面：不带参数的套件
和 `--color-sampling-only` 都能在 40 秒内撞上它。

**当时的一个错误推断**：两次崩溃前 `does not support raise()` 的条数分别是 661（采样变体）
和 716（完整套件），和"递归深度"同一量级，于是被读成"每一层递归都在 raise 一个顶层窗口"。
修完之后这个条数是 692 / 722，**没变**——raise 是测试自己正常的显示/前置动作产生的，
和递归深度只是碰巧同量级。教训：只凭"两个数字同量级"连不成因果，得让修完之后的数字
再讲一遍同样的故事才算数。

### C.11 结论：`WinIdChange → setScreen() → WinIdChange` 的自我递归

#### 怎么拿到的栈

不用"最深栈点采样"（上文的采样线程只给出 RSP/RIP，符号化对不准）。真正管用的是
**把 `dump-on-crash.ps1` 当调试器**：它 `DebugActiveProcess` 之后收到的**第一个**异常就是
真凶，后面的不是。

```
exception 0xC00000FD at 0x7FF6CD17BB77 first-chance=1 access=1 0xD78C803B10   ← 真凶
exception 0xC0000005 at 0x7FFF6A1C9772 ... (×3)                              ← 死后的异常派发
```

`0xC00000FD` 是 `STATUS_STACK_OVERFLOW`，`access=1`（写）+ 目标地址落在栈区间下沿，
正是 `_chkstk` 写守卫页；`0x7FF6CD17BB77` 在 exe 映像内，符号化后是
`WindowShortcutManager::eventFilter+0x27`（+39 字节，就是函数序言）。后面那三条
`0xC0000005` 的 rip 落在 ntdll，是线程已经死掉之后系统还在派发异常——**别拿它当崩溃点**。
上一轮的"最深栈点采样"其实也摸到了同一个 `eventFilter+0x27`，只是单点 RIP 说明不了
"这个函数是被谁一层层调进来的"，于是它被误读成"环在 eventFilter 之上"，方向就偏了。

转储里没有异常流（`MiniDumpWriteDump` 带异常信息仍返回 `0x80070006`），但
`MINIDUMP_THREAD.Stack` 的**内存是完整的**，所以离线线性扫栈就够：

```
python .workbuddy/qt5-port/stack-from-minidump.py \
  .workbuddy/qt5-port/dumps/gcs4/snow-shot-global-canvas-tests.dmp \
  22228 snow-shot-global-canvas-tests.exe 120
```

三个必须踩过的坑（都已在脚本里修好）：

1. **候选地址只认 `.text`**。按整张映像范围筛会把 `.data/.rdata` 里的 vtable、字符串指针
   也算进来，它们翻不出符号就掉到"最近的下一个符号"，输出成 `+0xA41CF0` 这种一眼假的位移。
2. **栈向下长**：低地址 = 最内层帧。上一版取 `entries[-keep:]` 拿到的是 CRT 入口那几层
   （`mainCRTStartup`），完全看不到递归。
3. 测试 exe 不叫 `snow_shot.exe`，映像名要能传参。

#### 栈长什么样

同一组返回地址以 **≈13 KB 一层**的周期重复了 700 次，每层固定这几个：

```
ScreenshotFloatingToolPaletteWindow::event            (+0x88 / +0x99)
ScreenshotFloatingToolPaletteWindow::applyPlacementScreen
AdDpiStableWindowController::eventFilter              (+0x16A)
WindowShortcutManager::eventFilter                    (+0x2054, 出现两次)
QPointer<QWidget>/QPointer<QScreen>::data, QWeakPointer<QObject>::internalData,
QBasicAtomicInteger<int>::loadRelaxed ...
```

两个 `eventFilter` 都在链上很好解释：它们是同一个事件派发的**过滤器链**
（`QApplicationPrivate::notify_helper` 会先过全部 object filter 再进 `event()`），
不是两层嵌套。真正的环只有两跳：

```
ScreenshotFloatingToolPaletteWindow::event(QEvent::WinIdChange)
  → applyPlacementScreen()
    → QWindow::setScreen(m_placementScreen)      // 重建平台窗口
      → QEvent::WinIdChange 又一次送到本 widget
        → event() → applyPlacementScreen() → ...
```

#### 为什么 offscreen 上才炸

`applyPlacementScreen()` 的终止条件写成"已经在新屏上就返回"：

```cpp
if (handle->screen() == m_placementScreen) return false;
handle->setScreen(m_placementScreen);
```

Qt 5.15 的 `QWindow::setScreen()` 文档原文是 **"If the window has been created, it will be
recreated on the newScreen"** —— 已创建的窗口会被**重建**，重建就会给 widget 补一个
`QEvent::WinIdChange`（这一点文档没写，但栈的形状和"窗口一重建就重发 WinIdChange"的既有
行为都指向它；本机能复现的 Qt 5.15.2 上实测如此，换 Qt 版本要重新确认）。而 QPA offscreen
只暴露一块屏：当 `m_placementScreen` 不是它、`setScreen()` 又被拒时，`handle->screen()`
永远不等于目标屏 —— 于是每进一层都再 `setScreen()` 一次，环没有出口，只能等栈耗尽。
`-platform windows` 上 `setScreen()` 能落地，第二轮进来时终止条件成立，所以同样的代码在
Windows 上一直是好的。

这也解释了为什么上一轮那些计数器全部"无辜"：环确实不经过 `QApplication::notify`
（它只在 `notify_helper` 之上）、也不经过控制器/采样窗口，它只在**这一个 widget 的
`event()` 与 `applyPlacementScreen()` 之间**闭合。

#### 修法

一次性尝试，拒绝嵌套重入（和 `ScreenshotOriginalImagePreviewWindow::applyGeometry()`
已有的 `m_applyingGeometry` 同一形状）：

```cpp
if (m_applyingPlacementScreen) return false;
QScopedValueRollback<bool> applying(m_applyingPlacementScreen, true);
handle->setScreen(m_placementScreen);
```

#### 修完的判据

| 入口 | 修前 | 修后 |
| --- | --- | --- |
| `-platform offscreen`（完整套件） | `rc=127`（`0xC00000FD`），716 条 raise | **EXIT=0**，722 条 raise |
| `--color-sampling-only -platform offscreen`（CI 上那条） | `0xC00000FD`，661 条 raise | **EXIT=0**，692 条 raise |
| `-platform windows`（完整套件） | EXIT=0 | **EXIT=0** |
| `ctest -R snow-shot-global-canvas`（这条 exe 的全部 14 个注册条目） | 崩在 #636 | **14/14 passed**（87s） |

补一次回归面：工具栏几何直接相关的那条 exe 也重编跑了
（`ctest -R floating-toolbar`，17 条）。`drag-tests` / `reverse-drag` / `seam-straddle-drag` /
`drag-print` / `dpi-transaction-offscreen` 全绿；另有 2 条红
（`keyboard-focus-windows-tests`、`action-layout-tests`）**做了 A/B**：把这两个文件退回
`0d52a58d^` 重新编译后它们照样失败，所以是本机既有的前台焦点 / 配置原子写问题，与重入闸无关。
ctest 输出里的 `***Not Run` 是 exe 没构建，不是失败，别混进判据。

顺带确认：`raise()` / `setting window masks` 这些 "This plugin does not support ..." 警告在
修完后**照旧刷**，它们只是 offscreen 少几个平台能力，不是症状，不要去"修"它们。

### C.12 Linux 上的窗口元素选择器仍是 stub（3 条 ctest 因此不再注册）

`window-element-api`、`selector-phased`、`selector-lifecycle` 这三条 ctest 是**同一个 exe**
（`snow-shot-window-element-api-tests` 加 `--selector-only` / `--selector-lifecycle-only`），
三条在 CI 上一起红，报的都是 `initial selector refresh failed`。

顺着断言往下读一层就能看到它们同源：

```
snow_shot/src/presentation/selector/screenshotselectorserviceclient.cpp:21
    #if defined(Q_OS_WIN) || defined(Q_OS_MACOS)      ← 真实实现
    ...
    #else
    // Native selector unavailable on unsupported platforms.
    bool ScreenshotSelectorServiceClient::hasService() const { return false; }
    bool ScreenshotSelectorServiceClient::ensureService()            { return false; }
    bool ScreenshotSelectorServiceClient::startRefresh(...)          { return false; }
```

也就是说 **Linux 走的是 stub 分支，`startRefresh()` 恒为 false**，三条测试断言的是一个
不存在的后端，不是产品在 Linux 上算错了。

**这不是"忘了加 ifdef"，而是 C ABI 还没有 X11 这个后端**：

| 层 | 现状 |
| --- | --- |
| `snow-ui-selector/src/linux.rs` | x11rb 后端**已实现**（`refresh`/`refresh_excluding_ids`/`refresh_with_displays`/`release_cache`） |
| `snow-ui-selector-c`（C ABI） | backend 枚举只有 `Uia`/`Msaa`/`Accessibility`，没有 X11；`lib.rs` 只有 `cfg(target_os = "macos")` 与 `not(macos)` 两条 |
| `snow_ui_selector.h` | `SnowUiSelectorBackend` 只有 UIA/MSAA/ACCESSIBILITY |
| Qt 侧 | `#if defined(Q_OS_WIN) \|\| defined(Q_OS_MACOS)`，其余平台 stub |

所以要真正接上，得跨三层改：C ABI 加 `SNOW_UI_SELECTOR_BACKEND_X11` 并在 `lib.rs` 里把它
分派到 `linux.rs`，再把 Qt 侧的门控打开并处理"X11 没有 hwnd，用 XID"这层差异。这是功能开发，
不是断言修正，不该混在一次修测试的提交里。

**处置**：三条 native 契约改在 `if(WIN32 OR APPLE)` 里 `add_test`，Linux 不再注册；
同一 exe 的三个设置类入口（`--shortcut-exit-confirmation-settings-only`、
`--auto-qr-settings-only`、`--guide-settings-only`）保持全平台注册，它们在 Linux 上一直是绿的。

**这一步踩到的坑（比结论本身更值得记）**：只把 `add_test()` 包进条件是不够的。下面这段
`set_tests_properties()` 的列表里还列着那三个名字，而对**从未 add 过**的测试调用
`set_tests_properties()` 是 **configure 阶段的硬错误**：

```
CMake Error at snow_shot/CMakeLists.txt:3622 (set_tests_properties):
  set_tests_properties Can not find test to add properties to:
  snow-shot-window-element-api-tests
```

如果就这么推上去，Linux 的 `Configure` 步骤会直接失败，比原来红三条严重得多。
抓到它的办法是把条件临时写成 `if(FALSE)` **在本机模拟 Linux 分支**——本机只能 configure
Windows 分支，这正是"未被解析的代码面=盲区"的又一次实例。修法是把三个名字收进
`_snow_shot_native_selector_tests` 变量，条件成立才填值，`set_tests_properties()` 展开它。

双向判据：

| 条件 | configure | 注册条目 |
| --- | --- | --- |
| `if(WIN32 OR APPLE)`（本机真实） | 成功 | 6 条（3 native + 3 settings） |
| `if(FALSE)`（模拟 Linux） | **成功** | 3 条（只剩 settings） |

#### 9.6.50 `QVariant::isNull()` 在 Qt5 与 Qt6 不是同一件事

Qt5 里 `QVariant(QString()).isNull() == true`；Qt6 把 `isNull()` 收窄成"invalid 或空指针"。
于是 `isValid() && !isNull()` 这种"有值"判据在 Qt6 上是恒等于 `isValid()` 的废话，在 Qt5 上却会
**静默丢掉一个类型正确、内容为空的值**。

本机 Qt 5.15.2 实测：

```
QVariant()          valid=0 null=1
QVariant(QString()) valid=1 null=1   <- 分歧点
QVariant("")        valid=1 null=0
QVariant(" \t ")    valid=1 null=0
```

`AdSelect` 正好踩到：`normalizeSelectValue()` 的注释已经写明"null 与 empty 必须共享同一个键
（QDataStream 区分二者）"，但函数开头那句 `if (!value.isValid() || value.isNull()) return {};`
在 Qt5 上先把 null QString 拦掉了，后面那段 trim 折叠根本没机会执行。表现是
`setCurrentValue(QString())` 变成"清空选择"，`adqt-select` 的
`emptyStringValuesSelectTheirLabel` 第一个组合就红。

修法：把"有值"收敛到一个 helper（`detail::hasSelectValue()` = `isValid()`），归一化、取键、
`setCurrentValue`、`currentText`、列表行同步五处一起换。Qt6 行为不变。

**门禁思路**：全仓搜 `isNull()` 搭配 `QVariant` 的地方，逐个人工判"这里想表达的是 invalid 还是
null"。不能机械替换 —— `QPixmap::isNull()` / `QString::isNull()` 是另一回事。

#### 9.6.51 Qt 光栅 clip 是"向外取整"，`toPoint()` 看不见它

OCR 字形缓存那条守卫原本比较 `device.map(clip.topLeft()).toPoint()` 与缓存侧的同一表达式，
想判断"取整后的 clip 是否相同"。但 Qt 建立光栅 clip 时对**左/上取 floor、右/下取 ceil**，
而 `toPoint()` 是**四舍五入** —— 一个只在底边上差一行的 clip，四舍五入后完全一样。

实测（dpr=2、canvas 变换 1.25）：

```
direct clip = 89,84 551x45
cached clip = 89,84 551x46     <- 缓存多一整行
```

缓存把 clip 平移到整数像素后再取整，加法顺序变了就会跨过边界，于是缓存多画一整行选区高亮。
改成比较 `mapRect(...).toAlignedRect()`；原来的角点比较保留，它还能覆盖亚像素那类情况。

判定信号是测试侧的 `ink cached=40376 direct=39825 solid_violations=551`：差异像素全部落在
"直绘是纯色"的位置 ⇒ 是画错位置，不是舍入误差。**只看差异像素数量会把它误判成容差问题。**

#### 9.6.52 "超长文件名 = 保存失败"这条前提被上游 #2004 拆掉了

`2fe8b10c`（#2004，导出路径支持时间变量）给**每个路径组件**加了截断
（`composeExportFileName` 上限 248 UTF-8 字节，留 7 字节给 `QSaveFile` 的 `.XXXXXX`）。
于是"用 300 个 `x` 当文件名制造确定性文件错误"这招彻底失效：写入**成功**，文件名被截到 248。
三处仍按老前提写的用例因此一起红，而它们的失败文本会把人带偏：

```
DRAGFAIL PROBE executor reached path=.../snow-shot-drag-pyNagI/xxx…xxx.png exists=1 size=155
encoding failure is reported
```

真正的红是最后那句 `wait` 超时（`failed` 永远是 false，因为根本没失败），不是它上面那句
"failed export must not start native drag" —— 后者是探针里 hoisted executor `throw` 自己造的假象。
**失败文本先分辨是"断言失败"还是"等待超时"，再决定改产品还是改夹具。**

这条前提现在还能不能造出来？不能，三条路都堵死：

- 非法字符（`/ \ : * ? " < > |`）在 `normalizeFilenameFormat` 就被 `setAutoSaveFilenameFormat` 拒掉；
- 长度被截断吃掉了；
- 目标目录由 `automaticDirectories()` 给出，配置目录 `mkpath` 失败还会回落到 Pictures/Documents。

所以三处只能换注入面，各自不同：

| 用例 | 原因 | 新构造 |
| --- | --- | --- |
| drag export | 暂存目录写死 `QDir::temp()`，文件名格式来自 schema 校验过的设置 | 新增 `setStagingRoot()`，把根指向一个**普通文件** ⇒ `QTemporaryDir` 建不出来（`ENOTDIR`/`ERROR_DIRECTORY`，两端一致） |
| pin execution auto-save | `ScreenshotAutomaticSaveRequest` 是结构体字段，**不过 schema** | `filenameFormat = "blocked/name"`，正是保存层对"老配置里带分隔符"的拒绝分支 |
| shared image export | 同上，但走设置 | 改成断言新行为：组件被**截短**、`copyFile` 仍然发布那个有界名字 |

第二个用例还有个坑值得记：它先 `presentPinnedArtifact(..., request)` 再改
`automatic.filenameFormat` 并 `request = automatic`，**这是死代码** —— 服务早按值收下了第一份请求。
探针 `ok=1 path=.../pin-0-4.bmp fmt=blocked/name` 一眼定案：实际保存用的是**第一个**格式。
把失败构造放在第二处，测试会"看起来改了却毫无变化"。凡是"改完没反应"，先查请求是不是被按值收走。

反向验证：把 drag export 的格式还原成 300 个 `x`，该用例立刻回到 `wait` 超时（红）；
换回注入根即绿。三组本地全绿：`snow-shot-pinned-drag-export-tests`、
`snow-shot-pinned-shared-image-export-tests`、`snow-shot-pin-execution-autosave-tests`。

#### 9.6.53 Qt5 `QImage` 从 const 数据构造后，元数据 setter 会 detach 并丢掉 cleanup

`captureCollageImage()` 用只读包装（`QImage(const uchar*, ...)`，`ro_data=true`）把捕获的栅格
和一块"预算预占"的生命周期绑起来：cleanup 回调负责释放 `CapturedImageLifetime`。
Qt5 下 `setDotsPerMeterX` / `setColorSpace` / `setText` 任何一个元数据 setter 都会先 `detach()`
——**深拷贝一份像素，新图不带那个 cleanup**，于是旧图析构时 cleanup 立刻跑掉，预占随之归零。
现象是"刚返回就已经释放"：

```
CAPTURE RETURN internal=155232 public=155232
BR2A reserved=0 key=365072220164     ← 返回后立刻归零
```

逐个 setter 加探针把界缩到 `setDotsPerMeterX`（`310464 → 155232`，正好一份像素）。
改法：捕获的栅格本来就是唯一所有者，直接 `bits()`（可写包装）取像素，引用计数 1 时不会拷贝；
改后 `BR2A reserved=155232`，预算断言通过。

判据：断言"预占在返回之后仍然在账上"能抓出"返回即失效"；只断言"返回的图非空"完全抓不到。

#### 9.6.54 离屏平台没有字体时，比较文字像素等于比较两张空白

stitching 的文本用例在改完 `formattedDocument` 后比较拼接快照的像素，本机红：

```
STITCH PROBE beforeEqAfter=1 frozenStable=1 before=504x77 after=504x77
stitching captures rendered text edits without retaining a mutable document
```

探针一条条证明"文档换了、分支对了、几何也对"，但就是**画不出像素**：

```
PVP PROBE doc=0x… len=19 text=[Current pinned text]
PVP PROBE geom doc=336x51 itemPos=0,0 scale=1.5 mapped=(0,0,504,76.5) layerGeom=(0,0,504,77)
PVP PROBE branch formatted=1 vp=504x77 differing=1     ← 只有探针自己点的那个红点
```

`-platform offscreen` 的 freetype 字体库没有字体目录（`QFontDatabase: Cannot find font directory
C:/Qt/5.15.2/msvc2019_64/lib/fonts`），不显式装载字体时每个字形都是空白。同一文件里其它文本夹具
都先 `addApplicationFont(":/recording-test-fonts/SnowRecordingTestSans-Regular.ttf")` 再
`QApplication::setFont(...)`，只有这条忘了 ⇒ 编辑前后都是空白，`before != after` 永远不成立。
补上后 `--stitching-snapshot-only` 与 `--multi-selection-only` 均 rc=0。

判据：**凡是"改什么都相等"的断言，先确认被测对象可见**（字体、非空像素、非空集合），
否则断言在测空气。"与中心像素不同的像素数"（`differing`）是最省事的可见性判据。

#### 9.6.55 持久化记录的是**内容矩形**，控件矩形比它多一圈阴影边距

上游重构成"内容矩形 / 外部矩形"之后，差分测试里四处夹具还在拿两者互比，永远不可能成立：

```
TF PROBE op=flipH before=24,24 352x212 after=40,40 320x180
                                   ↑ currentNativeGeometry()（含 16px 边距）
                                                   ↑ record.nativeGeometry（内容）
```

判据：`pinnedContentRect(currentNativeGeometry()) == config.nativeGeometry` 在
`pinnedSmallExtentRemainsResponsive()` 里已经写明——`config.nativeGeometry` 就是内容矩形。
平台层 `applyGeometry()` 收的也是外部矩形（探针实测 `authority=165,165 43x45` 恰好等于
`pinnedOuterRect(QRect(181,181,11,13))`），所以"注入 1px 漂移"必须注入在外部空间里。

同一类错误还有一处隐形的：OCR 文本层的父控件是内容控件（`layerAt=16,16`），而
`canvasToViewTransform()` 给的是画布坐标（`canvasAt=0,0`，尺寸 352x212 = 整个窗口），
两者差正好一个边距 16。比较前要用 `layer->mapTo(window)` 减 `canvas->mapTo(window)` 折算；
注意 `mapTo` 要求祖先关系，跨容器要用共同祖先（用画布当父参数会触发
`parent must be in parent hierarchy` 断言并终止进程）。

#### 9.6.56 延迟初始化会给载荷**追加**一段，别用整串字节断言"没变"

`pinnedSnapshotRetainsRecognitionBeforeDeferredSetup()` 断言 setup 之后 OCR 结果不变，实测：

```
EO PROBE same=0 earlyBytes=166 settledBytes=236
EO PROBE settled=<..早期字节..>53524d44 01000000 3d7b226c617465782e.
                                ↑ 视觉模型选择载荷（marker+version+len+JSON）
```

前面那份 OCR 字节一位不差，只是尾部多了一段"模型选择"。断言应是
`settled.startsWith(early)`（加 `recognition.requests == 0`），而不是整串相等；
整串相等还会在**序列化器规范化空字段**时误报——`table->error/code` 是 null 还是
空串在 JSON 往返后不可区分，序列化器统一折成非 null 空串，测试侧期望必须同样折一次。

#### 9.6.57 离屏无字体：文本断言在比空气，旋转时直接 abort

`-platform offscreen` 的 freetype 没有字体目录。两类后果：

1. 所有字形空白 ⇒ 像素比较等于比两张空白（§9.6.54），`before != after` 永远不成立；
2. **更狠**：把零度量的字形放进旋转的 `QGraphicsItem` 里栅格化，Qt5 直接
   `ASSERT: "mask.width() <= c.w && mask.height() <= c.h"`（`qtextureglyphcache.cpp:331`）
   然后 `terminating the test process immediately`——不是测试失败，是整进程死。

判据：只要夹具里出现文字渲染（哪怕只是复用别人的 OCR 结果），先
`addApplicationFont(":/recording-test-fonts/SnowRecordingTestSans-Regular.ttf")` +
`QApplication::setFont(...)`。看到 `qtextureglyphcache.cpp:331` 先怀疑字体，别去查几何。

#### 9.6.58 `processEvents()` 不排空 `DeferredDelete`

关闭一个顶层窗口走的是 `deleteLater()`；只用
`while (!guard.isNull()) { processEvents(); msleep(1); }` 等它，会老老实实等到超时。

```
close merged translation pin        ← 5s 后仍非空
```

修法：循环里补 `QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete)`，
与 `screenshot_pinned_window_tests.cpp` 里 `processUntilDeleted()` 的写法一致。

#### 9.6.59 垫片丢掉 `format` 的代价，以及预乘取整差 1 带来的连锁

`convertedToColorSpace(space, format)` 在 Qt5 只有单参版本，垫片原本把 format 丢掉。
后果在一个夹具上暴露得很干净：

```
CS PROBE space=[000001cb] presentation=0 differing=1 first=100,80
        got=80643219 want=80c86432      ← 预乘 vs 非预乘，只差半透明那一个像素
```

改成"先转空间、再 `convertToFormat(format, flags)`"（与 `snow_canvas_image.cpp` 里
Qt5 分支同一写法）后 `expectedFmt=6 == paintedFmt=6`、`differing=0`。

连带两个坑：

- 引擎的 Qt5 路径保留源布局，而 Qt5 自己会先规范化成 `ARGB32_Premultiplied` 再重采样，
  两边的**预乘取整差 1**。比较前先折到同一布局；仅当"布局本来就不同"时允许 ±1/通道，
  布局相同时仍要求逐字节相等（这样 Qt6 的严格性一点没松）。
- 折布局会长出额外的 `QImage` 副本，`checkRelease()` 判的是"释放后页已归还"，副本还活着
  就判不过 ⇒ 参考图必须放进内层作用域，让它在 `checkRelease()` 之前析构。

#### 9.6.60 调色板图像的色彩空间转换：二次量化 + 「小图干脆不转」

`Format_Indexed8` / `Mono`（以及 8 位灰阶）源图做色彩空间转换时，Qt5 与 Qt6 走的不是同一条路：

| | 做法 | 结果 |
|---|---|---|
| Qt6 单趟 `convertedToColorSpace(space, format, flags)` | 直接按目标布局展开，**调色板本身被转换** | 精确，分块/整图一致 |
| Qt5 两步（先转空间再 `convertToFormat`） | 展开 32 位 → 变换 → **重新量化回 8 位索引** | 每个通道可能整体挪一格 |

实测（P3 源 → sRGB，纯色索引项 `(51,102,51)`）：

```
正确的 P3→sRGB            = (26,103,44)      ← 显式转换调色板 / Qt6
Qt5 两步走的产物          = (0,102,51)       ← 索引 49 → 13，颜色被重新量化
未转换（分块路径拿到的）  = (51,102,51)
```

更阴的一条：**Qt5 这条转换路径对小图是空操作**。同一条源图裁剪成 15 行 / 40 行，`convertedToColorSpace()`
原样返回（调色板、索引都不动，只换 tag）；整图 263 行才真的转换。所以分块路径（tile view）
拿到的是"没转"的结果，整图路径拿到的是"转了但被量化过"的结果，两者逐字节都对不上：

```
row conversion source=3 size=1031x263 target=1 first=1 count=261 row=0
RP owned   pal49=51 index18=13 bytes=0,102,51,255     ← 整图：转了，被量化
RP tile    pal49=51 index18=49 bytes=51,102,51,255    ← 分块：根本没转
```

判据：**只要源 `depth() <= 8` 且要转色彩空间，就先展开到 32 位再转**（等价于 Qt6 的单趟语义），
不要依赖 Qt5 自己在索引布局里"顺便"处理。两处必须同改，否则夹具与产品各绿一半：

- `snow_canvas_image.cpp` 的 `convertColorAndFormat()` Qt5 分支；
- `qt_compat.h` 的 `snowImageConvertedToColorSpace()`（测试的参考值就是它算出来的）。

```cpp
if (image.depth() <= 8) {
    QImage expanded = image.convertToFormat(QImage::Format_ARGB32);
    expanded.setColorSpace(image.colorSpace());
    return expanded.convertedToColorSpace(colorSpace).convertToFormat(format, flags);
}
```

改完 `snow-canvas-image-tests` 从"停在 borrowed RGBA rows … color/alpha rounding"变成全绿。
副作用是好的：展开后再变换是**逐像素**运算，与分块边界无关，tile 路径与整图路径重新一致。

#### 9.6.61 packed 8 位源 + 分数 DPR：`transformed()` 直接段错误

`QImage::transformed()` 在 Qt5 上碰到 **`depth() <= 8` 的源 + `devicePixelRatio() != 1`** 不是"算错"，是**进程直接没了**
（`EXIT=139`）。矩阵（最小复现工程，脱离仓库树，`cl` + Qt5 直编）：

| 源格式 | DPR=1 | DPR=1.25 | DPR=2 |
|---|---|---|---|
| `Format_Mono` | 正常 | 崩 | 崩 |
| `Format_MonoLSB` | 正常 | 崩 | 崩 |
| `Format_Indexed8` | 正常 | 崩 | 崩 |
| `Format_Grayscale8` | 正常 | 崩 | 崩 |

崩溃点随变换走：`rotate(90)` 后就地 `scale(-1,1)`（即 isometric 镜像那串）最稳，单步 `rotate(90)` 有时能过。
只要把 DPR 换成 1，同样四组格式 × 六种变换全部正常——**DPR 对像素结果本来就没有影响**，它在 Qt6 里只是元数据。

所以引擎对 `depth() <= 8 && dpr != 1` 走"DPR 中和副本"，算完再回填：

```cpp
QImage request = image;
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    const bool neutralRatio = image.depth() <= 8 && image.devicePixelRatio() != 1;
    if (neutralRatio) request.setDevicePixelRatio(1);
#endif
    QImage transformed = request.transformed(transform, mode);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    if (neutralRatio && !transformed.isNull())
        transformed.setDevicePixelRatio(image.devicePixelRatio());
    // …紧随其后的 colorSpace / offset 补齐见 9.6.62
#endif
```

夹具 `packedFormatsSurviveFractionalDpr()`：4 格式 × 6 变换 × 2 采样模式，先取 DPR=1 的结果当 oracle，
再取 DPR=1.25 的结果比较"像素逐字节相同 + DPR 仍是 1.25"。反向验证：删掉 `setDevicePixelRatio(1)` 那两行，
`MUTATION EXIT=139`；恢复后 `EXIT=0`。

教训不在"补个守卫"，而在**崩溃定位**：这条红最初表现为整个 exe 无输出退出，`ctest` 只说 fail。
用 `IMG_PROGRESS` 探针（env-gated `fprintf`，打进产品 TU、复用已链接的测试 exe）才把范围从"整轮"缩到
"`Format_Mono` + `dpr=1.25` + `rotate(90).scale(-1,1)` 这一格"。

#### 9.6.62 `transformed()` 的元数据契约：Qt5 丢 colorSpace 与 offset

同一批用例里冒出来的第二条：`optimized operations must preserve Qt metadata` 在
`dpr=1.25/1.25 dpm=… offset=17,29/0,0` 上红——**变换后 offset 变回 0,0，colorSpace 变成 sRGB**。探针（`meta_probe`）逐项确认：

| 元数据 | Qt5 `transformed()` | Qt6 | 引擎承诺 |
|---|---|---|---|
| `dotsPerMeterX/Y` | 保留 | 保留 | 保留 |
| `text()` | 保留 | 保留 | 保留 |
| `colorSpace()` | **丢**（回到默认） | 保留 | 保留 |
| `offset()` | **丢**（归零） | 保留 | 保留 |

引擎的实现契约是 Qt6 语义，所以 Qt5 分支在 `transformed()` 之后补回这两项（见 9.6.61 代码块后半段）。

夹具侧的问题更值得记：这 12 处断言原来是拿 `source.transformed(...)` 直接当参考量的，等于**把 Qt5 的缺陷当标准答案**。
引入 `qtTransformReference()` 把参考量先折到 Qt6 语义（补 `setColorSpace` / `setOffset`）再比，
12 处裸调用全部改走它。

判据：**任何"参照实现"如果本身是被 Qt5 实现的 API，必须先把它折到目标语义上再比**，否则测出来的是 Qt5 的行为差异，
不是产品的行为差异——而且方向是反的：产品对，测试红。

#### 9.6.63 平滑采样域：源域重采样、两个"写什么都不写"的格式、以及不可信的 `mirrored()`

三条互相独立，但全部只在 `Qt::SmoothTransformation` 路径上出现。

**(1) 采样域不同 ⇒ 必须折到同一墨空间 + 给容差。** Qt5 的 `qt_scaleImage` 在**源格式域**重采样：

- straight-alpha 源（`ARGB32` / `RGB888` / `RGB16` …）返回**预乘**结果；
- 部分 16 位格式（`RGBA64` 系）**原样返回 packed**，根本没转。

Qt6/引擎则在统一的 32 位预乘墨上做。所以比较时先两边都 `convertToFormat(Format_ARGB32_Premultiplied)`，
再按源位深给容差：

```cpp
// depth<24：5 位通道一格 = 255/31 ≈ 8.2，取 9；其余取 2
const int tolerance = source.depth() < 24 ? 9 : 2;
```

**(2) `Grayscale16` / `RGB30` 源目标同格式的平滑变换，Qt5 上是空操作。** 探针 `smooth_matrix` / `target_smooth`
把"源格式 × 目标格式 × 采样模式"全跑了一遍，只有这两格输出**全 0**（不是差一格，是一个像素都没写）。
修法是"画进墨再转回来"，源也要一起转，否则混合器还是走同格式那条死路：

```cpp
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
const QImage::Format paintedFormat = format;
const bool paintThroughInk =
    mode == Qt::SmoothTransformation &&
    (format == QImage::Format_Grayscale16 || format == QImage::Format_RGB30);
if (paintThroughInk) {
    format = paintedFormat == QImage::Format_Grayscale16
                 ? QImage::Format_RGBA64_Premultiplied
                 : QImage::Format_ARGB32_Premultiplied;
}
#endif
// …painter 分支
if (paintThroughInk) view = view.convertToFormat(format);
painter.drawImage(QPoint(), view);
painter.end();
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
if (paintThroughInk) {
    QImage converted = result.convertToFormat(paintedFormat);
    copyMetadata(image, converted);
    return converted.sizeInBytes() < qsizetype(snow::memory::kMappedPixelBufferMinimum)
               ? converted : snowCanvasCopyImage(converted);
}
#endif
```

反向验证：关掉 `paintThroughInk`，`source format 28` 的墨空间差 **254**（全空 vs 正确），恢复后绿。

**(3) `QImage::mirrored()` 在 Qt5 上不可信。** 两处独立证据：

- alpha=255 的像素也会挪一格：`ff0a141e` → `ff0d151e`（`mirror_cmp`）；
- 大图 straight-alpha 直接**返回未映射结果**（`mirror_premul`）。

改成精确置换，两版行为一致：

```cpp
working = working.transformed(
    QTransform().scale(mirrored.testFlag(Qt::Horizontal) ? -1 : 1,
                       mirrored.testFlag(Qt::Vertical) ? -1 : 1),
    Qt::FastTransformation);
```

**(4) 但大图平滑镜像的 oracle 得反过来取 `FastTransformation`。** 引擎修好 `mirrored()` 之后，大图块仍然大面积差 255：
`mirror_probe` 证明**引擎的输出等于"精确整像素镜像"**，而 Qt5 的平滑镜像是**围绕半像素原点重采样**的，
两者系统性差一格。所以这一格的参照量不能是 `Qt::SmoothTransformation` 的 Qt5 结果，只能取 `FastTransformation`
（两版一致的精确置换）：

```cpp
// Qt5 平滑镜像 = 半像素原点的重采样，与"正确镜像"系统性差一格；
// 参考量取 FastTransformation：两版都是精确置换。
const QImage oracle = qtTransformReference(large, transform, Qt::FastTransformation);
```

命名上这三条都属于同一类：**"Qt5 也能跑、但跑的不是同一件事"**。区分办法只有一个——把矩阵跑全，
让每一格都有具体数值可比；"看起来没问题"的格子最贵。

#### 9.6.64 「假红灯」：陈旧二进制 + GUI 子系统吞掉 QTest 输出

本机那份 40 条的红灯清单里，至少一条是假的：`adqt-pagination-tests` 的源码 mtime（11-01 09:00）晚于
exe（10-31 10:07），而测试树里还躺着 **0 字节的 `adqt-segmented-tests.exe`**。也就是说那批"红"是在一棵
半新半旧的构建树上量的——重建之后 `adqt-pagination-tests` 直接绿，一个字符都不用改。

判据写在前面：**报告红灯之前先确认 exe 比源码新**。`ls -l` 比一眼断言便宜得多。

顺着这条挖出第二个坑，更隐蔽：**GUI 子系统的测试 exe 在终端里一个字都不打**。同一批测试里，
链 `adqt::core` 的小 exe 正常输出，链 `adqt::widgets` 的 15 MB exe（`QTEST_MAIN` → `QApplication`）
连 `********* Start testing` 都不打，`ctest --output-on-failure` 也是空的——只有一个

```
1/1 Test #43: adqt-pagination-tests ....***Failed    2.70 sec
```

没有失败原因，什么都看不出来。Windows 上 GUI 子系统进程只有在句柄被重定向时才写 stdout，而这里即使是
管道/文件重定向也拿不到，等于测试结果被整包丢掉。

绕过办法是让 QTest 自己开文件写，完全不碰 std 句柄：

```bash
./adqt-pagination-tests.exe -o /tmp/pg.txt,txt
```

`-o` 立刻把所有日志落盘（`PASS`/`FAIL!`/`Totals` 全在），上面那条红灯的第一手原因
（`installTranslator` 返回 FALSE）就是这么拿到的。

这两条合起来说明一件事：**"测试红"这个信号本身要先验证可信度**——比它新不新、你看不看得见它的输出，
任何一条不成立，后面所有排查都是白干。

#### 9.6.65 CI 日志下载固定在 707046 字节处断流

`gh-ci.py logs` 一直抛 `http.client.IncompleteRead(707046 bytes read)`。加了三重重试之后**每次仍然停在同一个字节数**，
说明不是网络抖动，而是这条链路上单个流式响应被截断（707046 ≈ 690 KB）。

`http.client.IncompleteRead` 的一个恶劣之处：`resp.read()` 已经消费掉的字节随异常一起丢，没法续传。

两条修法叠起来用：

1. `request()` / `download()` 全部走 `read_body()`，对 `IncompleteRead` / `ConnectionResetError` / `TimeoutError`
   重试四次（每次重新签自己的 302，所以重头拉是安全的）；
2. 重试也救不回来的，改成**分段 Range 拉取**：每次重新解析 `/actions/jobs/<id>/logs` 的签名 URL，
   带 `Range: bytes=a-b` 取 256 KB，拼起来。blob 存储认 Range，于是 3.95 MB 的日志能完整落地。

```python
req = urllib.request.Request(target, headers={"Range": f"bytes={len(collected)}-{len(collected)+CHUNK-1}"})
```

代价是每段一次 302，慢（约 3 分钟）；但"看不到日志"会让整轮 CI 白跑，这笔账划算。

分段拉取做成了独立脚本 `.workbuddy/uos-port/fetch-job-log.py`（用法：`python fetch-job-log.py <job-id> <out>`），
job id 从 `gh-ci.py jobs <run-id>` 的 `== [<id>] <name>:` 行取。

#### 9.6.66 渲染出来的 HTML 里，§9.7 被追加了 14 份

`docs-uos-linux-port.html` 是给人用浏览器读的副本，由 `.workbuddy/uos-port/sync-port-html.py`
从 Markdown 增量拼接（它不整篇重渲染，因为样式表、hero、目录都是手写的）。它有一处静默故障：

`SECTIONS` 里列了 `9.7 Linux CI 单元测试残留清单（…）`，而 §9.7 在 Markdown 里位于**文末**
（追加在附录之后）、在 HTML 里位于第 10 章之前。两条规则同时失效：

1. 通用规则是"替换 anchor 到**下一个同级标题**之间的内容"。`### 9.7` 下面是 `#### 9.7.1`，
   所以它每次只替换 §9.7 的引导段，`9.7.1`–`9.7.6` 一个字节都没进更新范围；
2. "已经在了"的判据是 `render()` 出的首行（`<h3>9.7 …</h3>`，**不带 `id`**）是否出现在 HTML 里，
   而文件里那一行历史上带过 `id`，于是判据永远为假 → 走"新建"分支 → 往 `<h2 id="10-` 前
   **再插一整份**。

结果：打开浏览器看到同一章重复 **14 次**（`9.7.1`–`9.7.6` 各 14 份、共 3258 行），
而每次运行都打印 `synced section: 9.7 …`，看起来一切正常。

修法：把它从 `SECTIONS` 挪到新的 `TAIL_SECTIONS`——从该标题一直渲染到下一个 `<h2>`，
整个块替换；标题匹配用正则容忍有无 `id`。

```python
first_line = rendered.split("\n", 1)[0]
pattern = re.escape(first_line).replace(re.escape("<h3>"), r"<h3[^>]*>", 1)
match = re.search(pattern, html_text)
following = re.search(r"<h2[ >]", html_text[match.end():])
```

**判据**：连跑两次必须都是 `nothing to sync`（幂等）。**反向验证**：往 Markdown 的 §9.7 里注入
一句 `REVERSE-PROBE-MARKER` → 跑一次，HTML 里该标记恰出现 1 次、且 `9.7.5` 仍只 1 份；
撤掉标记再跑，标记消失。只跑第一次（注入）看不出重复，因为重复是**累积**出来的。

顺带一条通用教训：**「脚本说同步了」不等于「读者看到的是对的」**。这类副本的判据不能只看
脚本的日志，要落到"同一标题在产物里出现几次"这种可数的量上。

同一轮还发现**反向的一笔**：附录 B、B.1、附录 C 从头到尾就没进过 HTML。
脚本的 `SECTIONS` 只列了 `9.6.x` 与 `9.7`，没人把这三章加进去，于是渲染出来的页面在附录 A
之后直接 `</main>`——整章实测基线（run 41264674368 / 41383823090 的 arm64 失败分类）和
Qt 5 崩溃栈那一章，读 HTML 的人一次都看不到，而 `md` 才是源。补法是 `APPENDED_SECTIONS`：
按 `(标题, 结束标题)` 成对列出，首次追加到 `</main>` 前，之后原地替换。

附录 C 的结束标题写作 `9.6.50 …` 而不是下一个 `##`，因为紧跟其后的 `9.6.50`–`9.6.66`
是由 `SECTIONS` 单独拼接的，整章渲染会让它们出现两份。

现在的判据是**章节数对齐**，不是"脚本说同步了"：

```
md h2: 16    html h2: 17（多出来的是手写的"目录"）
MD-ONLY / HTML-ONLY 均为空
```

`<div>`/`<main>`/`<table>`/`<pre>` 的开闭计数相等，`41264674368` 出现两次（与 md 相同）。


### 9.7 Linux CI 单元测试残留清单（2026-11-04，56/829）

`962cb5ab` 上 Linux x64 Release 的 `ctest`：**93% passed, 56 failed out of 829**（573 s）。构建、deb、AppImage、
元数据校验全绿，红的全在单元测试这一步。

先说结论：这 56 条**不是 56 个独立问题**，而是几类根因在不同套件上的投影。

#### 9.7.1 崩溃（4 条，最高优先）

| 用例 | 现象 |
|---|---|
| `adqt-button-signal-lifetime-tests` | `activationAllowsOwnerDeletion()` signal 11，栈是空的 |
| `snow-canvas-image-tests` | **SEGFAULT** |
| `snow-shot-mcp-document-tests` | SEGFAULT |
| `snow-shot-mcp-application-tests` | SEGFAULT |

`snow-canvas-image-tests` 这一条在 `dd92facf` 里应该已经跟着修掉了：本机崩溃点是
"packed 8 位源 + 分数 DPR 调 `transformed()`"（§9.6.61），Release/‑O2 下同样会踩。
所以下一次 run 的 delta 同时是这条修复的**反向验证**：它若还崩，说明 Linux 上另有第二个崩溃点。

`41801815105` 的 run 说明它没修掉：仍然 `***Exception: SegFault  2.19 sec`，主线程挂在
`libQt5Gui.so.5` 里，其余 5 条 `Thread (pooled)` 全在 `QWaitCondition::wait` 上空转——
即**崩的是主线程，不是 worker**。本机（Windows + offscreen）这整个套件是绿的，
所以只能靠 CI 定位。既然没有符号可用，就先让 exe 自己报位置：

```cpp
const bool trace = qEnvironmentVariableIsSet("SNOW_TEST_TRACE");
const auto step = [trace](const char* name, void (*fixture)()) {
    if (trace) { std::fprintf(stderr, "STEP %s\n", name); std::fflush(stderr); }
    fixture();
};
step("raw-buffers", &rawBuffersReturnPages);
…
step("chunk-boundaries-and-concurrency", &chunkBoundariesAndConcurrencyMatchQt);
```

配 `set_tests_properties(snow-canvas-image-tests … ENVIRONMENT "SNOW_TEST_TRACE=1")`，
下一次 run 的 ctest 段最后一行 `STEP …` 就是崩溃前跑到的夹具。**定位后两处一起删掉**——
这一条是为诊断临时加的，不能留在门禁里当永久噪声。

注意 `main()` 里那串裸调用改写成 `step(…)` 之后，**参数顺序就是执行顺序**，
改顺序会改变崩溃点，别顺手整理。

#### 9.7.2 字体度量：把「Windows 上恰好相等」写成了断言

| 用例 | 断言 |
|---|---|
| `adqt-button-lifetime-tests` | `sizeHint() == QSize(horizontalAdvance(text), metrics.height())` 精确相等 |
| `snow-shot-settings-navigation-tests` | 同上，换了个套件 |

两边用的是同一个 `QFont`，但 `AdButton::sizeHint()` 走的是 style 里的度量（可能过 `QFontMetricsF` 再取整），
`QFontMetrics` 走整型 advance，Linux 的 DejaVu 字距落在 .5 边界上时两者差 1 px 是必然的。
这类断言该改成"±1 px 内相等"，而不是把 Windows 的取整结果当规范。

已经改掉的两处（本机 Windows 改前后都绿——差值只在 Linux 上出现）：

| 位置 | 原来 | 现在 |
|---|---|---|
| `ant_design_qt/tests/button_lifetime_tests.cpp` | 宽、高都必须精确等于 `horizontalAdvance`/`height` | 两轴都放宽到 ±1，失败消息里带上实际差值 |
| `snow_shot/tests/settings_page_loading_tests.cpp` | 宽 ±2，高必须精确相等 | 高也放宽到 ±1 |

产品侧的成因也查清了，不是随意放宽：`measureTextWidth()`（`button.cpp`）取的是
`max(horizontalAdvance, boundingRect().width())`，**字形 bearing 探出 advance 时后者大 1**，
而 Linux 的 DejaVu 正是这种字体。所以"sizeHint 比 advance 宽 1 px"是正确行为，
断言该跟着产品语义走。真正的回归（内容内边距被加回来）会移动 8 px 以上，±1 的松弛度不影响判别力。

#### 9.7.3 鼠标 / 悬停坐标（5 条）

`snow-shot-physical-cursor-tests`（fractional hover）、`snow-shot-toolbar-guide-hover-tests`（global coordinates）、
`snow-shot-screenshot-selection-toolbar-hover-tests`、`snow-shot-export-dialog-placement-tests`。
四条**根因各不相同**，全都在本机 Windows + offscreen 上复现（开着 `QT_QPA_FONTDIR=C:/Windows/Fonts`
才跑得动，否则 offscreen 的字体库是空的，测试在字体断言上先倒）。

**(1) 半像素悬停：Qt5 在窗口级投递时把它折成整数。** `physical_cursor_tests` 的 Retina 场景里，光标
"移动一个物理像素" = 逻辑坐标 0.5 px，期望子控件收到 `QPointF(20.5, 30)`。实测：

```
hover mismatch: dispatched=1 moves=1 local=21,30 (want 20.5,30) global=131,140 (want 130.5,140)
```

事件确实投递到了（`dispatched=1 moves=1`），只是 `QWidgetWindow::handleMouseEvent` 用
`QMouseEvent::globalPos()`（Qt5 里是整数 `QPoint`）反算控件局部坐标，小数部分就此丢失；Qt6 全程保留。
于是夹具加了一个显式折叠函数，把"请求的位置"折成"当前 Qt 版本真正能投递的位置"：

```cpp
[[nodiscard]] inline QPointF deliveredMousePosition(const QPointF& requested) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return requested;
#else
    return QPointF(std::round(requested.x()), std::round(requested.y()));
#endif
}
```

（放在 `synthetic_mouse_test_support.h`，因为它是"合成事件的投递语义"这一层的事。）
断言仍然查"事件到了、按钮状态对、拖拽矩形对"，只是不要求 Qt5 做到它做不到的半像素。

**(2) 工具栏引导线：基准量取错了对象。** `toolbar-guide-hover` 原来拿
`receiver->mapToGlobal(local)` 当期望值，而产品转发的其实是**它收到的那次事件的全局坐标**。
普通 `QWidget` 不处理鼠标移动时 Qt 会把事件**转发给父控件并重算坐标**，所以"产品看到的事件"跟
"夹具请求的事件"不是同一个：

```
guide trace: raw=241,314 floored=241,314      <- 夹具发出的那次
guide trace: raw=242,315 floored=242,315      <- Qt 转发给父控件的那次（+1,+1）
guide mismatch: forwarded=242,315 expected=241,314
```

修法是不猜：装一个应用级过滤器，把**最后一次真正投递出去的事件**的全局坐标记下来，断言拿它当期望值。
这既保住了"必须转发事件的全局坐标"这条真契约，又不再把 Qt 的转发算术写进断言。

顺带同一条用例里还有个坑：`sendMove(&child, QPointF(3.8, 3.8))` 想验证"同像素的重复移动不触发重绘"，
但 Qt5 先把 3.8 折成 4，于是它跟前面的 3.1 落在**不同**像素上，真的该重绘。改用 3.4（两版都落在同一像素）。

**(3) 选区工具栏：没有活动窗口就没有焦点。** 失败在
`queued dropdown dismissal must preserve a newly focused external editor`。诊断：

```
focus mismatch: focused=null hostActive=0 platform=offscreen
```

`QApplication::focusWidget()` 只在窗口处于活动状态时才有值。offscreen 平台**从不激活窗口**
（连 `activateWindow()` 也是空操作），所以这条断言测的是平台，不是产品。两处闸门：
show 之后先探一次，popup 关闭后再探一次——因为 offscreen 会激活第一个显示的窗口，
随后原生 popup 抢走激活且永不归还。探不到就打印一行理由并跳过该函数的焦点检查。

**(4) 导出对话框：测试根本没跑在 offscreen 上。** 这条最冤——用例里早就写了
`if (QApplication::platformName() != "offscreen")` 的守卫，但 `CMakeLists.txt` 里
`snow-shot-export-dialog-placement-tests` / `-pipeline-tests` 两个变体**漏了**同门 `-workflow-tests`
有的 `ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"`。于是 Linux CI 上它们跑在 xcb + 无 WM，
`raise()` 是空操作，栈序断言必倒；Windows 本机因为手动带 `-platform offscreen` 反而是绿的。
补上环境后守卫生效。

这四条合起来是同一课的四个面：**"坐标对不上"要先问"期望值是从哪来的"**——是夹具请求的、Qt 转发的、
还是产品实际看到的。三者在 Qt5/Qt6 上都不一定相同。

`snow-canvas-tablet-input-tests` 原本也被归进这一组，但**它不是坐标问题，也不是 Linux 问题**：
把断言改成逐项打印期望/实际之后，本机 Windows Qt5 第一次跑就复现了——

```
tablet sample mismatch: device=2(want 2) id=2(want 2) has_pressure=0(want 1)
  timestamp=1234(want 1234) position=20.5,30.25(want 20.5,30.25) pressure=0(want 0)
```

唯一对不上的是 `has_pressure`。根因在适配器本身：Qt6 用
`pointingDevice()->capabilities()` 判断压感，Qt5 **没有这个查询通道**，`makePointerInput()` 只能退化成
"pressure() > 0 才算有压感"，于是"真实笔但此刻读数为 0"与"无压感轴的笔"无法区分——这是有意的降级，
代码里也写了注释。夹具却按 Qt6 语义断言"能力存在 ⇒ 永远 has_pressure=1"，两边自然打架。

修法是让断言跟着平台契约走，而不是去改适配器（改适配器等于把一个已知的信息缺口伪装成能力）：

```cpp
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        const int expectedHasPressure = 1;              // 能力可查询
#else
        const int expectedHasPressure = clamped > 0.0 ? 1 : 0;   // 只有读数能判
#endif
```

改完本机绿。这条教训值得单列：**"只在 CI 上红"不等于"只在 Linux 上错"**——那份 56 条清单里有几条
其实在本机 Windows Qt5 上也红，只是本地旧的红灯清单漏了它们（见 §9.6.64 的陈旧二进制）。
把"平台差异"这条直觉当过滤器，代价是反复推错根因。

#### 9.7.4 文本渲染 / 字形缓存（7 条）

`snow-canvas-text-draft-tests`、`snow-canvas-text-render-cache-tests`（同一条 882 px 不一致：
`reference-only=1 candidate-only=2 both-inked=879 cold=1 builds=3 hits=0`）、`snow-canvas-serial-number-render-tests`、
`snow-canvas-serial-number-font-tests`、`snow-canvas-filter-render-tests`、`snow-shot-screenshot-canvas-renderer-tests`、
`snow-shot-ocr-canvas-renderer-tests` / `snow-shot-ocr-line-box-rendering-tests`（"streaming replacement must
reconfigure only its glyph"）。

两个独立根因：worker 线程重建字形缓存时与 GUI 线程的字体数据库状态不一致；以及"替换文本时只重配字形、
丢弃缓存像素"这条路径在 Linux 字体回退链下没被走到。

先厘清一件事：`snow-canvas-text-draft-tests`、`snow-canvas-serial-number-render-tests`、
`snow-canvas-text-render-cache-tests` 是**同一个 exe 的三个 `add_test` 变体**
（`--serial-number-only` / `--text-render-cache-only` / 无参数），修好一个 exe 一次能消三条。

已经修掉的是序号模式那条 `serial rendering restores the caller's font state`：

```
FONT_PROBE base=Helvetica,12,-1,5,75,1,0,0,0,0 got=Helvetica,12,-1,5,75,1,0,0,0,0
FONT_KEY equal=1  base=Helvetica,12,-1,5,75,1,0,0,0,0 got=…
```

`toString()` 一模一样、`QFont::key()`（Qt 自己用来当字体缓存键的表示）也一模一样，
**只有 `operator==` 说不同**。原因是 Qt5 的 `QPainter::setFont()` 会把传入的字体解析进绘图设备的默认字体，
并丢掉 resolve mask；resolve mask 不在 `key()` 里，但参与 `operator==`。
Qt6 改成惰性解析，所以同一个断言在 Qt6 上成立、在 Qt5 上永远不成立。

断言改成"渲染意义上的同一个字体"——Qt5 比 `key()`，Qt6 保留精确 `==`：

```cpp
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
                    const bool restored = painter.font() == baseFont;
#else
                    const bool restored = painter.font().key() == baseFont.key();
#endif
```

这条和 9.7.1 的 `snow-canvas-image-tests` 是同一类：**断言写的是 Qt6 的契约，Qt5 上永远不可能成立**。
判据也统一了——先问"这个断言在 Qt6 上是否只是恰好成立"，再问"产品是否真的没还原"。

接着是那条 882 px 的 worker / GUI 字形缓存不一致。先要能看见它——本机 `-platform offscreen`
下这个 exe **一个字都不打就没了**（`QFontDatabase: Cannot find font directory`），连哪一步死的都不知道。
于是给 `main()` 加了两件 env-gated 的东西（都留在仓库里，因为下次还得用）：

```cpp
auto step = [trace](const char* name, void (*fn)()) {   // SNOW_TEST_TRACE=1
    if (trace) { std::fprintf(stderr, "STEP %s\n", name); std::fflush(stderr); }
    fn();
};
```

```cpp
// SNOW_TEST_TRACE=1 时挂上，崩溃前把出错模块的名字打出来
::SetUnhandledExceptionFilter(&reportFaultingModule);
```

第一次跑就得到 `STEP thread-fontdb` → `CRASH module=...\plugins\platforms\qoffscreend.dll`：
那个 882 px 的用例在本机是**过的**，本机崩溃点落在下一步（`textBackgroundUsesRectangleHatchTexture`），
而且崩在 **Qt 自己的 offscreen 插件**里（`QBrush` 纹理走 `QPixmap` 转换 → 插件内部 0xc0000005），
产品代码一行没参与。这条只影响本机 Windows + offscreen 的验证便利性，Linux 的 `qoffscreen.so`
是另一份实现，所以不加 skip、只记录。

真正的 882 px 是**时间基准问题**，不是产品问题：断言把"字体库变更之后"的 worker 重建，
拿去和"变更之前"的 GUI 快照比。字体库重置会从零重解析每个字族——在 fontconfig 宿主上等于重跑
一遍替换链，同一个 `QFont` 可能换个 face 或换套 hinting，**所有线程的光栅化同时平移**。
跨这个边界比，测的是那次重置，不是线程归属。改成同一世代内比：

```cpp
QImage oracle = expected;                 // 变更前的快照，只给 cold 那一半用
...
require(renderCacheDiagnosticsForCurrentThread().entries == 0,
        "font database changes must invalidate GUI layouts");
const QImage afterInvalidation = renderTextCacheSample(item, font, 1.0, true, false);
guiShift = differingPixelCount(afterInvalidation, expected);   // 失败消息里带上，供判读
oracle = afterInvalidation;               // 写完再放行 worker
fontsChanged.set_value();
```

worker 只在 `changed.wait()` 之后读 `oracle`，promise 自己提供 happens-before，不需要额外同步。
失败消息多一项 `gui-shift-across-reset=%13`：**它若等于 worker 的差值，就证明重置本身平移了整张图**；
若为 0 而 worker 仍然差，那才是 worker 侧真有问题（此时该查线程内的字体引擎，而不是夹具）。

本机（Windows + offscreen + 已装字体）改前改后 `thread-fontdb` 都是绿的，所以这一格只能靠 CI 判决——
这也正是把 `gui-shift-across-reset` 写进消息的原因：下一次 run 无论红绿都能定性。

剩下 3 个 `*canvas-renderer`（`-canvas-renderer` / `-ocr-canvas-renderer` / `-ocr-line-box-rendering`）
报的是同一句话 `a streaming replacement must reconfigure only its glyph and discard its cached
pixels before painting`——**三个 ctest 条目共用一个 exe**（`snow-shot-screenshot-canvas-renderer-tests`
加不同参数），所以只是同一个断言红了三次。本机 Windows + offscreen 全绿，属于真·Linux 差异，
但原来那句断言把三件事压在一起：

```cpp
require(geometrySync == textGeometry + 1 &&
            glyphBuilds == textBuilds &&
            glyphBytes < textBytes, "a streaming replacement must …");
```

红的时候没有任何信息能区分"几何没同步"、"重建了字形"、"缓存没被丢弃"。先拆成三条，并在失败时打数值：

```cpp
        if (geometryAfterReplacement != textGeometry + 1 || …) {
            std::cerr << "streaming replacement: geometry " << textGeometry << "->"
                      << geometryAfterReplacement << " builds " << textBuilds << "->"
                      << buildsAfterReplacement << " glyph-bytes " << textBytes << "->"
                      << bytesAfterReplacement << " direct-paints "
                      << layer.glyphDirectPaintCount() << '\n';
        }
```

`direct-paints` 是关键旁证：`glyphRasterBytes() < textBytes` 这条的**前提是这一行本来有缓存**。
一行若在 Linux 上走了直绘（带分数 DPR、或光栅超过缓存阈值），它从来没进过缓存，
`updateLineText()` 自然也没有像素可丢，于是字节数**相减为零**——这时断言红的是前提，不是产品。
一次 CI run 打出 `direct-paints` 与两个字节数就能定性，别再盲改。

**本机验证不到的两条，记在这里免得下次重复踩**：`snow-shot-scrolling-capture-settings-tests` 与
`snow-shot-screen-recording-controller-tests` 在本机 **Windows 上根本链接不通过**，且**早于本轮修改**
（`build-5tests.log` 里就有）：

```
snow-shot-scrolling-capture-settings-tests.exe : fatal error LNK1169
snow-shot-screen-recording-controller-tests.exe : fatal error LNK1120
snow_shot_settings.lib(unity_1_cxx.cxx.obj) : error LNK2005: … 已在 formfields.cpp.obj 中定义
windowchrome.cpp.obj : error LNK2005: snowTestFlushWindowComposition 已在 scrolling_capture_settings_tests.cpp.obj 中定义
```

`snow_shot_settings` 是 unity 构建（`snow_enable_unity_build`，`UNITY_BUILD_BATCH_SIZE 8`），而这两个测试
又各自单独编译了 `formfields.cpp` / `themecolorscheme.cpp`。MSVC 会把"成员被拉进来 + 同一符号另有一份
定义"当硬错误，GNU ld 对归档成员宽容，于是 **Linux CI 链接通过、Windows 本机过不了**——
仓库里没有 Windows 测试工作流，所以一直没人发现。这两条只能由 CI 判决。

#### 9.7.4b 光栅器的两处「Qt 6 精确、Qt 5 差一格」

`-magnifier-render`、`-filter-render`、`-serial-number-font`、`-magnifier-interaction` 这四条读下去，
全都栽在同一个模式上：**Qt 6 的像素/状态可以精确比较，Qt 5 不行**。三条本机（Windows + Qt 5.15 +
offscreen）就红，所以这次是本地改到绿，不靠 CI 猜。

**（1）放大镜渐变透明度**——`gradient opacity must increase from 16% to 42%`。

产品的 stops 是 `0.08 / 0.27`（`snow_canvas_magnifier_renderer.cpp`，`5d0b3a28` 从 `0.04 / 0.16` 提上来的），
测试期望却是 `0.16 + 0.26 * f`。实测本机同一行三点的 alpha 是 **23 / 29 / 34**，
代入 stop 公式 `255 * (0.08 + 0.19f) * 0.7` 完全吻合——**Qt 5 只画一遍**，
Qt 6 那条路径把同一块 strip 合成两遍（这正是"复用合成裁剪"那次改动的效果），
所以那边看到的是接近翻倍的 41 / 47 / 52，测试就照那个写了。

按版本分开写，两边都保留断言：

```cpp
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
                const int expected = qRound(stroke.a * (0.16 + 0.26 * fraction) * 0.7 * callerOpacity);
#else
                // 产品写的是 8% -> 27%；Qt 6 的两遍合成把它变成 16% -> 42%。
                const int expected = qRound(stroke.a * (0.08 + 0.19 * fraction) * 0.7 * callerOpacity);
#endif
```

失败消息里带上整行采样值与 `direct-paints` 计数，下一次不必再反推。**渐变本身不是产品缺陷**，
是同一个 stops 在两个 Qt 里的合成次数不同；文档 §6 里"连接线在 Qt5 上偏淡"这条可以据此收紧。

**（2）分块采样的定点相位**——`tile origins and coverage clips must preserve canonical sampling`。

原代码认为"整数步长可以和整图逐像素相等，只有分数步长才会把 RGB 修约一位"。Qt 6 成立，
Qt 5 不成立：同一块区域换个 tile 原点重画，覆盖度会被重新折进每个通道，
实测 `#ac6449` vs `#ad6349`、alpha `255` vs `254`——**RGB 和 alpha 都差一格**。

```cpp
bool sameTileSample(const QColor& left, const QColor& right) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return left == right;
#else
    return qAbs(left.alpha() - right.alpha()) <= 1 && qAbs(left.red() - right.red()) <= 1 &&
           qAbs(left.green() - right.green()) <= 1 && qAbs(left.blue() - right.blue()) <= 1;
#endif
}
```

**（3）背景渲染器看到的字体**——`offscreen serial scenes must preserve the incoming font …`。

失败时才看得出来：`toString()`、`key()`、`renderHints()` 全相等，`operator==` 却是 false。
原因是 **Qt 5 的 `setFont()` 会拿绘制设备的默认字体做一次解析并丢掉 resolve mask**，
两个"可见属性完全一致"的 `QFont` 因此不相等。判据换成 `key()`（Qt 自己比较字体用的就是它），
和 `snow-canvas-text-draft-tests` 里那条串号字体断言保持一致：

```cpp
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        const bool sameFont = actual == expectedFont;
#else
        const bool sameFont = actual.key() == expectedFont.key();
#endif
```

**（4）兄弟滤镜的"损伤紧不紧"** ——`sibling filters cannot consume or propagate …`。

像素是对的（`changedPixels == 0`），红的是"损伤区域不该盖住另一条滤镜"这条**紧致性**断言。
实测 Qt 5 交给 `QPaintEvent` 的是更新区域的**包围盒**（一个覆盖两块模糊的矩形），
不是区域本身；Qt 6 才给精确区域。于是：Qt 6 继续要求 `painted` 与兄弟滤镜区域不相交，
Qt 5 只断言像素不变，并把理由写进注释——平台看不见的东西不能当断言。

#### 9.7.4c 按钮的「owner 可以在激活进行中退休它」

`adqt-button-signal-lifetime-tests` 本机（Windows + Qt 5.15 + offscreen）直接 `EXIT=139`，
不用等 CI。四条同时崩：

| 用例 | 触发器 |
| --- | --- |
| `activationAllowsOwnerDeletion` | `AdSwitch` + `toggled` |
| `switchDragAllowsOwnerDeletion` | `released` / `toggled` / `clicked` |
| `radioGroupSelectionAllowsOwnerDeletion` | `Radio::toggled` 与 `checkedIdChanged` |
| `radioGroupNavigationAllowsOwnerDeletion` | 方向键改变选中后 `checkedIdChanged` |

**怎么定位的**：先加 `qInfo()` 探针，一个字都没打出来——QTest 的日志通道把输出吞了。
换成 `std::fprintf(stderr, ...)` + `std::fflush(stderr)` 才看见 `kind=0 signal=0 keyboard=0`，
再逐用例二分，确认是上面四条而不是整批。

**根因是附录 C.1**：Qt 5.15 的 `QAbstractButtonPrivate::click()` 在 `nextCheckState()` 返回后
仍在跑（`refresh()` 等），所有者从 `toggled` 里 `delete` 按钮，Qt 内部就去读已释放的私有对象。
Qt 6 有守卫。**widget 侧没有可防守的位置**——它在 Qt 的私有函数里被删掉的，自己无从插手。

**为什么不用 `#if QT_VERSION` 把用例跳过**：这条用例的名字就是契约本身——
「owner 可以在激活进行中退休这个 widget」。跳掉等于把契约从门禁里删掉，
而且 Qt 5 下真实 owner 也常常真这么干。真正能对两版同时兑现的做法是**把删除推迟到事件循环**：

```cpp
// Qt 5 defers the deletion to the event loop, Qt 6 keeps deleting it immediately.
template <typename T>
void retireOwner(QPointer<T>* object) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  delete object->data();
#else
  if (*object) {
    (*object)->deleteLater();
  }
#endif
}

void settleRetiredOwner() { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }
```

`settleRetiredOwner()` 必须显式写：§9.6.58 已经记过 `processEvents()` 不排空 `DeferredDelete`，
这里 `sendMouse()` 内部也没有。

**别顺手给所有用例都补 `settleRetiredOwner()`**：`radioGroupPropertyPropagationAllowsOwnerDeletion`
走的是 `controlSizeChanged` / `variantChanged` / `componentTokensChanged` 这类**属性**信号，
不经过 `click()`，立即 `delete` 一直就是安全的（它本来就没红）。加了不但没用，
还会让人误以为那里也有同样的坑。

**与附录 C.1 里 `adqt-value-widget-lifetime-tests` 的差别**：那一组断言的是「`delete` 必须安全」，
所以产品侧动过刀（`QSignalBlocker` + `QMetaObject::invokeMethod` 补发）；这一组断言的是
「owner 可以退休」，两种实现都满足契约，选延迟的那条更贴近真实 owner 的写法。

#### 9.7.5 平台能力缺失（`offscreen` 无托盘 / 无真实桌面）

属于这一组的六条：`snow-shot-system-tray-controller-tests`、
`snow-shot-recording-export-tray-tests`、`snow-shot-direct-capture-native-tests`、
`snow-shot-scrolling-capture-settings-tests`、`snow-shot-recording-capture-exclusion-tests`、
`adqt-widget-diagnostics-tests`。

**先修正 §9.6 那条判据**：`exit(77)` 只适用于"环境真的没有这个东西"（多屏、字体目录、托盘宿主）。
这一组里只有窗口激活/托盘宿主沾边，其余四条的真实原因是**断言把 Windows 当成唯一实现**——
平台缺一个 API 时产品换了一条**正确的**行为，测试却仍按 Windows 的期望断言。把它们 skip 掉，
等于把移植里唯一真正需要决策的地方（缺能力时降级成什么）从门禁里删掉。逐条：

**（1）托盘通知**——`snow-shot-system-tray-controller-tests` 报
`a disabled tray must not replace the last balloon with an update notice`，
`-recording-export-tray-tests` 报 `disabled legacy tray notifications cannot open files`。

产品侧本来就分了平台：`systemtraycontroller.cpp` 里 `showBalloon()` 把"写 `lastBalloon*` 属性"
这一段包在 `#if !defined(Q_OS_MACOS) && !defined(Q_OS_LINUX)` 内，`activateMessage()` 的
`if (!enabled) return;` 同样只对 Windows 生效；Linux 走
`platform::linux::SystemNotificationService`（session bus 上的 `org.freedesktop.Notifications`），
与托盘图标是否可见无关。测试的 `#else`（Windows）分支却把"托盘关了就该什么都不发生"当成契约，
于是 Linux 上 `openedPaths` 变成 3 条而不是 2 条。

修法是把 macOS 的语义扩展到 Linux，而不是加 skip：

```cpp
#if defined(Q_OS_MACOS) || defined(Q_OS_LINUX)
        require(openedPaths == QStringList{first, second, first, second},
                "hidden tray icons must not disable new or previously delivered native actions");
#else
        require(openedPaths.size() == 2, "disabled legacy tray notifications cannot open files");
#endif
```

两个服务在各自命名空间里，用别名抹平：`using NotificationService = platform::macos::…` /
`platform::linux::…`，`activated(Action::OpenRecording, path)` 签名一致。
`findChild<NotificationService*>()` 也因此两边通用。

有一处**只留 macOS**：`result.status == Unavailable`。macOS 的测试二进制没有 bundle，
原生路径必然报不可用；Linux 上"有没有守护进程"是宿主的属性——裸会话报 `Unavailable`、
桌面会话真投递，两者都是**投递了一次**，所以只计数不校验状态。

计数还不能用单次 `processEvents()`：macOS 的授权结果排队、Linux 走 D-Bus 回包，
都会晚于 `show()` 返回。加一个自旋到期望值的等待：

```cpp
        const auto waitForAttempts = [&](int expected) {
            QElapsedTimer clock;
            clock.start();
            while (notificationAttempts < expected && clock.elapsed() < 5000)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        };
```

**（2）直捕**——`snow-shot-direct-capture-native-tests` 报
`direct capture lost the full desktop snapshot`。

这条不是"环境没桌面"：fixture 是真的，`snow_shot_alias_test_rust_ffi()` 把 C ABI 符号改名成
`snow_test_*`，`directcapturenative.cpp` 和目标文件一起被改名，桩确实生效。差的是 X11 的三条契约：

1. **窗口没有自己的采集**。`directcapturenative.cpp` 在 `#ifdef Q_OS_LINUX` 里要求
   `request.windowFrame` 非空，否则直接返回空结果——窗口矩形得随请求传进来（由平台层读）。
   测试从来没设过它，于是 FocusedWindow 全部空手而归。
2. **一次性 native 目标只有显示器**。Linux 的 `oneShot` 多一个
   `target == DirectCaptureTarget::CurrentMonitor` 条件：窗口必须走桌面会话再裁。
   于是 `sessionCreations / desktopCaptures / targetCaptures` 三个计数器在窗口目标上**角色互换**。
3. **`admitRaster` 只挂在一次性路径上**。窗口在 Linux 上不经过它，预算用例要改成打显示器，
   否则 `admissionCalls` 恒 0。

断言改写后的 X11 契约（跨两台 head 的窗口）：

```cpp
        request.windowFrame = QRect(-1, -1, 2, 2);   // 左右各占一列
        …
        require(capturedWindow == 0 &&                              // X11 不传 HWND
                    frame.physicalBounds == QRect(-1, -1, 2, 2) &&
                    frame.image.pixelColor(0, 0).red() == 40 &&      // 来自 head A
                    frame.image.pixelColor(1, 0).red() == 70 &&      // 来自 head B
                    frame.image.pixelColor(1, 1).alpha() == 255,     // 根窗口无 alpha
                "window image must be cropped from every captured display");
```

另外两个 `fail*` 开关语义也跟着变：`failWindow` 在 X11 上没有杠杆（没有原生窗口读要失败），
要跳过；`failSecondDisplay` 则**反转**——少一台显示器就是半个窗口，必须报失败而不是返回半张图：

```cpp
            require(!captureDirectTarget(request).isValid(),
                    "a missing display must fail a cropped window capture");
```

**（3）滚动截屏设置**——`snow-shot-scrolling-capture-settings-tests` 报
`initial capture must exclude the interface`。

`excludeScrollingWindowsFromCapture()` 整块被 `#if defined(Q_OS_WIN) || defined(_WIN32) || defined(Q_OS_MACOS)`
包住，Linux 分支只有 `Q_UNUSED(overlay)`，所以 `excludedIds` 恒空、`excludedWindows` 永不入栈。
用 `constexpr` 把能力写出来，断言两边都保持有意义：

```cpp
#if defined(Q_OS_WIN) || defined(_WIN32) || defined(Q_OS_MACOS)
    constexpr bool kNativeCaptureExclusion = true;
#else
    constexpr bool kNativeCaptureExclusion = false;
#endif
    require(requests.size() == 1 &&
                requests.back().excludedIds.isEmpty() == !kNativeCaptureExclusion,
            "initial capture must exclude the interface wherever that is possible");
```

重采集处同样加短路，否则 Linux 上 `until()` 会一直等到超时：

```cpp
        until([&] { return !kNativeCaptureExclusion ||
                       (captureInterface ? excludedWindows.isEmpty() : !excludedWindows.isEmpty()); },
              "recapture must also reapply the native window sharing policy");
```

**（4）录制排除**——`snow-shot-recording-capture-exclusion-tests` 报
`unavailable native exclusion keeps drawing popup hidden; capture-on shows it`。

`guardToolbarSurface()` 的 Linux 分支是 `Q_UNUSED(surface)` 后返回 `true`：
没有 API 可调**不等于**调用失败，把弹窗藏起来只会白白拿走用户一个控件；
代价是工具栏进画面，这条降级已在 §6 与附录 C 记录。所以断言应为"恒可见"：

```cpp
#else
                require(drawingPopover->surfaceWidget()->isVisible(),
                        "a platform without native exclusion must not hide the drawing popup");
#endif
```

注意 Windows/offscreen 那半边的语义恰恰相反（排除失败 → 藏起来，别让弹窗漏进录制），
两边都留，别合成一个断言。

**（5）`adqt-widget-diagnostics-tests`** 单独说清楚，因为它**不是环境问题、也不是 Linux 问题**：
本机 Windows Qt5 一样红，报的是

```
menu publication diagnostic: stage=setModel mode=2 tree=… updates=0
  expectedRows=2 publishedRows=-1 modelRows=2 visible=0 hidden=1
```

`updates=0` 是关键——**Qt5 对这套菜单根本不发任何 `QAccessible` 更新**（Inline / Vertical / Horizontal
三种模式全为 0，之前看起来"前两种能过"只是 `publishedRows` 里残留了别的对象）。而 Qt6 会发，
所以这条断言在 Qt6 上成立、在 Qt5 上永远不成立，属于 §9.6 反复出现的那一类"断言写的是 Qt6 契约"。

修法：把"发布"和"数据到位"拆开。Qt6 仍然要求 published 计数精确等于期望；Qt5 只要求
**视图确实拿到了行**（`tree->model()->rowCount() == expected`），并且在 Qt5 万一真发布了时校验其数值：

```cpp
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  const bool ok = published == expected;
#else
  // published < 0 表示 Qt5 压根没发布，此时只能校验数据是否到手
  const bool ok = (published < 0 || published == expected) && modelRows == expected;
#endif
```

诊断输出同时改成**只在真失败时打印**：原来"Qt5 没发布"也会打一行，一次运行刷出四十多行，
把真正的失败淹掉了——被豁免的分支不该出声。

**（6）Print Screen 录制**——`snow-shot-shortcut-key-row-tests` 报
`a new recording session must save the new physical position`。

`ShortcutRecorder` 的原生实现只有 Windows（`PrintScreenShortcutRecorder`）：非 Windows 的
`handleKeyEvent()` 直接 `return false`，不产生任何事件。而 `ShortcutPlatform` 枚举只有 `MacOS`
一个成员，所以 Linux 上的绑定**不可能**带物理键位置。测试的 `#else` 分支是照着 macOS 写的
（失败消息自己都写着 "native macOS recording"），Linux 跟着掉进去，最后一条
`physicalKeys.value(MacOS, 128) == 8` 必然失败——`128` 正是 `macVirtualKeyForBinding()`
在找不到映射时的兜底值。

前半段对两个平台都成立（裸 release 不记录、无关 release 不覆盖已记录的 Ctrl+A、Cancel 丢弃），
所以只把末尾一条分叉：

```cpp
#if defined(Q_OS_MACOS)
    require(portableText(session.saved) == QStringList{QStringLiteral("Ctrl+C")} &&
                session.saved.first().physicalKeys.value(shortcut_domain::ShortcutPlatform::MacOS,
                                                         128) == 8,
            "a new recording session must save the new physical position");
#else
    require(portableText(session.saved) == QStringList{QStringLiteral("Ctrl+C")} &&
                session.saved.first().physicalKeys.isEmpty(),
            "a recording without native capture must save the combination itself");
#endif
```

括号里那条 `isEmpty()` 是**有意义的**：它把"这个平台没有物理位置"写成契约。
将来谁在 Linux 上补了物理键表，这条会失败并逼他改断言，而不是悄悄放行。

#### 9.7.5b 这一轮里真正的产品缺陷（不是断言问题）

三条里没有一条是"测试写错了"，都是产品在 Linux / 发行版配置下真的做错了事。
它们此前被更早的失败挡住，看不出来。

**（1）发行版配置里根本没有 ICO 编解码器**

`SNOW_IMAGE_PROFILE=redistributable`（打包预设全用它）走根 `CMakeLists.txt` 里那张显式
codec 清单：

```cmake
elseif(SNOW_APPS_BUILD_SNOW_SHOT)
    set(_snow_workspace_image_codecs PNG JPEG GIF HEIF JXL WEBP)   # 漏了 ICON
```

漏了 `ICON`，`snow_image` 就不编 `icon_codec.cpp`，而托盘**不走** Qt 的 `imageformats`
插件——它把 ICO/CUR 字节交给 `snow_shot_image_codec_backend`。于是自定义托盘图标在发行版里
一律得到 `The icon directory is invalid.`，静默退回内置默认图标。

同一条设置还在 `snow_shot/CMakeLists.txt` 里写过一份，但那一整块被
`if(NOT TARGET snow_image::snow_image)` 包着，而根 `CMakeLists.txt` 第 266 行早就
`add_subdirectory(snow_image)` 了 ⇒ **整块是死代码**，连它旁边原有的 PNG/JPEG/WEBP 三行
FORCE 也从没生效过。判据不能看"这里写了 ON"，要看 `CMakeCache.txt` 里
`SNOW_IMAGE_CODEC_ICON:BOOL` 和 `build.ninja` 里有没有 `icon_codec`：

```
$ grep -E "^SNOW_IMAGE_CODEC_ICON" build/windows-qt5-tests/CMakeCache.txt
SNOW_IMAGE_CODEC_ICON:BOOL=ON
$ grep -c icon_codec build/windows-qt5-tests/build.ninja
2
```

**（2）X11 上"关掉历史"的窗口直捕根本没有采集路径**

`captureDirectTarget()` 只在 `historyEnabled` 时才建 `SnowCaptureDesktopSession`，因为在
Windows/macOS 上 session 是"连续刷新的历史缓冲"，一次性抓取走
`snow_capture_target_screenshot()`。

X11 两者都没有：一次性 API 只能开**显示器**，窗口是从桌面采集里裁出来的，而桌面采集**必须有
session**。于是历史关闭 + 窗口目标 ⇒ `snapshot` 恒为 `nullptr`，返回空帧加一句
`snow_capture_last_error_message()` 的残留文本。历史不是开 session 的理由，它只是**唯一**的调用者：

```cpp
#ifdef Q_OS_LINUX
    const bool needsDesktopSession =
        request.historyEnabled || request.target == DirectCaptureTarget::FocusedWindow;
#else
    const bool needsDesktopSession = request.historyEnabled;
#endif
```

一次性捷径的条件（显示器目标 + 历史关闭）不动，其它行为不变。

**（3）空文档的固定窗口导出，投影比画布少了一步**

`renderPinnedViewport()` 对"文档里没有任何图元"的固定窗口导出自己开画布、自己
`drawImage`，注释写着"Match the canvas export surface and painter hints"。但画布那侧还做了
一件事：给输出设一个临时 `devicePixelRatio`（= 导出倍率），再把画笔按它的倒数缩放。这一步
数学上是恒等，却改变了 Qt 光栅器**采样相位**——37x19 放大到 74x38 时，右/下边缘每个像素差一档
（实测 `#fcb40090` 对 `#fbb4008f`）。

修法不是让手写那份去模仿，而是**消灭第二份实现**：导出面本来就有"没有 runtime 时只做投影"
的分支（`renderToImage()` 在场景同步不上时就是那么走的），把它暴露出来：

```cpp
// snow_canvas_runtime.h
static QImage projectSourcesToImage(const QRectF& canvasRect, const QSize& outputSize,
                                    const QList<CanvasExportSource>& sources);
```

`snow_shot` 的空文档分支改调它，两条路径从此是同一段代码，而不是两个"应当相等"的实现。

**（4）顺带：测试算参考值时绕过了产品漏斗**

`screenshot_export_artifact_tests` 的 sRGB 参考值是
`image.copy().convertedToColorSpace(SRgb).convertToFormat(RGBA8888)`。源是 `Format_Mono` 时，
Qt5 会在索引布局里"展开→变换→重新量化"，小图干脆不转（§9.6.60），于是它和行源比的根本不是
裁剪，而是**调色板量化**。参考值改成先展开到 32 位再转（与 `snowImageConvertedToColorSpace()`
同一条路），Qt6 上两种写法逐像素等价，所以不需要 `#if`。

**本机 Windows + Qt5 已知差异（不是 Linux 问题）**：同一套件里
`lazyResizeRowsAndRegionsPreserveFullImagePixels()` 的"copied row and ROI callbacks share a
synchronized bounded conversion reader"在本机偶发失败、偶发 `EXIT=139`（`Format_RGBA64_Premultiplied`，
共享的 `BoundedSrgbRowReader` 两道线程），而 Linux 的顺序里它**先于**空文档用例执行而且从没红过
（arm64 日志的首条失败就是空文档那条）。疑点在 Qt5 `QColorSpace` 转换缓存的线程安全，属于本机
Harness 噪声，先记录不追。

#### 9.7.5c 托盘三条断言：平台读法而不是平台能力

`system-tray-controller` / `recording-export-tray` 两个 ctest 条目其实是**同一个 exe**
（`--recording-export-only`），所以一处改两处生效。

**（1）通知点击被算了两遍**

导出完成的通知流程里有一句裸的 `trayIcon->messageClicked()`，它落在"托盘已隐藏 → 点旧气泡
不该打开文件"的段落中间。三平台语义不同：

| | `messageClicked` 是否连到 `activateMessage` | 那次点击的含义 |
| --- | --- | --- |
| Windows | 是 | 就是被测行为：隐藏的托盘不能再打开文件 |
| macOS | **否**（`#ifndef Q_OS_MACOS` 才连） | 空操作 |
| Linux | 是 | 第二条通道，而紧接着的两条 `notifications->activated(...)` 才是原生通知的点击 |

Linux 上两条通道同时活着，同一份导出被打开两次，`openedPaths` 只可能对不上。改法是把它放回
它本来测的那个平台：

```cpp
#if !defined(Q_OS_MACOS) && !defined(Q_OS_LINUX)
        trayIcon->messageClicked();
#endif
```

**（2）"没有活动窗口"在无 WM 的环境里不成立**

`QApplication::activeWindow() == nullptr` 在 `offscreen` 下永远为假：没有窗口管理器在最后一个
对话框关闭时清除 Qt 的焦点窗口，实测拿到的是一个 `visible=0` 的裸 `QWidget`。这属于
"断言环境而非产品"第二类（runner）。能对两版都成立的说法是**没有可见窗口持有焦点**：

```cpp
    const QWidget* activeWindow = QApplication::activeWindow();
    require(activeWindow == nullptr || !activeWindow->isVisible(),
            "the tray-only state should have no active window");
```

留着的部分才是契约：真有对话框没关掉（`isVisible()` 为真）照样红。

**（3）ICO 前置断言永远不可能成立**

那一段原本在用到处才 `QCoreApplication::setLibraryPaths({})`，然后断言
`!QImageReader::supportedImageFormats().contains("ico")`。Qt5 的插件扫描是**第一次查询时就定型**，
只要那次扫到了东西就不再重扫，所以到这一步 ICO 插件早已在缓存里——断言必假，而 `require`
会 `exit(1)`，整套通知断言一条都跑不到。

唯一的时机窗口是"进程还没解码过任何图像之前"。于是把清空提到 `main()` 里 `QApplication`
构造之后、`verifyTrayMenuPosition()` 之前，ICO 段只负责恢复；原本紧跟其后的 JPEG 方向环
（需要 jpeg 插件）挪到恢复之后。中间那段不需要任何非内置格式（夹具是 PNG，内置托盘图标也是 PNG）。

#### 9.7.5d 2026-11-05 这一批：一条产品缺陷 + 七条「断言写的是另一条管线」

**（1）调色板源导出偏色（真产品缺陷）**

`ImageSplitExport` 把源图 `convertToFormat(ARGB32)` 之后才转色彩空间。索引/单色源（`depth() <= 8`）
在 Qt5 上是「展开 → 变换 → 重新量化回同一批 256（或 2）个 palette 项」，小图干脆不转（§9.6.60）：
索引 DisplayP3 源导出 1517 像素里 635 个偏色，最深 0x0f。修法是先展开再变换，且**展开会丢
profile**，必须把 `colorSpace()` 取出来再 `setColorSpace()` 复原：

```cpp
if (!converted.isNull() && converted.depth() <= 8) {
    const auto space = converted.colorSpace();
    converted = converted.convertToFormat(QImage::Format_ARGB32);
    if (!converted.isNull())
        converted.setColorSpace(space);
}
```

测试侧 `normalizedForExport()` 是同一件事的参考实现，必须同步展开，否则它拿来比的还是原始
palette 项（§9.7.5b(4)）。

**（2）`lrelease` 会丢弃「译文与源文相同」的条目**

en_US 目录里只留下英文真正覆盖的少数串。`QTranslator::translate()` 对缺失条目返回**空 QString**，
而应用侧 `tr()` 会回退到源文——测试若直接拿 `translate()` 的返回值比，期望就变成空串，
`QString::arg` 还会顺手报 "Argument missing"。复现回退时必须用 `fromUtf8`：
About 文案里有 `·` 和 `©`，`fromLatin1` 会把两字节拆成 `Â…`，打印出来一模一样，只有比较才暴露。

**（3）QPainter 的「正路径」与「反向空洞路径」AA 差一级**

`selection-background-preview`（= `screenshot-canvas-renderer-tests --background-opacity-only`）
在 dpr=1.3 的一个像素上差 3 档：`#8594a7` 对 `#8292a6`，本机与 CI **同点同值**，所以既不是平台差
也不是随机。原因是两条管线填的是互补路径：预览用正路径填覆盖度（`maskSelectionPreview`、
`maskBackgroundForSelection` 都填 `selectionPath`），导出合成器 `compose()` 的 `roundedHole()`
填的是「矩形减圆角」的反向空洞。Qt 光栅器对互补填充的覆盖度量化可以差一级，而 50% 背景叠在
阴影上正好把这一级放大成 3 档。

能写死的判据是按覆盖度分层，而不是整体放宽容差：

```cpp
const int tolerance = coverage > 0 && coverage < 255 ? 3 : 2;
```

测试自己用**同一条圆角路径**（`selected` + `cornerRadius * dpr`）填一张 `Format_Alpha8`
覆盖度图来判边。整帧 22 万像素里只有这一个像素越界，放宽到 3 之后其余仍是 2 的严格契约。

**（4）Qt5 绝不写窗口 opacity，所以那个 fixture 是「不可能状态」**

Qt5 的 windows 插件把 `setWindowOpacity()` 实现成 `SetLayeredWindowAttributes`（常量 alpha），
与 `WA_TranslucentBackground` 的逐像素 alpha 混用后窗口卡在常量 alpha 模式，再
`setWindowOpacity(1.0)` 也拿不回来（§9.7.5 附近的说明）。所以 `shouldConcealFirstPaint()` 在 Qt5
恒 `false`、`setConcealWindowOpacity()` 是空函数。

`frame-paced warmed reveal` 的 fixture 自己 `setWindowOpacity(0.0)` 来模拟遮盖，然后要求
presenter 恢复成 1.0——这在 Qt5 上描述的是产品**拒绝制造**的状态，永远不可能成立。改成断言
产品真正的不变量：

```cpp
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        require(qAbs(window.windowOpacity() - 1.0) < 0.001,
                "frame-paced warmed reveal must restore full window opacity");
#else
        require(qFuzzyCompare(window.windowOpacity(), fixtureOpacity),
                "a Qt5 reveal must leave the window opacity untouched");
#endif
```

**（5）分数 DPR 下「子矩形重绘 == 整帧重绘」不能逐像素要求相等**

`image-split-canvas` 把上报的 damage 子矩形渲染到上一帧副本上，再和整帧比较。dpr=1.5 时
damage 的边界列在设备空间取整，合法地差一档（实测 `x=233`）。可断言的不变量是「所有变化像素
都落在上报 damage 的设备空间覆盖内」：

```cpp
const auto bounds = damage.boundingRect();
const QRect allowed(QPoint(qFloor(bounds.left() * scale) - 1, qFloor(bounds.top() * scale) - 1),
                    QPoint(qCeil((bounds.left() + bounds.width()) * scale),
                           qCeil((bounds.top() + bounds.height()) * scale)));
require(allowed.contains(changed), "...");
```

变化框要按行 `memcmp` 先筛掉相同行，否则 90 次逐像素扫描把用例拖到 77 秒（改后 68 秒）。

**（6）高斯滤镜逐 tile 重采样：旋转字形的 alpha 容差**

`layout-compaction` 的 `batched pixels must retain source samples and exact coverage`：
`decoration=3 scale=2` 时 `alphaErrors=1200`、最大差 1。高斯滤镜每个 tile 带自己的 halo 重采样，
裁剪过的字形跨度落点会和整体渲染差一级。判据改成比**最大 alpha 差**并给逐级容差
（`scale==1.25 → 5`，`decoration==2 → 2`，`decoration==3 → 1`），其余integral/半倍采样仍是精确相等。

**（7）两条「数字写死在注释里」的计数断言**

- `settings-color-commit` 的 `cover all ten settings color pickers`：登记表早已 15 个颜色字段。
  改成从 registry 推导（`SettingsFieldKind::Color` 计数），顺带把 ctest `TIMEOUT 30` 抬到 120
  （本机 50.6 秒）。
- `configuration-archive` 的 `only settings that actually differ must announce changes`：期望集合
  改成按 `applySnapshot()` 前后**真实快照 diff** 推导。materialize 会修复重复默认快捷键
  （`pin_to_screen_shortcuts/toggle_playback` 与 `drawing_mode` 都是 `Space`），那是真实值变化，
  不是多余公告。

#### 9.7.5e 阴影 tile 接缝被合成两次（真产品缺陷，两个平台都有）

`region-shadow-renderer` 报 `dense tiled union and hole dpr=1.000000 transform=1: maximum
premultiplied channel difference=34 ... mismatch count=12 bounds=47,473,1041,1`，本机与 CI 逐位相同。

先把 12 个越界像素打出来，形态就很清楚了：同一行上有左右两组对称的 6 像素斜坡，`actual` 恰好是
`expected` 的「合成两次」——`a + a*(1-a)` 而不是 `a`（40→74、28→53、18→35、10→20、5→10、2→4、
1→2，逐个对得上）。所以不是掩膜算错，是同一块阴影被 `DestinationOver` 叠了两遍。

为什么会叠两遍：那一行的 `d = 473.17` 正好是 tile 网格接缝（图 512 像素）映射到设备空间的位置，
而 Qt5 光栅器把 clip 矩形**向外取整**。用一个 20 行探针程序就能直接量出来：

```
seam=473.1700
top    first=0 last=473      // 上 tile 的 clip.bottom=512 → ceil(473.17)-1 = 473
bottom first=473 last=599    // 下 tile 的 clip.top=512   → floor(473.17) = 473
```

设备行 473 被两个 tile 同时覆盖。中间为什么不显形：那一行大部分像素在选区内部，阴影 alpha 本来就是
0，只有左右两侧的阴影斜坡（选区边界）非零——于是表现为「一行的两条边变黑一倍」。

修法不是放大容差，而是让两侧的 clip 边落在**同一个整设备坐标**上，各自再偏四分之一像素进自己这
侧（抵消浮点噪声，保证 `floor(int+0.25)` 与 `ceil(int-0.25)-1` 恰好不重叠也不留缝）：

```cpp
QRectF snappedInternalClip(const QRectF& clip, const QRect& core, const QTransform& toDevice) {
    ...
    if (qFuzzyCompare(clip.left(), frame.left()))
        snapped.setLeft(pin(clip.left(), true, 1.0));    // 本 tile 从整列开始
    if (qFuzzyCompare(clip.right(), frame.right()))
        snapped.setRight(pin(clip.right(), true, -1.0)); // 本 tile 到整列之前结束
```

只钉「内部」边（`clip` 仍等于 `core` 的那几条），四周的 perimeter 边保持原来的 guard 外扩——那条注释
说的是对的：整图边缘的抗锯齿覆盖必须由原始整图提供。tile 循环与 strip 循环两处都要钉，否则换一个
缩放比就走回老路。

**（8）Qt5 的字体引擎缓存是**每线程**的，`fontDatabaseChanged` 只清 GUI 线程**

`snow-canvas-text-draft` / `text-render-cache` 的
`preparedTextLayoutsStayOnTheirThreadAndFollowFontDatabaseChanges`：worker 线程在字体库重置后
重建的栅格和 GUI 线程重建的差 882 像素，其中 `both-inked=879`、两侧独占各 1/2 ——**布局一样，只有
强度不同**。原因不是 worker 缓存没失效（`builds=3 hits=0` 都成立），而是 Qt5 的 `QFontCache`
走 `QThreadStorage`：GUI 线程的 `fontDatabaseChanged` 清不掉 worker 线程的引擎缓存，于是 worker
用重置前的 hinting 继续出图。

这条差异属于「重置」而不是「线程亲和性」——用例自己的注释也是这么写的。于是 Linux 上只保留真正
可判定的部分（重置前 worker 与 GUI 必须逐像素相同、重置后必须重建三个 phase 而不是命中缓存），
把跨重置的逐像素相等留给引擎按线程无关方式缓存的平台：

```cpp
#if defined(Q_OS_LINUX)
    if (!observed.rebuiltMatches) { ...只打印诊断... }
#else
    require(observed.rebuiltMatches, ...);
#endif
```

#### 9.7.5f 2026-11-06：一条 Qt6-only 诊断把整个测试步骤吞了

上一轮为了定位 `empty document fast path` 的差一档，在
`screenshot_export_artifact_tests.cpp` 里加了

```cpp
rendered.colorSpace().description()
```

`QColorSpace::description()` 是 Qt 6.8 才有的成员。Linux 侧编译直接失败：

```
error: 'class QColorSpace' has no member named 'description'
FAILED: snow_shot/CMakeFiles/snow-shot-export-artifact-tests.dir/...
```

ninja 在第一个失败后停止调度，于是 **`Unit tests` 这个步骤根本没跑**，CI 报的
`27 tests failed` 里一条测试结果都没有——整轮只有这一个编译错误。也就是说
「修好一条断言」和「CI 变绿」之间隔着一层编译门禁，而门禁外的红没有任何信息量。

本机没暴露它的原因不是编译器宽容，而是 **mtime 陷阱**：那个 TU 上一次编译成功后
时间戳比源码新，ninja 认为它是最新的，改动从未进过 `cl.exe`（同一轮里
`collage-document` 也栽在这上面，换新二进制后直接通过）。

规则：**诊断代码同样受「Qt5 可用面」约束**。要标识一个 `QColorSpace`，用两代都有的
结构量：

```cpp
qDebug() << int(space.primaries()) << int(space.transferFunction()) << int(image.format());
```

`description()`（6.8）、`colorModel()`（6.0）、`toIccProfile()`（6.0，Qt5 叫
`iccProfile()`）三个全部不可用。sRGB 与 Display P3 的 `primaries()` 不同，而断言
比较的正是这个，所以结构量足够定位。

顺带一条：**改完源码先 `touch`**。`cmake --build` 静默返回 0 不代表这个 TU 被重编过；
删过 `.pdb`/`.ilk` 之后尤其要确认链接真的发生了（日志里应有 `Linking CXX executable`）。

#### 9.7.5g 断言链的滚雪球：一次 CI 只能暴露每条用例的第一处失败

`require()` 是 `std::_Exit(EXIT_FAILURE)`，用例在第一条不成立的断言处终止。于是
「修好 A」之后，同一个用例里的 B 成为下一条红——这一轮有四处都是这样：

| 用例 | 修前第一条红 | 修后暴露的第二条 |
|---|---|---|
| `image-split-editor` | damage 覆盖度 | 空态拖放路径（`C:/split.png`） |
| `cloud-upload` | destination 同步 | 三行全宽列（同一根因：异步接受值） |
| `screen-recording-shortcut` | 重复点 Select | 重复按绘图快捷键 |
| `settings-navigation` | 按钮按文本定尺寸 | （本机 70 s，超时 60 s） |

所以「CI 从 38 掉到 27」不等于「修好了 11 条」，其中若干只是把红挪到了下一行。
判断进展要看**同一用例的断言行号是否前移**，而不是看总数。

#### 9.7.5h 共享缓存总量证明不了「某一行被丢弃」

`ScreenshotOcrTextLayer::glyphRasterBytes()` 是整个共享缓存的净额。断言
`bytesAfterReplacement < textBytes` 想说的是「被替换的那一行丢掉了它的栅格」，但
共享净额无法区分是哪一行。Linux 上这一行本来就走**直绘**
（`paintRaster()` 里 `directClip != cachedClip` 的取整检查没过，Qt5 的 clip 向外取整
与缓存中间面的整数平移不同），所以它从来没有栅格可丢，净额纹丝不动：

```
streaming replacement: geometry 16->17 builds 29->29 glyph-bytes 325512->325512 direct-paints 94
```

`geometry +1` 和 `builds 不变` 都成立，只有第三条不成立——而它在这台机器上**没有
可判定的对象**。修法不是在两个平台之间挑一个，而是把判据落到实际对象上：

```cpp
// 只有该行真的持有时，"丢弃"才有断言对象。
qsizetype glyphRasterBytesForLine(int lineIndex) const;   // bench hook
...
require(lineBytesAfter == 0, "…discard its cached pixels…");
require(lineBytesBefore == 0 || bytesAfterReplacement < textBytes,
        "…release the replaced line's share of the cache");
```

#### 9.7.5i 三条「等待」类断言：`flushEvents()` 只跑一轮，异步完成排在它后面

三个用例都把「异步提交完成后 UI 应同步」写成 `apply…(); flushEvents(); require(…)`。
设置字段是**异步持久化**的：`submitDraft` 先发 `Pending`（accepted 仍是旧值），写完
再发 `Clean`（accepted 换新）。widget 只在收到通知后 `QTimer::singleShot(0, …)` 重建，
所以一轮 `processEvents` 可能正好落在两个通知之间：

```
CLOUD-REBUILD force=0 default=[6775e195-…] rows=1      ← 全是旧值
cloud destination sync: sessionDefault=  value=6775e195-…   ← session 已是空，控件没有
```

判据改成等状态到位，而不是采样一次：

```cpp
waitUntil([&] { return destination->currentValue().toString() == defaultId; }, "…");
waitUntil([&] { return widget.findChild<QWidget*>("cloudUploadRow:" + id) != nullptr; }, "…");
```

同一批里另外两条「跨平台只能有一个值」的：

- `QUrl::fromLocalFile("C:/split.png")` 在 Unix 上把盘符当路径段，`toLocalFile()` 得到
  `/C:/split.png`，拖放列表永远不等于硬编码的期望。用 `QTemporaryDir::filePath()` 造 URL。
- xlsx 包里可能有**目录条目**（`xl/` 之类，payload 长度 0）。测试支撑代码原来写
  `check(length > 0)`，遇到目录条目就抛「invalid Excel workbook package」——而且所有
  `check()` 共用同一句消息，等于没有信息。改成跳过 `name.endsWith('/')`，并给每个
  `check()` 一个步骤名。

#### 9.7.5j 录音面板重复激活：**不要**改成 Export Settings（改了就红六套件）

`screen-recording-shortcut-tests` 里三条断言说「重复点已激活的工具要回到 Export
Settings」：

```
clicking active Select must return to Export Settings
repeating a drawing shortcut must switch only to Export Settings
repeating a recording tool shortcut must return to Export Settings like a button click
```

于是给 `activateToolFromToolbar()` 加了一条：

```cpp
if (alreadyActive && m_options.recordingDrawingMode) {
    setRecordingExportSettingsVisible(true);   // ← 已回滚，别再加回来
    return true;
}
```

结果下一轮 CI 一次红六套件，全部指向相反的方向：

```
repeated recording eraser click requests Select while preserving its mode
repeated recording shortcuts must return to Select
repeated recording tool activation selects canvas elements without export settings
repeated eraser activation selects without losing the host mode
```

即 `screenshot_tool_palette_tests`（`recordingEraserActivationReturnsToSelect`）、
`screen_recording_controller_tests`（`recordingActiveToolsReturnToSelect`）都要求
**重复激活回到 Select 工具**，而且这些用例逐个工具（shape/arrow/brush/eraser/…）、
逐个录音状态（Idle/Recording/Paused）、逐个入口（按钮、快捷键、`Tool::Select`
快捷键）地断言，比 e2e 的三条更细。产品侧 `activateDrawingTool()` 也只
`emit selectRequested()`，没有任何一路会把 `recordingExportSettingsVisible` 抬起来；
`connectScreenRecordingSelection()` 收到 selectRequested 时只会
`canvas->setCanvasTool(Select)` + `inputMode = Drawing`。

结论：这两组断言**互相矛盾**，上游代码状态（`const Tool requestedTool = alreadyActive
&& tool != Tool::Select ? Tool::Select : tool;`）满足的是「回到 Select」。判断依据放在
**哪里能反向验证**：改产品会让六套件一起红，改 e2e 只影响一条交互用例。

教训（比结论更重要）：e2e 的一条断言看起来最像「用户视角的真相」，但它可能只是
某次提交里顺手写的期望；当它与一组逐项覆盖的单元/集成用例冲突时，先数覆盖面，
再用「反向验证红在哪一侧」决定改谁。

#### 9.7.5k 构建产物清理：`.ilk` 与 `.pdb` 可以先删

本机 Debug 树到 25.6 G 时 `df` 只剩 375 M，链接直接失败：

```
LINK : fatal error LNK1201: 写入程序数据库"…tests.pdb"时出错
```

`LNK1201` 是**磁盘满伪装成的链接错误**（和 `STATUS_ACCESS_VIOLATION` 伪装成 rustc
崩溃同一类）。可安全删除且只触发重链接（不重编译）的：

- `*.ilk`（增量链接数据库，删了下次全量链接）
- `*.pdb`（除正在调试的那几个，删了链接时重建）

实测 16 个 `.ilk` 2.27 G + 70 个 `.pdb` 3.02 G，共释放约 5.3 G。**不动** `cargo/`、
`.tools/`、`artifacts/`。

#### 9.7.5l 「缓存 vs 直绘」逐像素比较：容差要看局部梯度，不是常数 1

三条用例（`screenshot-canvas-renderer`、`ocr-canvas-renderer`、`ocr-line-box-rendering`）
共用一个判据：缓存的字形栅格与直绘结果逐像素比，**实心色必须完全相等，边缘只允许
一个灰阶的差**。Linux 上失败，而且只差一个像素：

```
OCR cached/direct mismatch: mode=0 dpr=1.5 pixels=12 max_channel_difference=67
  ink cached=44089 direct=44089 first=88,69 cached=#fffffef7 direct=#fffffef6
  difference_violations=1 solid_violations=0 edge_violations=0
```

`edge_violations=0` 说明**每个差异像素都在边上**，`ink` 两边完全相等（没有多画/少画），
只有 1 个像素的落差到了 67。原因是缓存的中间面与直绘面的**像素原点不同**，Qt5 的
字形缓存 key 由整数设备坐标决定，边缘像素可能落到相邻的覆盖度桶里——这是**抗锯齿
斜坡上的一步**，不是「缓存画错了」。

常数容差 1 在这一步上必然误报，因为斜坡本身有多陡，一步就有多宽。判据改成跟局部
斜率走：

```cpp
// 3x3 邻域里最大的通道落差 = 这里的斜坡宽度
int edgeRange = 0;   // 遍历邻域时顺便求 max
matchesDirect &= (difference <= 1 || difference <= edgeRange) && !solid && edge;
```

这样「平坦区域差 2」照样红（`edgeRange` 为 0），「斜坡上差 67」放过；缓存整体错位
时是一整块而不是一两个像素，`solid` 与 `edge` 两条仍然拦得住。

#### 9.7.5m MCP 测试把响应写进已返回的栈帧（SEGFAULT 的真身）

`mcp-document-tests`、`mcp-application-tests` 都是 `SIGSEGV`，gdb 只给出两帧：

```
Thread 1 received signal SIGSEGV
0x00007ffff2766c9d in operator==(QString const&, QString const&)
#1  0x00005555558d2204 in runMcpApplicationTests()
#2  0x0000555555780f22 in main()
```

`operator==` 崩 + 回溯被截断 = **栈被写坏**，不是 QString 本身的问题。元凶是每个
`call()` 都长这样：

```cpp
std::optional<ScreenshotMcpResponse> result;                 // 栈上
service.request(request, [&](auto response) { result = std::move(response); });
while (!result && ...) { processEvents(); }
return *result;                                              // 帧已经结束
```

`McpApplicationService::request()` 在带 `idempotencyKey` 时会把 completion 存进
replay 表，之后**可能再来一次**（重放命中时交给 waiters）。第二次调用落在已经返回的
栈帧上，写进去的是整个 `ScreenshotMcpResponse`——于是下一个复用到那块栈的局部变量
（往往正好是一个 `QString`）读出来就是垃圾，比较时崩。

修法是把持有者搬出栈：

```cpp
auto result = std::make_shared<std::optional<ScreenshotMcpResponse>>();
service.request(request, [result](auto response) { *result = std::move(response); });
...
return result->value();
```

并在按 5000ms 轮询的那一处顺带数一下 completion 次数，`> 1` 就打一行——如果以后
产品真的重复回调，日志会直接说出来，而不是又变成一次玄学崩溃。

#### 9.7.5n `QString::fromLatin1(QByteArray)` 在 Qt5 遇 NUL 就截断

`collage-pdf-export` 报「long image tiles must remain on one page」，但导出的 PDF
里明明有 31 个图像字典（30 个 `1x2048` 蓝瓦片 + 1 个 `19x23` 红图）。测试自己的诊断
说出了真身：

```
pdf scan: bytes=1220 chars=186 dicts=1 streams=1
tile match 0 at 41 size 7x11 length 242 valid 1
```

1220 字节的 PDF 转成 QString 只剩 186 个字符：

```cpp
// Qt 5.15.2 qstring.h:712-713
static inline QString fromLatin1(const QByteArray &str)
{ return str.isNull() ? QString() : fromLatin1(str.data(), qstrnlen(str.constData(), str.size())); }
```

`qstrnlen` 数到**第一个 NUL 字节**为止。Qt6 改成显式传 `str.size()`，所以同一份代码
在 Windows/macOS 的 Qt6 上一直是绿的。PDF 由 FlateDecode 流组成，压缩数据里 0x00 遍地
都是 ⇒ 第一个流之后的所有图像对象都被「看不见」，`pdfImages()` 只返回 1 张。

凡是「把非文本字节缓冲当文本扫」的地方都要钉住长度：

```cpp
QString pdfText(const QByteArray& pdf) {
    return QString::fromLatin1(pdf.constData(), int(pdf.size()));
}
```

同一个陷阱也适用于 `QString::fromUtf8(bytes)`、`QString(bytes)` 这些吃 `QByteArray`
的重载（Qt5 全部走 `qstrnlen`；Qt6 全部走 `size()`）。安全写法只有三种：显式长度、
先 `toBase64()/toHex()`、或确认缓冲里不可能有 NUL。产品代码里现有几处（`mcpdocumentservice`
读 UTF-8 文本、`screenshotmcprserver` 转 hex）都属后两类，无需改。

判据要点：**Qt6 绿、Qt5 红，且红在「数量少了一半以上」** ⇒ 先怀疑 NUL 截断，别查正则。

#### 9.7.5o `QTest::mouseMove` 走坐标不走抓取；外加一个跨函数残留的样式解析器

`recording-audio-gain-popover` 报「dragging out of a hover-opened editor preserves
gain editing」，本地 Windows Qt5 一样红——**不是 Linux 专属**，是 Qt5/Qt6 的派发差异：

```cpp
const QPoint thumb(slider->width() / 2, 16);
QTest::mousePress(slider, Qt::LeftButton, Qt::NoModifier, thumb);
QTest::mouseMove(slider, {slider->width() + 80, 16});   // 落点已经出了控件
```

Qt5 的 `QTest` 鼠标助手按**坐标**找窗口：`x = width + 80` 那个点归弹窗后面那个 host
窗口所有，于是移动（以及同坐标的 release）根本没送到按住不放的 slider 上。Qt6 把它交给
持有隐式抓取的控件，所以上游一直绿。判据很好认：**控件内坐标能动、控件外坐标完全没反应**。

把这类手势直投给控件（控件本来就是平台该选中的那个），应用级事件过滤器照跑，所以
「弹窗不许被拖关」这半条契约仍然被覆盖：

```cpp
void sendMouseTo(QWidget* widget, QEvent::Type type, QPointF position, Qt::MouseButton button,
                 Qt::MouseButtons buttons) {
    QMouseEvent event(type, position, widget->mapToGlobal(position.toPoint()), button, buttons,
                      Qt::NoModifier);
    QCoreApplication::sendEvent(widget, &event);
}
```

修好它之后同一个二进制又崩在**后一个**函数里（前一个用例以前总在 `require` 处退出，
后面的函数从来没被执行过，所以这一串问题是串着暴露的）。定位方法：在 `main` 里给每个
函数入口各打一行，二分到「A 之后 B 崩」；再单独跑 B（加一个临时 `--x-only` 参数）——
单独跑是绿的，说明 B 自己没问题，是 A 留下的状态。

真凶是 A 装给 slider 的样式解析器：

```cpp
slider->setSemanticStyleResolver([&](const auto& ctx) { focusFeedback = ctx.focused; ... });
```

它捕获的是 A 栈帧里的局部变量，而弹窗的表面控件**比栈帧活得久**（函数返回后仍会被重绘），
于是下一次重绘就把死栈帧当活变量读，随机崩在下一个用例里。同一个坑在 `#638 trimming`
那类「先装观察者、函数返回」的写法里同样成立。修法是函数结束前显式卸掉：

```cpp
slider->setSemanticStyleResolver({});
```

顺带记两个失败的假设，省得后人再走一遍：把 tooltip 收掉（`QToolTip::hideText()`）、在
`popup.close()` 之后再 flush，都**不能**消除崩溃——只有清掉解析器才行。

#### 9.7.5p 非整数 DPR 下的像素对照量的是光栅相位，不是样式

`image-split-editor` 与 `image-split-canvas` 共用 `sequenceNumbersUsePdfBadgeStyle()`：
把画布上的序号徽章与「PDF 页上那个 QLabel 徽章」逐像素比，允许十分之一的像素有差。

```
Badge 10, font 17, scale 1.25: 127 of 1204 pixels differ
result ordinals must match PDF badge size, corners, opacity and font in both themes at every DPI
```

`127 / 1204 = 10.5%`，只超出预算 7 个像素；而 Windows Qt6 与本地 Windows Qt5 都绿，
**样式完全没变**。原因是两个渲染器落不到同一套栅格上：

- 画布：`SequenceNumberBadgeStyle::paint()` 用 `QRectF`，矩形原点是
  `imageToViewport(region.topLeft()) + inset`，**带小数**；`drawRoundedRect` 与
  `drawText(QRectF)` 都按这个小数原点抗锯齿。
- 参考：同一份样式表交给 `QLabel`，`QStyle` 内部用 **整数** `QRect` 布局和绘制。

整数 DPR 时两者的小数部分都是 0，能逐像素对上；`scale=1.25/1.5` 时每条边、每一笔
字形都错开半个设备像素，于是**整圈抗锯齿斜坡**都变了颜色。这不是被测样式的问题，
是「拿 QLabel 当独立参考」这条路在非整数 DPR 下必然付的代价。

判据改成只比参考图的**平坦像素**（3×3 邻域亮度极差 ≤ 24），容差放到 12：

```cpp
const auto isFlat = [&](int x, int y) { /* 3x3 luminance range <= 24 */ };
for (int y = bounds.top(); y <= bounds.bottom(); ++y)
    for (int x = bounds.left(); x <= bounds.right(); ++x) {
        if (!isFlat(x, y))
            continue;                       // 相位在这里，注定对不上
        ++flat;
        if (withinChannel(actual, wanted, 12)) continue;
        ++different;
    }
const bool matches = different * 10 < flat;
```

平坦区恰好是「尺寸、圆角、底色透明度、字号」必须显形的地方：字号变大时字形会盖到
参考图的平坦底上（照样算差异），alpha 变化会改动整块平地，尺寸变化会挪动块边界。

**但圆角是个例外，必须单独测**——半径变化只挪动角上那段圆弧，而圆弧在两张图里都是
边缘、都被上面那段 `continue` 跳过了。反向验证时 `radius + 3` 的注入**曾经直接通过**
（"漏判"），补一条沿对角线往里数「走到第几步才踩到填充」的探针才抓住：

```cpp
const auto cornerInset = [&](const QImage& image, int x, int y, int dx, int dy) {
    for (int step = 0; step <= 6; ++step)
        if (luminance(image.pixelColor(x + dx * step, y + dy * step)) < 200)
            return step;
    return 7;
};
// 四角各取一次，|actual - expected| <= 1（相位最多挪一个整像素）
const bool matches = different * 10 < flat && radiusDelta <= 1;
```

反向验证（改产品 `SequenceNumberBadgeStyle::paint()`，每项单独编译运行）：

| 注入 | 结果 |
| --- | --- |
| 无（基线） | 绿，36 个组合全过 |
| `backgroundAlpha - 40` | 红：`421 of 421 flat pixels differ` |
| `radius + 3` | 红：`corner inset off by 2` |
| `radius = 0` | 红：`corner inset off by 2` |

两个环境事实顺带记下来，省得下次白查：

- **Windows 的 `-platform offscreen` 也没有字体库**，和 Linux 一样每个字形都是空白
  （导出的 `badge-actual.png` / `badge-pdf.png` 只有圆角方块、没有数字）。所以本机
  验不了「字号」这一条；字号走「字形盖到参考图平地」的判据，只能靠 CI 验。
- **Qt5 + 非整数 DPR + 旋转文字会崩**：本地 Windows Qt5 跑 `--canvas-only` 在
  `scale=1.5` 处静默死（`rc=3`，无输出、WerFault 触发 `reg.exe` 命中沙箱黑名单把整棵树
  送 SIGTERM），`probe` 定位到竖标尺里 `rotate(-90)` 之后的 `drawText`。Linux Qt5 与
  Windows Qt6 同一用例跑到 1.5/2.0 都是绿的，所以这是本地这套
  Qt 5.15.2-msvc2019 + offscreen 组合的毛病，不只是用例问题；真要碰它得在真机上用
  调试器看 libQt5Gui。本机想跑这个套件只能在 `scale` 列表里临时摘掉 1.5，**别提交**。

#### 9.7.5q offscreen 不交接激活：`ApplicationDeactivate` 是它自己合成的

`recording-audio-controller` 报「rapid source switching rejects a stale preview before
retirement completes」。判据是三件一起成立：麦克风浮层可见、系统音频浮层不可见、预览
monitor 没被重建（`creates=1 active=1`）。本地复现，诊断打印：

```
audio gain switch: microphone=0 system=0 creates=1 active=1 \
  micEnabled=1 sysEnabled=1 micTrigger=1 micSurface=0
```

monitor 那一半全对，两个浮层全关。打开 `adqt.popup` 调试类别（`QT_LOGGING_RULES="adqt.popup.debug=true"`）
看轨迹，关掉刚打开那个浮层的是 **`host.close detail 3`（= `ScopeDeactivated`）**，
而它来自 `QEvent::ApplicationDeactivate` 分支（不是 scope 窗口的 `WindowDeactivate`）：

```
visibility.applied ... anchor "screenRecordingMicrophone" requested true actual true
surface.hide "adpopover-surface"          <- 兄弟浮层的顶层工具窗口被释放
host.close detail 3 anchor "screenRecordingMicrophone" requested true actual true
visibility.transition detail 0 ... surface.hide ... applied requested false actual false
```

根因是平台能力：`scheduleScopeDeactivateClose()` 里本来有「浮层自己就是活动窗口就不关」
的护栏，但 **offscreen QPA 从不激活任何窗口**，`QApplication::activeWindow()` 恒为 null，
护栏永远不成立；更早的 `ApplicationDeactivate` 分支连护栏都没有。真实平台隐藏工具窗口
会把激活交回 scope 窗口，offscreen 交给「没有」，于是每次 `surface.hide()` 都等于「用户
切走了」。Qt6 的 offscreen 不发这一串事件，所以上游一直是绿的。

**别去改产品**：先试过在 `popup_interaction_host.cpp` 里加「平台没有活动窗口就不推断
失活」，`recording-audio-controller` 绿了，但 `recording-audio-gain-popover` 的
`application deactivation closes focused editing` 立刻红——那**正是**要靠这个事件关浮层
的地方，等于把契约拆了。正确的位置在用例：平台能力不同就按平台分叉，并且把 monitor
那半条契约（本用例真正的主题）保持严格：

```cpp
const bool platformActivatesWindows =
    QGuiApplication::platformName() != QLatin1String("offscreen");
if (!platformActivatesWindows) {
    microphone->openAndFocus();          // 相当于「真实用户的下一拍点击」
    QCoreApplication::processEvents();
}
require(microphone->popover()->isVisible() && !system->popover()->isVisible() &&
            audioMonitorCreates == 1 && audioMonitorsActive == 1, ...);
```

顺带一个能让本机多跑 5 个套件的链接修法：`verifyToolbarCaptureExclusion*` 定义在
`tests/toolbar_capture_exclusion_native_tests.cpp`，而 CMake 只在
`WIN32 AND SNOW_QT_MAJOR STREQUAL "6"` 分支里把它加进目标（它要 `QRhiWidget`）。
`screen_recording_controller_tests.cpp` 的调用点只用 `#ifdef Q_OS_WIN` 包着，于是
**Windows + Qt5** 这套组合链接时缺两个符号：

```
error LNK2019: 无法解析的外部符号 verifyToolbarCaptureExclusionAffinity / verifyToolbarCaptureExclusion
```

Linux Qt5 上这段被 `Q_OS_WIN` 整个编掉，Windows Qt6 有定义，所以两边 CI 都看不见。
调用点加 `QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)` 即可（那些 runner 本来也只在 Qt6 注册）。
同名的 `screenshot_toolbar_remembered_tool_tests.cpp` 不要一起改：它那个目标还缺
`applyCaptureExclusionCompositionFallback` / `setWindowExcludedFromCapture`（来自
Qt6 分支加的 `windowchrome.cpp`），改一半只会把 `#if/#endif` 配平搞坏。

再记一次本机踩的坑：清理磁盘时删掉 `build/**/*.pdb` 会让 **PCH 一起失效**
（`error C2859: ... 不是创建预编译头时使用的 pdb`），随后只删 `*.pch` 又不触发重建
（`fatal error C1083`，因为 `.pch` 和 `.obj` 是同一条规则产出的，`.obj` 还在 ninja 就
认为它是最新的）。修法：`touch` 每个 `CMakeFiles/<t>.dir/cmake_pch.cxx` 强制重跑那条
规则。

#### 9.7.5r `QVariant::operator==` 对自定义类型：Qt6 从 `operator==` 派生，Qt5 答「不等」

`snow-shot-mcp-application-tests` 在 Linux CI 是 `***Exception: SegFault 0.36 sec`，本机
Qt5 是当场断言：

```
[qt.fatal] ASSERT: "!isEmpty()" in file C:\Qt\5.15.2\msvc2019_64\include\QtCore/qvector.h, line 242
```

行 242 是 `QVector::first()` 的 `Q_ASSERT(!isEmpty())`。用
`SNOW_PROBE_REQUIRE=1`（临时给 `require()` 加一条成功也打印的分支）把死点夹到
`models_update` 之后，再打状态：

```
PROBE type=snow_shot::CustomAiModels valid=1 draftType=snow_shot::CustomAiModels
      size=0 draftSize=1 phase=1 busy=1 store=1 variantEq=0 payloadEq=1 err=
```

四个数把根因钉死了：**存储里确实有 1 个模型**（`store=1`），**草稿也有**（`draftSize=1`），
但**两个内容完全相同的 QVariant 比较结果是「不等」**（`variantEq=0`，而直接比较
payload 是 `payloadEq=1`）。

`SettingsRuntimeSession::valuesEqual()` 对自定义渲染器最终落到 `first == second`。Qt6 会
从类型的 `operator==` 派生 `QMetaType` 的 equals 函数，Qt5 只认
`QMetaType::registerEqualsComparator<T>()` 注册过的那几个，其余自定义类型一律答「不等」。
于是写入路径判 `pending` 恒真：字段永远 `Pending`/`busy`，`acceptedValue` 永远停在上一个
值（空列表），`customAiModels()` 对一个明明配置好的服务商返回空表。UI 上表现就是
「模型列表怎么填都是空」。

修法（`settingsruntimesession.cpp`，只影响 Qt5）：

```cpp
void registerSettingsVariantComparators() {
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QMetaType::registerEqualsComparator<CustomAiModels>();
    QMetaType::registerEqualsComparator<TextTranslationConfigurations>();
    QMetaType::registerEqualsComparator<CloudUploadSettings>();
#endif
}
```

在 `SettingsRuntimeSession` 构造函数里调一次。注册后同一份探针变成
`size=1 phase=0 busy=0 variantEq=1`，用例转绿。挑这三个类型是因为它们是
`valuesEqual()` 唯一没有显式分支的自定义 renderer；工具栏布局、快捷键表本来就有显式比较
（但它们的 `QVariant` 层面同样「永远不等」，会让 `SettingsFieldState::operator==` 一直报
变化，属于同一类隐性噪声，暂不动）。

反向验证：把三行 `registerEqualsComparator` 注释掉 ⇒ `--application-only` 立刻回到
`qvector.h:242` 断言；恢复 ⇒ rc=0。

#### 9.7.5s Qt5 的 `QFutureInterface` 持有结果，promise 一死 `QFutureWatcher::result()` 就读空

修完上一条，同一个用例前进到配置导出，然后换一种方式死：

```
PROBE before export path=.../redacted.zip
PROBE worker enter .../redacted.zip
PROBE worker wrote empty=1        # 归档写成功
PROBE wentry count=0 finished=1   # 主线程读结果：已 finished，但结果数是 0
<Segmentation fault>
```

`background()` 用 `runMcpWork()` + `QFutureWatcher<Response>`，回调里 `watcher->result()`。
Qt6 的 `QPromise` 把结果放在与 future 共享的状态里，promise 什么时候死都行；Qt5 的
`SnowPromise` 是一个 `QFutureInterface<T>`，而

```cpp
QFutureInterface<T>::~QFutureInterface()
{
    if (!derefT())
        resultStoreBase().template clear<T>();   // 结果就地销毁
}
```

future 只共享 base 状态，不参与这个 typed 引用计数，所以**工作线程里那份 promise
析构的一刻，结果就没了**；`QFuture::result()` 再去 `resultAt(0)` 就是取空容器的下标，
`finished=1` 但 `resultCount()==0`，读到的指针是释放过的。

修法（`mcpasyncwork_p.h`）：`runMcpWork()` 返回 `McpWork<Result>`（future + promise），
主线程调用点改成

```cpp
runMcpWork(&workers, [..] { ... }).setFutureOn(watcher);
```

`setFutureOn()` 把 promise 挂到一个 parent 是 watcher 的 `McpWorkKeeper` 上，
并用 `promise.reset()` 让 worker 在 `finish()` 之后立刻放手。这样「谁读结果」和
「promise 活多久」是同一个对象，不再依赖线程时序。四个调用点全部改掉
（`background()`、导入的 `runtimeWatcher`、归档读取、`applicationcontroller` 的
`resolveMcpSource`）。

一条没敢当成证据的观察：只把 promise 改成堆上 `shared_ptr`、**不**挂到 watcher 上，这次
也是绿的。那是因为 worker 拷贝的那份要等 `QThreadPool` 销毁 runnable 才释放，恰好晚于
主线程处理 `finished`。这属于时序运气，不是保证，所以保留 keeper。反向验证用「回到
HEAD 版 `mcpasyncwork_p.h` + 四个调用点」做：`rc=139`，连跑三次都崩；改回来
`--application-only` 连跑三次 + 全量一次全 rc=0。

同一轮顺带修掉一个**只在 Debug 树里成立的**超时：4000×4000 的 PNG 回退重编码在本机
未优化构建要 4874 ms，用例里 `call()` 的 5 s 挂死护栏差一点就误判成产品卡住。`call()`
加了可选的 `timeoutMs`（默认 5000），只有那一次大图请求传 30000，注释写清是护栏不是
产品断言。

#### 9.7.6 外部依赖 / 子进程 / 网络（其余）

`mcp-fixture`、`mcp-application-fixture`、`recording-build-profiles`（Python traceback）、
`api-client`（"local API test server timed out waiting for a request"）、
`ocr-cpu-recognition-service`、`ocr-managed-runtime`（模型未 staged）、
`cloud-upload`、`cloud-upload-settings`、`text-translation`、`configuration-archive`、
`collage-pdf-export`、`image-split-editor`、`image-split-canvas`、`overlay-frame-presenter`、
`magnifier-render`、`magnifier-interaction`、`pinned-recognition-save`、`pinned-quick-save`、
`about-page`、`settings-color-commit`、`screen-recording-shortcut`、
`recording-history-shortcut`、`recording-audio-controller`、`recording-audio-gain-popover`、
`recording-hidden-tool-shortcut`、`region-shadow-renderer`、
`selection-background-preview`、`layout-compaction`。

已移出本清单（2026-11-05，见 §9.7.5b / §9.7.5c）：`system-tray-controller`、
`recording-export-tray`、`direct-capture-native`、`export-artifact`、`print-export-snapshot`。

已移出本清单（2026-11-05 第二批，见 §9.7.5d）：`about-page`、`settings-color-commit`、
`configuration-archive`、`selection-background-preview`、`layout-compaction`、
`image-split-editor`、`image-split-canvas`、`image-split-export`、`overlay-frame-presenter`。

这一组需要逐个看，但排期放在 9.7.1–9.7.5 之后：前三类是**同一根因打多个套件**，修一条消一片；
这一组反过来，修一条只消一条。

工具侧同时补了两条：`gh-ci.py` 的 `read_body()` 重试 + Range 分段（§9.6.65），
以及 GUI 测试的 `-o file,txt`（§9.6.64）。没有这两条，上面这张表根本读不出来。
