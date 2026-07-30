# CAD Parser — C++ Demo Library

C++17 解析库，支持 **DXF** 和 **DWG** CAD 文件，提供统一的 `cad::Drawing` 数据结构。
核心 DXF/SVG/CLI 可在 Linux 和 Windows 上构建；DWG 解析按平台上可用的 LibreDWG C API
或 `dwgread` 工具启用。

完整的接口说明、数据模型、坐标约定、渲染行为和扩展实体流程见
[API 与架构开发指南](docs/API_AND_ARCHITECTURE.md)。README 保留项目概览和常用命令。

## 文档

| 文档 | 内容 |
|------|------|
| [API 与架构开发指南](docs/API_AND_ARCHITECTURE.md) | C++ API、`Drawing` 数据模型、DXF/DWG 后端、SVG/Qt 渲染、坐标系、测试和扩展指南 |
| [README](README.md) | 构建方式、CLI 快速上手和支持的实体 |

## DWG 双后端架构

DWG 解析支持两种后端，CMake 配置时选择其中一种：

| 后端 | 机制 | 优势 | 劣势 |
|------|------|------|------|
| **C API** (推荐) | 直接链接 `libredwg.a`，调用 `dwg_read_file()` | 🚀 性能最优，无子进程开销，内存直接转换 | 需要编译 libredwg 源码 |
| **CLI** (fallback) | 通过跨平台子进程管道调用 `dwgread` / `dwgread.exe` | 📦 无需链接 C API | 子进程 + JSON 解析开销 |

当 CMake 找到 libredwg C API 时，构建产物使用 C API；否则，若启用了
`CAD_USE_LIBREDWG_CLI`，构建产物使用 CLI。该选择发生在**配置/编译期**，C API
解析失败时不会在运行期自动改走 CLI。

```
                  ┌─────────────────────────┐
                  │    cad_parser.h          │
                  │   (统一入口)              │
                  ├─────────────────────────┤
                  │    common_types.h        │
                  │   (Line/Circle/Arc/...)  │
                  └───────────┬─────────────┘
                              │
          ┌───────────────────┼───────────────────┐
          │                   │                   │
    ┌─────┴─────┐      ┌──────┴──────┐      ┌─────┴─────┐
    │dxf_parser │      │ dwg_parser  │      │parse_file │
    │ (纯C++)   │      │  (dispatcher)│      │(自动识别) │
    └───────────┘      └──┬───────┬──┘      └───────────┘
                          │       │
              ┌───────────┘       └───────────┐
              ▼                               ▼
    ┌──────────────────┐          ┌──────────────────┐
    │ dwg_api_impl.cpp │          │ dwgread CLI      │
    │ (libredwg C API) │          │ (子进程管道)      │
    │ #include <dwg.h> │          │ + mini JSON解析  │
    └────────┬─────────┘          └──────────────────┘
             │
    ┌────────┴─────────┐
    │ libredwg (.a/.lib)│
    │  (第三方静态库)   │
    └──────────────────┘
```

## 支持的实体

| 实体 | DXF | DWG (API) | DWG (CLI) | 说明 |
|------|-----|-----------|-----------|------|
| LINE | ✅ | ✅ | ✅ | 直线 |
| CIRCLE | ✅ | ✅ | ✅ | 圆 |
| ARC | ✅ | ✅ | ✅ | 圆弧 |
| ELLIPSE | ✅ | ✅ | ✅ | 椭圆 |
| LWPOLYLINE | ✅ | ✅ | ✅ | 轻量多段线（含 bulge） |
| POLYLINE | ✅ | ✅ | ✅ | 传统多段线 |
| TEXT | ✅ | ✅ | ✅ | 单行文字 |
| MTEXT | ✅ | ✅ | ✅ | 多行文字 |
| INSERT | ✅ | ✅ | ✅ | 块引用 |
| POINT | ✅ | ✅ | ✅ | 点 |
| SOLID/3DFACE | ✅ | ✅ | ✅ | 填充面 |
| SPLINE | ✅ | ✅ | ✅ | NURBS 样条 |
| DIMENSION | ✅ | ✅ | ✅ | 标注 |
| HATCH | ✅ | ✅ | ✅ | 填充图案 |

## 编译

### Linux：纯 DXF + DWG CLI fallback（无需编译 libredwg）

```bash
cd dwg-dxf-parser-cpp
mkdir build && cd build
cmake ..
cmake --build . --parallel

# DWG 需要系统安装 libredwg-tools
sudo apt install libredwg-tools  # Ubuntu/Debian
```

### Linux：libredwg C API 源码集成（推荐，性能最优）

```bash
cd dwg-dxf-parser-cpp

# 1. 编译 libredwg 为静态库
./scripts/build_libredwg.sh

# 2. 构建 cad-parser，指定 libredwg 路径
mkdir build && cd build
cmake .. -DLIBREDWG_ROOT_DIR=../third_party/libredwg
cmake --build . --parallel
```

`build_libredwg.sh` 会自动：
1. 初始化固定的 `third_party/libredwg/` 子模块（自定义目录时克隆 0.13.4）
2. 运行 `autogen.sh` → `./configure --disable-bindings --enable-static`
3. 编译出 `src/.libs/libredwg.a`

`scripts/build_libredwg.sh` 使用 Autotools 和 Bash，适用于 Linux/Unix 环境。Windows 使用
`scripts/build_windows.ps1` 构建仓库固定的 LibreDWG 子模块，或安装 `dwgread.exe` 后使用
CLI 后端。

### Windows：Visual Studio 2022 / MSVC

先安装 CMake 3.16+ 和 Visual Studio 的"Desktop development with C++"工作负载。仓库将
LibreDWG 0.13.4 源码固定为 Git 子模块；普通 clone 后，在构建 C API 前先初始化该子模块。

#### 零依赖（仅 DXF + DWG CLI fallback）

DXF 核心、SVG 导出、CLI 和测试不需要任何第三方 CAD 依赖：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DBUILD_TESTS=ON -DBUILD_QT_VIEWER=OFF
cmake --build build --config Release --parallel
cmake -E chdir build ctest -C Release --output-on-failure
.\build\Release\cad-parser-cli.exe demo\sample.dxf
```

#### 完整构建（DXF + DWG C API + Qt 查看器）

**1. 编译 libredwg（DWG 原生支持）**

初始化固定版本的子模块后，直接用 MSVC 编译：

```powershell
git submodule update --init --recursive --depth 1
cd third_party\libredwg
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DBUILD_SHARED_LIBS=ON
cmake --build build --config Release --parallel
cd ..\..
```

产物：`build/Release/libredwg.dll` + `libredwg.lib`

**2. 安装 Qt5（cad-viewer 可视化）**

```powershell
pip install aqtinstall
python -m aqt install-qt windows desktop 5.15.2 win64_msvc2019_64 -O D:/Qt
```

**3. 构建全部目标**

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DBUILD_TESTS=ON -DBUILD_QT_VIEWER=ON `
  -DCMAKE_PREFIX_PATH="D:/Qt/5.15.2/msvc2019_64" `
  -DLIBREDWG_ROOT_DIR="third_party/libredwg"
cmake --build build --config Release --parallel
```

**4. 部署运行时 DLL（仅需首次或 DLL 变更后）**

```powershell
# 将需要的 DLL 复制到 exe 旁
copy third_party\libredwg\build\Release\libredwg.dll build\Release\
copy D:\Qt\5.15.2\msvc2019_64\bin\Qt5Core.dll build\Release\
copy D:\Qt\5.15.2\msvc2019_64\bin\Qt5Gui.dll build\Release\
copy D:\Qt\5.15.2\msvc2019_64\bin\Qt5Widgets.dll build\Release\
mkdir build\Release\platforms
copy D:\Qt\5.15.2\msvc2019_64\plugins\platforms\qwindows.dll build\Release\platforms\
```

也可以用 `scripts/build_windows.ps1` 自动初始化子模块、构建静态 LibreDWG，并在启用查看器时
实际执行 `windeployqt` 部署 Qt 运行时。使用外部共享 LibreDWG 安装时，脚本也会复制找到的
`libredwg.dll` 或 `redwg.dll`。

**5. 验证**

```powershell
# 运行测试
cmake -E chdir build ctest -C Release --output-on-failure

# DWG 解析
.\build\Release\cad-parser-cli.exe d:\path\to\drawing.dwg --count

# SVG 导出
.\build\Release\cad-parser-cli.exe demo\sample.dxf --svg output.svg

# HTML 交互式查看器
.\build\Release\cad-parser-cli.exe demo\sample.dxf --html viewer.html

# Qt 查看器（无界面截图，用于自动化回归比对）
.\build\Release\cad-viewer.exe demo\sample.dxf --screenshot preview.png
```

也可以使用仓库内脚本执行同一流程：

```powershell
.\scripts\build_windows.ps1 -EnableQtViewer -QtDir "D:/Qt/5.15.2/msvc2019_64"
```

### CMake 选项

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `CAD_USE_LIBREDWG_API` | ON | 启用 libredwg C API 后端 |
| `CAD_USE_LIBREDWG_CLI` | ON | CMake 未找到 API 时，启用 CLI 后端 |
| `LIBREDWG_ROOT_DIR` | 空 | 指定 libredwg 源码树或安装路径 |
| `BUILD_QT_VIEWER` | ON | 构建 Qt5 查看器 |
| `BUILD_TESTS` | OFF | 构建测试 |
| `CMAKE_PREFIX_PATH` | 空 | Qt 安装路径，如 `D:/Qt/5.15.2/msvc2019_64` |

### 测试

```bash
cmake -S . -B build -DBUILD_TESTS=ON
cmake --build build --parallel
cmake -E chdir build ctest --output-on-failure
```

## 用法

### CLI 演示工具

```bash
# DXF 解析（纯 C++，零依赖）
./build/cad-parser-cli demo/sample.dxf

# 详细信息
./build/cad-parser-cli demo/sample.dxf --verbose --max 20

# 只列图层
./build/cad-parser-cli demo/sample.dxf --layers

# 按类型统计
./build/cad-parser-cli demo/sample.dxf --count

# 列出块定义
./build/cad-parser-cli demo/sample.dxf --blocks

# DWG 解析（C API 或 CLI）
./build/cad-parser-cli drawing.dwg

# 检查 DWG 支持状态
./build/cad-parser-cli --check-dwg
```

### Qt 查看器

在 CMake 找到 Qt5 Widgets 时会生成 `cad-viewer`：

```bash
./build/cad-viewer drawing.dwg

# 无界面生成 PNG，便于回归比对
QT_QPA_PLATFORM=offscreen ./build/cad-viewer drawing.dwg \
  --screenshot preview.png --screenshot-size 1600x1000
```

查看器支持缩放、拖拽平移、适配视图和图层显隐。有关 CAD 笛卡尔坐标如何映射到
Qt/SVG 屏幕坐标，以及文本对齐的当前行为，见[开发指南](docs/API_AND_ARCHITECTURE.md#7-坐标系与文本对齐)。

### C++ API

```cpp
#include <cad_parser/cad_parser.h>

// === DXF 解析（纯 C++，无外部依赖） ===
cad::ParseResult result;
cad::Drawing d = cad::parse_dxf_file("floorplan.dxf", {}, &result);

// === DWG 解析（构建时选定的 libredwg 后端） ===
cad::DwgParseResult dwg_result;
cad::Drawing dwg = cad::parse_dwg_file("mechanical.dwg", {}, &dwg_result);

// === 自动识别格式 ===
cad::Drawing any = cad::parse_file("mystery.dwg"); // .dxf 也可

// === 快速预览（只读头部） ===
auto info = cad::peek_dxf_header("huge.dxf");

// === 遍历实体 ===
for (auto& rec : d.entities) {
    std::cout << cad::entity_type_name(rec.entity)
              << " on layer \"" << rec.layer_name << "\"\n";

    std::visit([](auto&& e) {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, cad::LineEntity>) {
            std::cout << "  (" << e.start.x << ", " << e.start.y
                      << ") -> (" << e.end.x << ", " << e.end.y << ")\n";
        }
        if constexpr (std::is_same_v<T, cad::CircleEntity>) {
            std::cout << "  center=(" << e.center.x << ", " << e.center.y
                      << ") r=" << e.radius << "\n";
        }
    }, rec.entity);
}

// === 访问图层 ===
for (auto& [name, layer] : d.layers) {
    std::cout << name << " color=" << layer.color_index << "\n";
}
```

## 项目结构

```
dwg-dxf-parser-cpp/
├── CMakeLists.txt                  # 主构建配置
├── cmake/
│   └── FindLibreDWG.cmake          # libredwg 查找模块
├── docs/
│   └── API_AND_ARCHITECTURE.md      # API、架构与开发指南
├── scripts/
│   ├── build_libredwg.sh           # libredwg 一键编译脚本
│   └── build_windows.ps1            # Windows 配置、构建和测试脚本
├── third_party/                    # 第三方源码目录
│   └── libredwg/                   # 固定版本的 Git 子模块
├── include/cad_parser/
│   ├── cad_parser.h                # 统一入口
│   ├── common_types.h              # 14种实体类型 + 图层/块/颜色
│   ├── dxf_parser.h                # DXF 解析器 API
│   └── dwg_parser.h                # DWG 解析器 API（统一接口）
├── src/
│   ├── common_types.cpp
│   ├── dxf_parser.cpp              # 纯C++ DXF解析器（~700行）
│   ├── dwg_parser.cpp              # DWG 解析器（dispatcher + CLI后端）
│   ├── dwg_api_internal.h          # C API 内部接口
│   ├── dwg_api_impl.cpp            # libredwg C API 后端（~400行）
│   └── main.cpp                    # CLI 演示工具
└── demo/
    └── sample.dxf                  # 测试用 DXF 文件
```

## libredwg C API 后端核心流程

`dwg_api_impl.cpp` 直接调用 libredwg C API：

```cpp
#include <dwg.h>  // libredwg C header

Dwg_Data dwg_data;
memset(&dwg_data, 0, sizeof(Dwg_Data));

// 1. 读取 DWG 文件 → Dwg_Data 结构树
int rc = dwg_read_file(filepath, &dwg_data);

// 2. 遍历对象，按类型分派
for (unsigned i = 0; i < dwg_data.num_objects; i++) {
    Dwg_Object* obj = dwg_data.object[i];
    switch (obj->type) {
        case DWG_TYPE_LINE: {
            auto& line = obj->tio.LINE;
            // line->start.x, line->end.y, ...
            break;
        }
        case DWG_TYPE_CIRCLE: {
            auto& circle = obj->tio.CIRCLE;
            // circle->center, circle->radius, ...
            break;
        }
        // ... 14种实体类型
    }
}

// 3. 释放 libredwg 内存
dwg_free(&dwg_data);
```

与 CLI 方式的关键区别：

| 对比维度 | C API | CLI |
|----------|-------|-----|
| 数据通道 | 内存直接访问 | 子进程管道 + JSON 文本 |
| 精度 | 二进制 double，无损 | JSON 序列化可能有精度损失 |
| 性能 | 单次遍历，O(n) | 子进程 + JSON 解析 |
| 部署 | 链接静态库，单文件 | 依赖 `dwgread` 在 PATH |

## License

MIT
