# CAD Parser — C++ Demo Library

C++17 解析库，支持 **DXF** 和 **DWG** CAD 文件，提供统一的 `cad::Drawing` 数据结构。

## DWG 双后端架构

DWG 解析支持两种后端，编译时自动选择最优方案：

| 后端 | 机制 | 优势 | 劣势 |
|------|------|------|------|
| **C API** (推荐) | 直接链接 `libredwg.a`，调用 `dwg_read_file()` | 🚀 性能最优，无子进程开销，内存直接转换 | 需要编译 libredwg 源码 |
| **CLI** (fallback) | 通过 `popen()` 调用 `dwgread` 子进程 | 📦 系统 `apt install` 即可 | 子进程 + JSON 解析开销 |

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
    │ (libredwg C API) │          │ (popen 子进程)   │
    │ #include <dwg.h> │          │ + mini JSON解析  │
    └────────┬─────────┘          └──────────────────┘
             │
    ┌────────┴─────────┐
    │  libredwg.a      │
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

### 方式一：纯 DXF + DWG CLI fallback（无需编译 libredwg）

```bash
cd dwg-dxf-parser-cpp
mkdir build && cd build
cmake ..
make -j$(nproc)

# DWG 需要系统安装 libredwg-tools
sudo apt install libredwg-tools  # Ubuntu/Debian
```

### 方式二：libredwg C API 源码集成（推荐，性能最优）

```bash
cd dwg-dxf-parser-cpp

# 1. 编译 libredwg 为静态库
./scripts/build_libredwg.sh

# 2. 构建 cad-parser，指定 libredwg 路径
mkdir build && cd build
cmake .. -DLIBREDWG_ROOT_DIR=../third_party/libredwg
make -j$(nproc)
```

`build_libredwg.sh` 会自动：
1. `git clone` libredwg 到 `third_party/libredwg/`
2. 运行 `autogen.sh` → `./configure --disable-bindings --enable-static`
3. 编译出 `src/.libs/libredwg.a`

### CMake 选项

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `CAD_USE_LIBREDWG_API` | ON | 启用 libredwg C API 后端 |
| `CAD_USE_LIBREDWG_CLI` | ON | 当 API 不可用时，回退到 CLI |
| `LIBREDWG_ROOT_DIR` | 空 | 指定 libredwg 源码树或安装路径 |
| `BUILD_TESTS` | OFF | 构建测试 |

### 测试

```bash
cmake -S . -B build -DBUILD_TESTS=ON
cmake --build build -j$(nproc)
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

### C++ API

```cpp
#include <cad_parser/cad_parser.h>

// === DXF 解析（纯 C++，无外部依赖） ===
cad::ParseResult result;
cad::Drawing d = cad::parse_dxf_file("floorplan.dxf", {}, &result);

// === DWG 解析（libredwg 后端） ===
cad::DwgParseResult dwg_result;
// 优先使用 C API，不可用时自动回退 CLI
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
├── scripts/
│   └── build_libredwg.sh           # libredwg 一键编译脚本
├── third_party/                    # 第三方源码目录
│   └── libredwg/                   # (git clone 后出现)
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
| 数据通道 | 内存直接访问 | `popen()` + JSON 文本 |
| 精度 | 二进制 double，无损 | JSON 序列化可能有精度损失 |
| 性能 | 单次遍历，O(n) | 子进程 + JSON 解析 |
| 部署 | 链接静态库，单文件 | 依赖 `dwgread` 在 PATH |

## License

MIT
