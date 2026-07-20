#include "cad_parser/dxf_parser.h"
#include "svg_exporter.h"

#include <cmath>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

const char* fixture = R"DXF(0
SECTION
2
TABLES
0
TABLE
2
LAYER
0
LAYER
2
Red
70
0
62
1
6
Continuous
0
ENDTAB
0
ENDSEC
0
SECTION
2
BLOCKS
0
BLOCK
2
CURVED_BLOCK
10
10
20
10
30
0
0
LWPOLYLINE
8
0
70
0
10
10
20
10
42
0.414213562373095
10
20
20
10
0
ENDBLK
0
ENDSEC
0
SECTION
2
ENTITIES
0
MTEXT
8
Red
10
0
20
0
40
2.5
3
Hello 
1
World\PSecond line
0
SPLINE
8
Red
70
0
71
3
72
6
73
2
10
0
20
0
30
0
10
5
20
5
30
0
40
0
40
0
40
0
40
1
40
1
40
1
0
POLYLINE
8
Red
70
1
0
VERTEX
10
0
20
0
30
3
0
VERTEX
10
10
20
0
30
3
0
SEQEND
0
LWPOLYLINE
8
Red
70
0
10
0
20
20
42
1
10
10
20
20
0
ELLIPSE
8
Red
10
20
20
20
11
5
21
0
40
0.5
41
0
42
3.141592653589793
0
INSERT
8
Red
2
CURVED_BLOCK
10
30
20
40
41
2
42
2
50
90
0
HATCH
8
Red
2
SOLID
70
1
71
0
91
1
92
2
72
0
73
1
93
4
10
40
20
40
10
50
20
40
10
50
20
50
10
40
20
50
97
0
0
TEXT
8
Red
10
10
20
20
11
50
21
60
40
5
1
Centered text
72
1
73
2
0
ENDSEC
0
EOF
)DXF";

void test_parser() {
    cad::ParseResult result = cad::ParseResult::InternalError;
    cad::Drawing drawing = cad::parse_dxf_string(fixture, {}, &result);
    expect(result == cad::ParseResult::Success, "complete fixture parses successfully");
    expect(drawing.entities.size() == 8, "all top-level entities are retained");
    expect(drawing.blocks.size() == 1, "block definition is parsed");

    const auto& block_polyline = std::get<cad::LwPolylineEntity>(
        drawing.blocks.at("CURVED_BLOCK").entities.at(0).entity);
    expect(block_polyline.vertices.size() == 2,
           "repeated block polyline coordinates are not lost");

    const auto& mtext = std::get<cad::MTextEntity>(drawing.entities.at(0).entity);
    expect(mtext.value == "Hello World\\PSecond line",
           "MTEXT continuation chunks preserve source order");

    const auto& spline = std::get<cad::SplineEntity>(drawing.entities.at(1).entity);
    expect(spline.control_points.size() == 2, "SPLINE control points are extracted");
    expect(spline.knots.size() == 6, "SPLINE knot vector is extracted");

    const auto& converted = std::get<cad::LwPolylineEntity>(drawing.entities.at(2).entity);
    expect(converted.vertices.size() == 2 && converted.closed,
           "legacy POLYLINE converts to a closed lightweight polyline by default");
    expect(std::abs(converted.elevation - 3.0) < 1e-9,
           "legacy POLYLINE elevation survives conversion");

    const auto& ellipse = std::get<cad::EllipseEntity>(drawing.entities.at(4).entity);
    expect(std::abs(ellipse.end_angle - 180.0) < 1e-9,
           "DXF ellipse radians are normalized to degrees");

    const auto& hatch = std::get<cad::HatchEntity>(drawing.entities.at(6).entity);
    expect(hatch.boundary_loops.size() == 1 && hatch.boundary_loops[0].size() == 4,
           "HATCH polyline boundary vertices are extracted");

    const auto& text = std::get<cad::TextEntity>(drawing.entities.at(7).entity);
    expect(text.has_alignment_point && text.alignment_point.x == 50.0 &&
               text.alignment_point.y == 60.0,
           "TEXT secondary alignment point is retained");
    expect(text.horizontal_alignment == 1 && text.vertical_alignment == 2,
           "TEXT alignment modes are retained");

    cad::DxfParserOptions options;
    options.convert_heavy_polylines = false;
    drawing = cad::parse_dxf_string(fixture, options, &result);
    expect(std::holds_alternative<cad::PolylineEntity>(drawing.entities.at(2).entity),
           "legacy POLYLINE conversion option is honored");
}

void test_svg() {
    cad::ParseResult result;
    const cad::Drawing drawing = cad::parse_dxf_string(fixture, {}, &result);
    const std::string svg = cad::export_svg(drawing);
    expect(svg.find("stroke=\"#FF0000\"") != std::string::npos,
           "SVG uses the layer ACI color");
    expect(svg.find("<tspan") != std::string::npos &&
           svg.find("Second line") != std::string::npos &&
           svg.find("\\P") == std::string::npos,
           "SVG cleans and splits MTEXT");
    expect(svg.find(" A ") != std::string::npos,
           "SVG emits arc commands for polyline bulges");
    expect(svg.find("<g transform=\"matrix(") != std::string::npos,
           "SVG expands INSERT block geometry in SVG coordinates");

    cad::Drawing invalid_text;
    cad::TextEntity text;
    text.value = std::string(1, static_cast<char>(0xFF));
    text.height = 1.0;
    cad::EntityRecord invalid_record;
    invalid_record.entity = text;
    invalid_record.layer_name = "0";
    invalid_text.entities.push_back(std::move(invalid_record));
    const std::string sanitized = cad::export_svg(invalid_text);
    expect(sanitized.find(static_cast<char>(0xFF)) == std::string::npos,
           "SVG replaces invalid text bytes with valid UTF-8");
}

void test_invalid_input() {
    cad::ParseResult result;
    cad::parse_dxf_string("0\nEOF\n", {}, &result);
    expect(result == cad::ParseResult::InvalidFormat,
           "input without a DXF section is rejected");

    cad::parse_dxf_string("0\nSECTION\n2\nENTITIES\n0\nENDSEC\n", {}, &result);
    expect(result == cad::ParseResult::TruncatedFile,
           "missing EOF is reported as truncated");

    cad::parse_dxf_string("0\nSECTION\n2", {}, &result);
    expect(result == cad::ParseResult::TruncatedFile,
           "dangling group code is reported as truncated");
}

} // namespace

int main() {
    test_parser();
    test_svg();
    test_invalid_input();
    if (failures != 0) {
        std::cerr << failures << " regression test(s) failed\n";
        return 1;
    }
    std::cout << "All CAD parser regression tests passed\n";
    return 0;
}
