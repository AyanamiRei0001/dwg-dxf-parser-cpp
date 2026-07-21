# CAD Parser API 与架构开发指南

本指南描述当前代码库的实际行为，面向两类读者：使用 `cad_parser` 解析 CAD 文件的
C++ 调用方，以及需要新增实体、修复显示或定位 DXF/DWG 差异的开发者。

快速构建和 CLI 命令见 [README](../README.md)。本文中的“当前”均指仓库现有实现，
不会把尚未落地的接口字段描述成已支持能力。

## 1. 设计边界

项目将 DXF 和 DWG 归一化为同一个 `cad::Drawing`。调用方不必直接处理 DXF group
code 或 LibreDWG 的 `Dwg_Object`，而是遍历 `std::variant` 形式的实体。

```text
ASCII DXF  ──> DXF 组码解析 ────────┐
                                   │
DWG ──> LibreDWG C API 或 dwgread ─┼──> cad::Drawing
                                   │        ├── C++ 业务代码
                                   │        ├── cad-parser-cli
                                   │        ├── SVG / HTML 导出
                                   │        └── Qt 查看器
```

范围和原则如下：

- DXF 输入是 ASCII DXF，头文件声明的版本范围为 R12 至 R2018。
- DWG 是通过 GNU LibreDWG 读取的二进制格式；可读取的版本和字段完整度取决于该依赖。
- 解析器保留原始几何单位，不进行毫米、米、英寸等单位换算。
- 核心几何坐标采用 CAD 笛卡尔坐标：X 向右、Y 向上；只有显示层会翻转 Y 轴。
- 未识别的 DXF 实体会跳过，不会让整个文件失败。调用方应依据状态码和实体统计判断结果是否适合业务使用。

## 2. 头文件与接口边界

| 位置 | 用途 | 建议 |
|------|------|------|
| `include/cad_parser/cad_parser.h` | 统一入口，包含数据模型、DXF 与 DWG API | 外部项目优先包含它 |
| `include/cad_parser/common_types.h` | `Drawing`、实体、图层、块和颜色 | 需要直接操作模型时包含 |
| `include/cad_parser/dxf_parser.h` | DXF 解析、状态码与选项 | 需要 DXF 专用控制时包含 |
| `include/cad_parser/dwg_parser.h` | DWG 解析、状态码与选项 | 需要 DWG 专用控制时包含 |
| `src/svg_exporter.h` | SVG 导出声明 | 当前供仓库内 CLI/测试使用，尚未安装到 `include/` |

`cmake --install` 目前只安装 `include/` 下的头文件。因此 SVG 导出符号虽然编入
`cad_parser` 静态库，`src/svg_exporter.h` 还不是已安装 SDK 的完整公共头文件。若将库
分发给外部项目，建议先将该头文件迁至 `include/cad_parser/` 并相应调整 include 路径。

## 3. 统一数据模型

### 3.1 顶层结构

```cpp
cad::Drawing drawing;

// 文件元数据、范围和计数
cad::DrawingInfo info = drawing.info;

// 模型空间顶层实体。INSERT 在此处仍是 INSERT，不会被解析器展开。
for (const cad::EntityRecord& record : drawing.entities) {
    std::cout << record.layer_name << " "
              << cad::entity_type_name(record.entity) << '\n';
}

// 图层和块定义按名称索引。
const auto& layers = drawing.layers;
const auto& blocks = drawing.blocks;
```

`Drawing` 的成员职责如下：

| 成员 | 含义 |
|------|------|
| `info` | 文件名、格式、版本、解析出的范围以及实体/图层/块计数 |
| `entities` | 顶层实体的有序列表，每项带实体公共属性 |
| `layers` | `std::map<std::string, Layer>`，按图层名查找颜色、线型、冻结和可见状态 |
| `blocks` | `std::map<std::string, BlockRecord>`，保存块基点和块内实体 |

`EntityRecord` 将几何内容与通用 CAD 属性分开保存：

```cpp
struct EntityRecord {
    Entity entity;                 // 几何，std::variant
    std::string layer_name = "0";
    Color color;                   // ACI / BYLAYER / BYBLOCK 信息
    std::string line_type_name;
    LineWeight line_weight;
    std::string handle;            // DXF handle / DWG object ID
};
```

使用 `std::visit` 是遍历实体最稳妥的方式：

```cpp
for (const cad::EntityRecord& record : drawing.entities) {
    std::visit([](const auto& entity) {
        using T = std::decay_t<decltype(entity)>;
        if constexpr (std::is_same_v<T, cad::LineEntity>) {
            std::cout << entity.start.x << "," << entity.start.y
                      << " -> " << entity.end.x << "," << entity.end.y << '\n';
        } else if constexpr (std::is_same_v<T, cad::TextEntity>) {
            std::cout << entity.value << '\n';
        }
    }, record.entity);
}
```

当调用方只关心一种实体时，也可以先用 `std::get_if<T>(&record.entity)`，避免对类型
不匹配的 `std::get<T>` 抛出异常。

### 3.2 几何与单位约定

- `Point2D` 和 `Point3D` 使用 `double`。大多数 2D 显示路径使用 X/Y，但 Z 会保留在相应实体中。
- `BoundingBox2D` 只提供宽高计算；它不会由解析器自动附着到每个实体。
- 除 DXF 椭圆的源参数会从弧度转换外，模型中旋转和角度字段均以**度**表示，例如 `ArcEntity`、`TextEntity` 与 `InsertEntity`。
- `Color::index` 记录 AutoCAD Color Index。`0` 表示 BYBLOCK，`256` 表示 BYLAYER，使用 RGB 时为 `-1`。渲染器据此再结合图层颜色解析最终颜色。

### 3.3 支持的实体

`Entity` 是以下 14 种 C++ 类型的 `std::variant`：

| CAD 实体 | C++ 类型 | 关键字段 |
|------|------|------|
| `LINE` | `LineEntity` | `start`、`end`、`thickness` |
| `CIRCLE` | `CircleEntity` | `center`、`radius` |
| `ARC` | `ArcEntity` | `center`、`radius`、起止角度 |
| `ELLIPSE` | `EllipseEntity` | 中心、相对中心的长轴端点、轴比、起止角度 |
| `LWPOLYLINE` | `LwPolylineEntity` | 2D 顶点、`bulge`、宽度、闭合状态 |
| `POLYLINE` | `PolylineEntity` | 3D 顶点、`bulge`、闭合状态 |
| `TEXT` | `TextEntity` | 插入点、对齐点、文字、高度、旋转和对齐模式 |
| `MTEXT` | `MTextEntity` | 插入点、文本框宽度、高度、附着点 |
| `INSERT` | `InsertEntity` | 块名、插入点、比例、旋转和阵列参数 |
| `POINT` | `PointEntity` | 位置 |
| `SOLID` / `3DFACE` | `SolidEntity` | 四个顶点 |
| `SPLINE` | `SplineEntity` | 控制点、拟合点、节点向量、次数 |
| `DIMENSION` | `DimensionEntity` | 简化的定义点、文字中点、覆盖文本 |
| `HATCH` | `HatchEntity` | 图案属性和边界环 |

### 3.4 块与 INSERT

解析阶段会把 `BLOCKS` 段保存到 `Drawing::blocks`，顶层 `INSERT` 仍保持引用形式。这样
可以保留块定义并避免对每个插入实例复制几何数据。

SVG 导出器与 Qt 查看器会在**渲染时**递归展开 `INSERT`，处理块基点、缩放、旋转、行列阵列以及
块内嵌套插入，并通过活动块集合避免循环引用导致无限递归。业务代码若需要“展开后的实体列表”，
需要自行遍历 `drawing.blocks` 并应用同类仿射变换；当前解析器不提供这个列表。

## 4. DXF API

### 4.1 常用调用

```cpp
#include <cad_parser/cad_parser.h>

cad::DxfParserOptions options;
options.convert_heavy_polylines = true;
options.max_entities = 10'000;
options.on_progress = [](size_t pairs, size_t entities) {
    std::cerr << "processed group pairs: " << pairs
              << ", entities: " << entities << '\n';
};

cad::ParseResult result;
cad::Drawing drawing = cad::parse_dxf_file("floorplan.dxf", options, &result);
if (result != cad::ParseResult::Success) {
    throw std::runtime_error(cad::to_string(result));
}
```

可用函数：

| 函数 | 适用场景 |
|------|----------|
| `parse_dxf_file(path, options, result)` | 从文件解析完整 DXF |
| `parse_dxf_string(content, options, result)` | 已在内存中的 DXF 内容，例如网络或测试夹具 |
| `peek_dxf_header(path, result)` | 仅扫描 `HEADER` 段获取版本和 extents，不构建实体、图层或块 |
| `to_string(ParseResult)` | 将状态码转为可打印文本 |

`ParseResult` 的状态依次覆盖成功、文件不存在、读文件失败、格式非法、版本不支持、文件截断和
内部错误。即使函数返回了一个 `Drawing`，也应先检查状态码；失败时它可能只是带有基础 `info` 的
空对象。

### 4.2 `DxfParserOptions`

| 字段 | 当前行为 |
|------|----------|
| `convert_heavy_polylines` | 默认 `true`。将传统 `POLYLINE`/`VERTEX` 转为 `LwPolylineEntity`；设为 `false` 可保留 `PolylineEntity` 和 Z 坐标。 |
| `max_entities` | 默认 `0`，表示不限制。大于 0 时限制顶层实体数，适合做快速预览。 |
| `on_progress` | 在常规支持实体解析后调用；传统 `POLYLINE` 的完成路径不保证单独回调。第一个参数是已处理的 DXF group-pair 索引，不是物理文本行号；第二个参数是当前顶层实体数。 |
| `expand_inserts` | 目前只是预留字段，解析器尚未读取它；设为 `true` 不会展开 `INSERT`。渲染器会在显示阶段展开块。 |

DXF 解析会处理 `HEADER`、`TABLES`、`BLOCKS` 和 `ENTITIES` 段。`MTEXT` 的连续 group code 会按
源顺序拼接，`LWPOLYLINE`/`POLYLINE` 的 bulge 和顶点宽度会保留，`HATCH` 会提取边界环。

## 5. DWG API 与后端选择

### 5.1 调用方式

```cpp
cad::DwgParserOptions options;
options.dwgread_path = "/opt/libredwg/bin/dwgread";  // 只在 CLI 后端有意义
options.output_format = "json";                      // CLI 默认值

cad::DwgParseResult result;
cad::Drawing drawing = cad::parse_dwg_file("layout.dwg", options, &result);
if (result != cad::DwgParseResult::Success) {
    std::cerr << cad::to_string(result) << '\n';
}
```

辅助函数：

| 函数 | 作用 |
|------|------|
| `parse_dwg_file` | 通过当前构建选择的后端解析完整 DWG |
| `peek_dwg_header` | 获取 DWG 元信息；CLI 路径仍需运行 `dwgread` 并读取 JSON |
| `is_libredwg_available` | API 构建时检查已链接后端，CLI 构建时在 PATH/常见位置寻找 `dwgread` |
| `to_string(DwgParseResult)` | 将 DWG 状态转为可打印文本 |

`DwgParseResult` 除一般文件/格式错误外，还可报告 `LibreDwgNotFound` 和 `LibreDwgError`。

### 5.2 CMake 后端决策

配置阶段的决策是：

```text
CAD_USE_LIBREDWG_API=ON 且找到 libredwg  ->  C API 后端
否则，CAD_USE_LIBREDWG_CLI=ON             ->  dwgread CLI 后端
否则                                      ->  无 DWG 后端
```

C API 直接调用 `dwg_read_file()` 并把 LibreDWG 对象转换为统一模型，适合性能和字段完整度
要求较高的场景。CLI 后端启动 `dwgread` 子进程，再解析 JSON 或转换出的 DXF。两者不是运行期主备：
若最终可执行文件已选中 C API，它遇到解析失败不会再调用 CLI。

建议通过 CMake 输出中的 `DWG backend:` 确认实际选择，并用以下命令检查：

```bash
./build/cad-parser-cli --check-dwg
```

### 5.3 `DwgParserOptions` 的有效范围

| 字段 | 当前行为 |
|------|----------|
| `dwgread_path` | 仅 CLI 后端使用。为空时在 PATH 和几个常见位置寻找 `dwgread`。 |
| `output_format` | 仅 CLI 后端使用。`json` 与 `dxf` 有实现：前者走内置 JSON 转换，后者先由 `dwgread` 输出 DXF 再复用 DXF 解析器。`svg` 虽出现在头文件注释中，但当前没有对应解析分支，不应传入。 |
| `on_progress` | 类型已声明，但当前 C API 和 CLI 实现均未触发回调。 |

## 6. 便利入口与 CLI

### 6.1 `parse_file`

`cad::parse_file(path)` 是仅适合简单工具的便利函数：扩展名为 `.dwg` 时走 DWG，其他所有扩展名
都按 DXF 处理。它不向调用方返回 `ParseResult` / `DwgParseResult`，因此生产代码应优先调用
`parse_dxf_file` 或 `parse_dwg_file` 并检查状态。

### 6.2 `cad-parser-cli`

```bash
./build/cad-parser-cli drawing.dxf
./build/cad-parser-cli drawing.dwg --count
./build/cad-parser-cli drawing.dxf --layers --blocks
./build/cad-parser-cli drawing.dxf --verbose --max 20
./build/cad-parser-cli drawing.dwg --svg preview.svg --no-text
./build/cad-parser-cli drawing.dxf --html preview.html
```

| 选项 | 作用 |
|------|------|
| `--verbose`, `-v` | 输出实体详细字段，同时输出图层和块信息 |
| `--layers`, `-l` | 输出图层表 |
| `--count`, `-c` | 只输出各实体类型计数 |
| `--blocks`, `-b` | 输出块定义、基点和块内实体数 |
| `--max N` | 限制 CLI 预览输出条数，不改变解析量 |
| `--svg`, `-s FILE` | 从解析结果导出 SVG |
| `--html`, `-H FILE` | 生成内嵌 SVG、支持缩放和平移的 HTML 文件 |
| `--no-text`, `-T` | SVG/HTML 导出时隐藏 `TEXT` 和 `MTEXT` |
| `--check-dwg` | 检查当前 DWG 后端是否可用 |

## 7. 坐标系与文本对齐

### 7.1 坐标系

模型层是 CAD 笛卡尔坐标系：X 增大向右，Y 增大向上。解析时不会翻转坐标，也不会重定位到原点。

Qt 和 SVG 的屏幕坐标 Y 向下，因此显示时分别做了以下变换：

| 目标 | 从 CAD `(x, y)` 的映射 |
|------|--------------------------|
| Qt `QGraphicsScene` | `(x, -y)` |
| SVG | `((x - min_x) * sx, (max_y - y) * sy)` |

因此，CAD 中逆时针的弧在 SVG/Qt 中仍会保持正确视觉方向，代码会同时调整 Y 与旋转/扫描方向。
目前 Qt 查看器没有绘制 X/Y 轴或网格；笛卡尔坐标约定体现在解析模型和上述渲染变换中，而不是画面上的
坐标轴。状态栏中的 `x: -- y: --` 目前也只是占位文字，尚未接入鼠标坐标跟踪。

### 7.2 `TEXT` 的锚点与对齐

`TextEntity` 同时保存：

- `insertion_point`：DXF group code 10 的插入点。
- `alignment_point`：DXF group code 11 的第二对齐点。
- `has_alignment_point`：DXF 中存在第二点时为真；DWG C API 路径在非默认对齐时设置。
- `horizontal_alignment`：0 左、1 中、2 右、3 aligned、4 middle、5 fit。
- `vertical_alignment`：0 baseline、1 bottom、2 middle、3 top。

Qt 查看器使用对齐点处理常见的居中/右对齐/垂直对齐，并把默认 CAD baseline 转成 Qt 文本项目的左上角。
字体实际字形仍取决于系统的 CJK 字体回退和 `QFontMetricsF`，所以不同字体或缺字回退可能出现轻微视觉偏差。
`aligned` 与 `fit` 是双点宽度模式，当前查看器不拉伸或压缩字宽，仍以插入点定位；这是有意保守的显示近似。

SVG 导出器当前对 `TEXT` 使用 `insertion_point`，尚未应用 `alignment_point`、水平/垂直对齐字段；
`MTEXT` 的 attachment point 也未完整实现。若比较 SVG 与 Qt 截图时文本位置不同，应先按这一差异判断，
不应据此认为解析坐标本身出错。

## 8. SVG 与 Qt 查看器

### 8.1 SVG 导出

仓库内可使用：

```cpp
#include "svg_exporter.h"  // 当前位于 src/，见第 2 节的安装边界

cad::SvgOptions options;
options.width = 1600;
options.height = 1000;
options.color_by_layer = true;
options.auto_fit = true;
options.hide_text = false;

const bool written = cad::export_svg_file("drawing.svg", drawing, options);
```

| `SvgOptions` 字段 | 作用 |
|-------------------|------|
| `width` / `height` | SVG 输出画布的目标尺寸 |
| `stroke_width` | 描边宽度 |
| `color_by_layer` | 为真时按实体颜色、BYBLOCK/BYLAYER 与图层 ACI 决定颜色 |
| `default_stroke` | 不按图层着色时的统一颜色 |
| `background` | 背景填充色 |
| `auto_fit` | 为真时基于实际实体和展开后的 INSERT 计算范围，并留 5% 边距 |
| `show_labels` | 预留调试字段，当前导出器未绘制实体标签 |
| `hide_text` | 跳过 `TEXT` 与 `MTEXT` |

导出器会安全转义 XML 文本并替换无效 UTF-8。它会递归渲染块引用，但有若干有意简化：

- `SPLINE` 绘制为控制点连线，而不是 NURBS 曲线。
- `HATCH` 只绘制第一个边界环的半透明填充，不复现图案。
- `DIMENSION` 只绘制简化引线和覆盖文字。
- `ELLIPSE` 输出为完整椭圆，部分椭圆裁剪未实现。
- 文本对齐限制见第 7 节。

### 8.2 Qt 查看器

当构建机安装了 Qt5 Widgets 且 `BUILD_QT_VIEWER=ON` 时，CMake 会生成 `cad-viewer`。

```bash
./build/cad-viewer path/to/drawing.dxf
./build/cad-viewer path/to/drawing.dwg

# 适合 CI 或人工回归比图；无显示服务器时使用 offscreen 平台插件
QT_QPA_PLATFORM=offscreen ./build/cad-viewer path/to/drawing.dwg \
  --screenshot /tmp/drawing.png --screenshot-size 1600x1000
```

查看器支持打开文件、滚轮缩放、拖拽平移、适配视图、重置变换和图层可见性开关。PNG 截图使用场景内容
的包围盒并保留 24 像素边距，`--screenshot-size` 接受 `宽x高`，例如 `1600x1000`；格式无效时回退到
`1015x653`。

Qt 渲染器与 SVG 一样会处理层色、BYLAYER/BYBLOCK 和嵌套块，但样条、填充、标注以及部分文本格式都是
近似显示。视觉回归应优先比较几何、图层和块位置，再单独检查字体渲染。

## 9. 新增或修复实体的开发流程

新增一种 CAD 实体时，建议按数据流完成改动，避免“能解析但不能显示”或“DXF 正常、DWG 丢失”的半实现：

1. 在 `include/cad_parser/common_types.h` 增加实体结构，并把它加入 `Entity` variant；在 `src/common_types.cpp` 增加 `entity_type_name` 分支。
2. 在 `src/dxf_parser.cpp` 实现 group code 到结构字段的转换，并在 `ENTITIES` 和需要时的 `BLOCKS` 解析分派中接入它。
3. 在 `src/dwg_api_impl.cpp` 增加 LibreDWG C API 转换；若仍支持 CLI JSON 路径，也在 `src/dwg_parser.cpp` 的 JSON 转换中接入。
4. 明确实体在 `INSERT` 内的坐标/颜色继承规则，确认其能放入 `BlockRecord::entities`。
5. 在 `src/svg_exporter.cpp` 添加范围计算和 SVG 渲染分支；在 `src/qt_viewer/CadViewer.cpp` 添加 Qt 绘制分支，并在这里统一处理 `-y`。
6. 在 `test/parser_tests.cpp` 增加最小 DXF 夹具和字段断言；若修改导出器，再断言 SVG 的关键元素或变换。
7. 用真实 DWG 走 C API/CLI 路径验证，必要时生成固定尺寸 PNG 与参考截图比对。

对于文本问题，先依次确认：解析出的 `value` 是否为正确 UTF-8、`height`/`rotation` 是否正确、第二对齐点是否存在、
对齐模式是否是 `aligned`/`fit`，最后才检查 Qt 字体与度量。这样能把解析问题、坐标问题和字体渲染问题分开。

## 10. 测试与排查

启用并运行现有回归测试：

```bash
cmake -S . -B build -DBUILD_TESTS=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

现有测试覆盖 DXF 基础解析、重型多段线转换、曲线 bulge、块、MTEXT 拼接、样条节点、HATCH 边界、
`TEXT` 第二对齐点、SVG 颜色/块变换/文本清洗以及若干非法输入。

排查某张图显示不一致时，可按以下顺序缩小范围：

1. 用 `cad-parser-cli file --count --layers --blocks` 确认实体、图层和块的数量。
2. 用 `--verbose --max N` 抽查关键实体的坐标、文字、图层和块名。
3. 分别导出 SVG 与 Qt 固定尺寸截图，判断差异是否只发生在某个渲染器。
4. 对 DWG 确认 CMake 实际后端；CLI 与 C API 的 LibreDWG 字段覆盖可能不同。
5. 将能稳定复现的问题缩成最小 DXF 夹具，加入 `test/parser_tests.cpp` 后再改实现。

## 11. 已知限制

- DXF 仅支持 ASCII 输入；二进制 DXF 不在当前范围内。
- DWG 的可读性受 LibreDWG 版本和后端影响，未知对象可能被跳过。
- `DxfParserOptions::expand_inserts`、`SvgOptions::show_labels` 和 `DwgParserOptions::on_progress` 目前是未接线的预留能力。
- CLI 的 `DwgParserOptions::output_format` 仅支持 `json` 与 `dxf`；不要传 `svg`。
- SVG 导出头文件尚未随安装包导出。
- 样条、图案填充、标注和部分 MTEXT/TEXT 对齐为近似显示，不应作为 CAD 编辑器级保真输出。
- 当前 Qt 界面没有显式坐标轴/网格，也没有鼠标 CAD 坐标读数。

这些限制适合作为后续迭代的任务清单。新增功能时请同时更新本指南、README 中的支持矩阵和对应回归测试。
