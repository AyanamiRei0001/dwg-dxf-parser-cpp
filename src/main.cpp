/**
 * @file main.cpp
 * @brief CLI demo tool for the CAD parser library.
 *
 * Usage:
 *   cad-parser-cli <file.dxf>              Parse and summarize
 *   cad-parser-cli <file.dxf> --verbose    Full entity dump
 *   cad-parser-cli <file.dxf> --layers     List layers
 *   cad-parser-cli <file.dxf> --count      Entity counts only
 *   cad-parser-cli <file.dwg>              Auto-detects DWG (needs libredwg)
 *   cad-parser-cli --check-dwg             Check if libredwg is available
 */

#include "cad_parser/cad_parser.h"
#include "svg_exporter.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// =========================================================================
//  Utility
// =========================================================================

namespace {

struct CliOptions {
    std::string filepath;
    std::string svg_output;     // if set, export SVG to this path
    std::string html_output;    // if set, export interactive HTML viewer
    bool verbose        = false;
    bool layers_only    = false;
    bool count_only     = false;
    bool check_dwg      = false;
    bool show_blocks    = false;
    bool no_text        = false;     // skip text in SVG export
    int  max_preview    = 20;
};

void print_usage(const char* prog) {
    std::cout << "CAD Parser — Demo CLI Tool\n"
              << "===========================\n\n"
              << "Usage:\n"
              << "  " << prog << " <file.dxf|file.dwg> [options]\n\n"
              << "Options:\n"
              << "  --verbose, -v     Show full entity details\n"
              << "  --layers, -l      List layers only\n"
              << "  --count, -c       Show entity counts only\n"
              << "  --blocks, -b      List block definitions\n"
              << "  --max N           Max entities to preview (default 20)\n"
              << "  --svg, -s FILE    Export drawing to SVG for visualization\n"
              << "  --html, -H FILE   Export interactive HTML viewer (zoom/pan)\n"
              << "  --no-text, -T     Hide text annotations in SVG/HTML export\n"
              << "  --check-dwg       Check if libredwg DWG support is available\n"
              << "  --help, -h        Show this help\n\n"
              << "Examples:\n"
              << "  " << prog << " floorplan.dxf\n"
              << "  " << prog << " floorplan.dxf --svg output.svg\n"
              << "  " << prog << " mechanical.dwg --svg preview.svg\n"
              << std::endl;
}

void parse_args(int argc, char* argv[], CliOptions& opts) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            std::exit(0);
        } else if (arg == "--verbose" || arg == "-v") {
            opts.verbose = true;
        } else if (arg == "--layers" || arg == "-l") {
            opts.layers_only = true;
        } else if (arg == "--count" || arg == "-c") {
            opts.count_only = true;
        } else if (arg == "--no-text" || arg == "-T") {
            opts.no_text = true;
        } else if (arg == "--blocks" || arg == "-b") {
            opts.show_blocks = true;
        } else if (arg == "--check-dwg") {
            opts.check_dwg = true;
        } else if ((arg == "--svg" || arg == "-s") && i + 1 < argc) {
            opts.svg_output = argv[++i];
        } else if ((arg == "--html" || arg == "-H") && i + 1 < argc) {
            opts.html_output = argv[++i];
        } else if (arg == "--max" && i + 1 < argc) {
            opts.max_preview = std::stoi(argv[++i]);
        } else if (arg[0] != '-') {
            opts.filepath = arg;
        }
    }
}

// ---- Entity visitor for printing ----

struct EntityPrinter {
    int indent = 2;

    std::string indent_str() const {
        return std::string(indent * 2, ' ');
    }

    void operator()(const cad::LineEntity& e) const {
        std::cout << indent_str() << "Start: (" << e.start.x << ", " << e.start.y << ", " << e.start.z << ")\n";
        std::cout << indent_str() << "End:   (" << e.end.x << ", " << e.end.y << ", " << e.end.z << ")\n";
    }
    void operator()(const cad::CircleEntity& e) const {
        std::cout << indent_str() << "Center: (" << e.center.x << ", " << e.center.y << ", " << e.center.z << ")\n";
        std::cout << indent_str() << "Radius: " << e.radius << "\n";
    }
    void operator()(const cad::ArcEntity& e) const {
        std::cout << indent_str() << "Center: (" << e.center.x << ", " << e.center.y << ", " << e.center.z << ")\n";
        std::cout << indent_str() << "Radius: " << e.radius
                  << ", Angle: " << e.start_angle << "° → " << e.end_angle << "°\n";
    }
    void operator()(const cad::EllipseEntity& e) const {
        std::cout << indent_str() << "Center: (" << e.center.x << ", " << e.center.y << ", " << e.center.z << ")\n";
        std::cout << indent_str() << "Major axis: (" << e.major_axis_endpoint.x << ", "
                  << e.major_axis_endpoint.y << ", " << e.major_axis_endpoint.z << "), "
                  << "Ratio: " << e.axis_ratio << "\n";
    }
    void operator()(const cad::LwPolylineEntity& e) const {
        std::cout << indent_str() << "Vertices: " << e.vertices.size()
                  << (e.closed ? " (closed)" : " (open)") << "\n";
        int show = std::min(static_cast<int>(e.vertices.size()), 5);
        for (int i = 0; i < show; ++i) {
            auto& v = e.vertices[i];
            std::cout << indent_str() << "  [" << i << "] (" << v.position.x << ", " << v.position.y << ")";
            if (std::abs(v.bulge) > 1e-8) std::cout << " bulge=" << v.bulge;
            std::cout << "\n";
        }
        if (static_cast<int>(e.vertices.size()) > show) {
            std::cout << indent_str() << "  ... +" << (e.vertices.size() - show) << " more\n";
        }
    }
    void operator()(const cad::PolylineEntity& e) const {
        std::cout << indent_str() << "Vertices: " << e.vertices.size()
                  << (e.closed ? " (closed)" : " (open)") << "\n";
    }
    void operator()(const cad::TextEntity& e) const {
        std::cout << indent_str() << "\"" << e.value << "\"\n";
        std::cout << indent_str() << "At: (" << e.insertion_point.x << ", "
                  << e.insertion_point.y << ", " << e.insertion_point.z << ")"
                  << ", Height: " << e.height << ", Rotation: " << e.rotation << "°\n";
    }
    void operator()(const cad::MTextEntity& e) const {
        std::cout << indent_str() << "\"" << e.value << "\"\n";
        std::cout << indent_str() << "At: (" << e.insertion_point.x << ", "
                  << e.insertion_point.y << ", " << e.insertion_point.z << ")"
                  << ", Height: " << e.height << "\n";
    }
    void operator()(const cad::InsertEntity& e) const {
        std::cout << indent_str() << "Block: \"" << e.block_name << "\"\n";
        std::cout << indent_str() << "At: (" << e.insertion_point.x << ", "
                  << e.insertion_point.y << ", " << e.insertion_point.z << ")"
                  << ", Scale: (" << e.scale_x << ", " << e.scale_y << ", " << e.scale_z << ")"
                  << ", Rotation: " << e.rotation << "°\n";
    }
    void operator()(const cad::PointEntity& e) const {
        std::cout << indent_str() << "(" << e.position.x << ", " << e.position.y << ", " << e.position.z << ")\n";
    }
    void operator()(const cad::SolidEntity& e) const {
        std::cout << indent_str() << "P1: (" << e.p1.x << ", " << e.p1.y << ", " << e.p1.z << ")\n";
        std::cout << indent_str() << "P2: (" << e.p2.x << ", " << e.p2.y << ", " << e.p2.z << ")\n";
        std::cout << indent_str() << "P3: (" << e.p3.x << ", " << e.p3.y << ", " << e.p3.z << ")\n";
        std::cout << indent_str() << "P4: (" << e.p4.x << ", " << e.p4.y << ", " << e.p4.z << ")\n";
    }
    void operator()(const cad::SplineEntity& e) const {
        std::cout << indent_str() << "Degree: " << e.degree
                  << (e.closed ? ", closed" : "") << (e.periodic ? ", periodic" : "") << "\n";
        std::cout << indent_str() << "Control points: " << e.control_points.size()
                  << ", Fit points: " << e.fit_points.size()
                  << ", Knots: " << e.knots.size() << "\n";
    }
    void operator()(const cad::DimensionEntity& e) const {
        std::cout << indent_str() << "Type: " << e.type;
        if (!e.text_override.empty()) std::cout << ", Text: \"" << e.text_override << "\"";
        std::cout << "\n";
    }
    void operator()(const cad::HatchEntity& e) const {
        std::cout << indent_str() << "Pattern: \"" << e.pattern_name
                  << "\", Scale: " << e.pattern_scale
                  << ", Boundary loops: " << e.boundary_loops.size() << "\n";
    }
};

// ---- Entity type counting ----

struct EntityTypeCounter {
    std::map<std::string, int> counts;

    void count(const cad::Entity& e) {
        counts[cad::entity_type_name(e)]++;
    }

    void print() const {
        std::cout << "\nEntity Type Breakdown:\n";
        std::cout << "----------------------\n";
        for (auto& [name, cnt] : counts) {
            std::cout << "  " << std::left << std::setw(16) << name << std::right << std::setw(6) << cnt << "\n";
        }
        std::cout << std::endl;
    }
};

// ---- Layer info ----

std::string lw_name(cad::LineWeight lw) {
    switch (lw) {
        case cad::LineWeight::BYLAYER: return "ByLayer";
        case cad::LineWeight::BYBLOCK: return "ByBlock";
        case cad::LineWeight::DEFAULT: return "Default";
        default: return std::to_string(static_cast<int>(lw)) + "mm";
    }
}

void print_layers(const std::map<std::string, cad::Layer>& layers) {
    std::cout << "\nLayers:\n";
    std::cout << "-------\n";
    std::cout << std::left << std::setw(24) << "Name"
              << std::setw(8) << "Color"
              << std::setw(16) << "LineType"
              << std::setw(10) << "LineWt"
              << std::setw(8) << "State"
              << "\n";
    std::cout << std::string(66, '-') << "\n";

    for (auto& [name, layer] : layers) {
        std::string state;
        if (layer.frozen)  state += "F";
        if (layer.locked)  state += "L";
        if (!layer.visible) state += "H";
        if (state.empty()) state = "Active";

        std::cout << std::left << std::setw(24) << name
                  << std::setw(8) << layer.color_index
                  << std::setw(16) << layer.line_type_name
                  << std::setw(10) << lw_name(layer.line_weight)
                  << std::setw(8) << state
                  << "\n";
    }
    std::cout << std::endl;
}

void print_blocks(const std::map<std::string, cad::BlockRecord>& blocks) {
    std::cout << "\nBlock Definitions:\n";
    std::cout << "------------------\n";
    for (auto& [name, blk] : blocks) {
        std::cout << "  " << name
                  << "  Base: (" << blk.base_point.x << ", " << blk.base_point.y << ")"
                  << "  Entities: " << blk.entities.size();
        if (!blk.description.empty()) std::cout << "  \"" << blk.description << "\"";
        std::cout << "\n";
    }
    std::cout << std::endl;
}

} // anonymous namespace

// =========================================================================
//  Entry point
// =========================================================================

int main(int argc, char* argv[]) {
    CliOptions opts;
    parse_args(argc, argv, opts);

    // --check-dwg
    if (opts.check_dwg) {
        std::cout << "Checking for libredwg (dwgread)...\n";
        std::string version;
        if (cad::is_libredwg_available(&version)) {
            std::cout << "  ✅ Available: " << version << "\n";
        } else {
            std::cout << "  ❌ Not found.\n"
                      << "  Install: apt install libredwg-tools  (Ubuntu/Debian)\n"
                      << "           or build from https://www.gnu.org/software/libredwg/\n";
        }
        return 0;
    }

    // Require a file path
    if (opts.filepath.empty()) {
        print_usage(argv[0]);
        return 1;
    }

    // Determine file format from extension
    std::string ext;
    auto dot = opts.filepath.rfind('.');
    if (dot != std::string::npos) {
        ext = opts.filepath.substr(dot);
        for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    // Parse the file
    cad::Drawing drawing;

    if (ext == ".dwg") {
        std::cout << "Parsing DWG file: " << opts.filepath << " ...\n";
        cad::DwgParseResult result;
        cad::DwgParserOptions dwg_opts;
        drawing = cad::parse_dwg_file(opts.filepath, dwg_opts, &result);

        if (result != cad::DwgParseResult::Success) {
            std::cerr << "Error: " << cad::to_string(result) << "\n";
            return 1;
        }
    } else {
        // Treat as DXF
        std::cout << "Parsing DXF file: " << opts.filepath << " ...\n";
        cad::ParseResult result;
        cad::DxfParserOptions dxf_opts;

        // Progress callback
        dxf_opts.on_progress = [](size_t lines, size_t entities) {
            std::cout << "\r  Parsed " << lines << " pairs, found "
                      << entities << " entities..." << std::flush;
        };

        drawing = cad::parse_dxf_file(opts.filepath, dxf_opts, &result);

        std::cout << "\r"; // clear progress line

        if (result != cad::ParseResult::Success) {
            std::cerr << "Error: " << cad::to_string(result) << "\n";
            return 1;
        }
    }

    std::cout << "✅ Parse successful!\n\n";

    // ---- Print drawing info ----
    auto& info = drawing.info;
    std::cout << "File:      " << info.filename << "\n";
    std::cout << "Format:    " << info.format << "\n";
    if (!info.version.empty()) {
        std::cout << "Version:   " << info.version << "\n";
    }
    std::cout << "Entities:  " << info.entity_count << "\n";
    std::cout << "Layers:    " << info.layer_count << "\n";
    std::cout << "Blocks:    " << info.block_count << "\n";
    std::cout << "Extents:   (" << info.min_x << ", " << info.min_y << ") → ("
              << info.max_x << ", " << info.max_y << ")\n";

    // ---- Entity type breakdown ----
    EntityTypeCounter counter;
    for (auto& rec : drawing.entities) {
        counter.count(rec.entity);
    }
    counter.print();

    // ---- SVG Export ----
    if (!opts.svg_output.empty()) {
        std::cout << "Exporting SVG to: " << opts.svg_output << " ...\n";
        cad::SvgOptions svg_opts;
        svg_opts.color_by_layer = true;
        svg_opts.hide_text = opts.no_text;
        if (cad::export_svg_file(opts.svg_output, drawing, svg_opts)) {
            std::cout << "✅ SVG exported successfully!\n";
        } else {
            std::cerr << "❌ Failed to write SVG file.\n";
        }
    }

    // ---- HTML Export (interactive viewer) ----
    if (!opts.html_output.empty()) {
        std::cout << "Exporting HTML viewer to: " << opts.html_output << " ...\n";
        cad::SvgOptions svg_opts;
        svg_opts.color_by_layer = true;
        svg_opts.hide_text = opts.no_text;
        std::string svg_content = cad::export_svg(drawing, svg_opts);

        // Wrap SVG in interactive HTML viewer
        std::ofstream html(opts.html_output);
        html << "<!DOCTYPE html>\n<html lang=\"zh-CN\">\n<head>\n"
             << "<meta charset=\"UTF-8\">\n"
             << "<title>CAD Viewer - " << drawing.info.filename << "</title>\n"
             << "<style>\n"
             << "*{margin:0;padding:0;box-sizing:border-box;}\n"
             << "body{background:#1a1a2e;color:#eee;font-family:sans-serif;overflow:hidden;height:100vh;}\n"
             << "#t{position:fixed;top:0;left:0;right:0;z-index:100;background:#16213e;"
             << "padding:6px 14px;display:flex;align-items:center;gap:10px;border-bottom:1px solid #0f3460;}\n"
             << "#t .ti{font-weight:bold;font-size:13px;color:#e94560;}\n"
             << "#t .inf{font-size:11px;color:#aaa;}\n"
             << "#t button{background:#0f3460;color:#eee;border:1px solid #1a508b;"
             << "padding:3px 10px;border-radius:3px;cursor:pointer;font-size:11px;}\n"
             << "#t button:hover{background:#1a508b;}\n"
             << "#t span{font-size:11px;color:#ccc;}\n"
             << "#c{position:fixed;top:38px;left:0;right:0;bottom:0;cursor:grab;overflow:hidden;background:#222;}\n"
             << "#c:active,#c.pan{cursor:grabbing;}\n"
             << "#w{transform-origin:0 0;position:absolute;top:0;left:0;}\n"
             << "#w svg{display:block;}\n"
             << "</style>\n</head>\n<body>\n"
             << "<div id=\"t\">\n"
             << "<span class=\"ti\">🏗 CAD Viewer</span>\n"
             << "<span class=\"inf\">" << drawing.info.filename
             << " | " << drawing.info.entity_count << " entities"
             << " | " << drawing.info.layer_count << " layers</span>\n"
             << "<span>|</span>\n"
             << "<button onclick=\"zs(1.4)\">🔍+</button>\n"
             << "<button onclick=\"zs(1/1.4)\">🔍−</button>\n"
             << "<button onclick=\"zf()\">⊞ Fit</button>\n"
             << "<span id=\"zl\">50%</span>\n"
             << "<span style=\"margin-left:auto;font-size:10px;color:#555;\">Scroll=Zoom | Drag=Pan | DblClick=Fit</span>\n"
             << "</div>\n"
             << "<div id=\"c\"><div id=\"w\">";
        // Strip XML declaration from SVG before embedding
        size_t svg_start = svg_content.find("<svg");
        if (svg_start != std::string::npos) {
            html << svg_content.substr(svg_start);
        } else {
            html << svg_content;
        }
        html << "</div></div>\n"
             << "<script>\n"
             << "let s=0.5,px=40,py=40,dp=0,sx,sy,ox,oy;\n"
             << "function u(){w.style.transform=`translate(${px}px,${py}px) scale(${s})`;zl.textContent=Math.round(s*100)+'%';}\n"
             << "c.onwheel=e=>{e.preventDefault();let r=c.getBoundingClientRect();"
             << "let mx=e.clientX-r.left,my=e.clientY-r.top;"
             << "let ns=Math.min(50,Math.max(0.01,s*(e.deltaY<0?1.15:1/1.15)));"
             << "px=mx-(mx-px)*(ns/s);py=my-(my-py)*(ns/s);s=ns;u();};\n"
             << "c.onmousedown=e=>{if(e.button)return;dp=1;c.classList.add('pan');sx=e.clientX;sy=e.clientY;ox=px;oy=py;};\n"
             << "onmousemove=e=>{if(!dp)return;px=ox+(e.clientX-sx);py=oy+(e.clientY-sy);u();};\n"
             << "onmouseup=()=>{dp=0;c.classList.remove('pan');};\n"
             << "c.ondblclick=()=>zf();\n"
             << "function zs(f){s=Math.min(50,Math.max(0.01,s*f));u();}\n"
             << "function zf(){s=0.35;px=30;py=40;u();}\n"
             << "u();\n"
             << "</script>\n</body>\n</html>\n";
        html.close();
        if (html.good()) {
            std::cout << "✅ HTML viewer exported! Open with any browser:\n";
            std::cout << "   firefox " << opts.html_output << "\n";
        } else {
            std::cerr << "❌ Failed to write HTML file.\n";
        }
    }

    // ---- Layers ----
    if (opts.layers_only || opts.verbose) {
        print_layers(drawing.layers);
    }

    // ---- Blocks ----
    if (opts.show_blocks || opts.verbose) {
        print_blocks(drawing.blocks);
    }

    // ---- Entity details ----
    if (opts.verbose && !opts.layers_only) {
        std::cout << "\nEntity Details:\n";
        std::cout << "===============\n";

        EntityPrinter printer;
        int shown = 0;
        int limit = opts.max_preview;

        for (auto& rec : drawing.entities) {
            if (shown >= limit) {
                std::cout << "\n... showing " << limit << " of "
                          << drawing.entities.size() << " entities (use --max N for more)\n";
                break;
            }

            std::cout << "\n[" << (shown + 1) << "] "
                      << cad::entity_type_name(rec.entity)
                      << "  Layer: \"" << rec.layer_name << "\""
                      << "  Handle: " << rec.handle << "\n";
            std::visit(printer, rec.entity);
            ++shown;
        }
    } else if (!opts.layers_only && !opts.count_only) {
        // Brief preview: just list entity types
        std::cout << "\nEntity List (first " << std::min(opts.max_preview, static_cast<int>(drawing.entities.size()))
                  << "):\n";
        std::cout << "------------------------------------------\n";

        int shown = 0;
        for (auto& rec : drawing.entities) {
            if (shown >= opts.max_preview) {
                std::cout << "... +" << (drawing.entities.size() - shown) << " more (use --verbose for details)\n";
                break;
            }
            std::cout << "  [" << (shown + 1) << "] "
                      << std::left << std::setw(14) << cad::entity_type_name(rec.entity)
                      << "  Layer: \"" << rec.layer_name << "\"\n";
            ++shown;
        }
    }

    return 0;
}
