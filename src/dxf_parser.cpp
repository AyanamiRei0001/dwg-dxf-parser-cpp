/**
 * @file dxf_parser.cpp
 * @brief Pure C++ DXF parser implementation.
 *
 * DXF file structure:
 *   Each line is either a group code (integer) or a value.
 *   Pairs of (group_code, value) form data records.
 *
 *   Sections are delimited by group code 0 + "SECTION" / "ENDSEC".
 *   The ENTITIES section (code 0 + "ENTITIES") holds drawing entities.
 *   Each entity starts with code 0 + entity type, followed by codes for
 *   its properties (layer, color, geometry, etc.).
 *
 * Reference: AutoCAD DXF Reference (Autodesk)
 */

#include "cad_parser/dxf_parser.h"
#include "cad_parser/common_types.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace cad {

// =========================================================================
//  Internal helpers
// =========================================================================

namespace {

// Trim leading/trailing whitespace.
std::string trim(std::string s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// Case-insensitive string compare.
bool iequals(const std::string& a, const std::string& b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(),
                      [](char ca, char cb) {
                          return std::tolower(static_cast<unsigned char>(ca)) ==
                                 std::tolower(static_cast<unsigned char>(cb));
                      });
}

// ---- DXF group-code pair ----
// A single (code, value) record read from the file.
struct DxfPair {
    int code = -1;
    std::string value;
};

// ---- In-memory representation of the DXF file as a list of pairs ----
using DxfPairs = std::vector<DxfPair>;

// ---- Parser state ----
// Positions and metadata tracked during parsing.
struct ParserState {
    size_t pos = 0;                    // current pair index
    size_t line_number = 0;           // approximate line in file (×2 for code+value)
    std::string current_section;      // current SECTION name
    std::string acad_version;         // $ACADVER
    Drawing drawing;                  // output
    std::unordered_map<std::string, std::string> header_vars; // $VAR -> value
    bool saw_section = false;
    bool saw_eof = false;
    bool truncated = false;
};

// =========================================================================
//  File I/O — read all DXF pairs from a file or string
// =========================================================================

ParseResult read_pairs_from_file(const std::string& filepath, DxfPairs& pairs) {
    std::ifstream file(filepath);
    if (!file.is_open()) return ParseResult::FileNotFound;

    std::string line;
    int expected_code = -1;
    bool expecting_code = true;

    while (std::getline(file, line)) {
        if (expecting_code) {
            const std::string code_line = trim(line);
            if (code_line.empty()) continue; // skip blank lines between pairs
            try {
                expected_code = std::stoi(code_line);
            } catch (...) {
                std::cerr << "[DXF] Failed to parse group code at line with content: \""
                          << code_line << "\"" << std::endl;
                return ParseResult::InvalidFormat;
            }
            expecting_code = false;
        } else {
            // Empty line = empty value (e.g., $DIMBLK with no value)
            if (!line.empty() && line.back() == '\r') line.pop_back();
            pairs.push_back({expected_code, line});
            expecting_code = true;
        }
    }

    if (!expecting_code) return ParseResult::TruncatedFile;
    if (pairs.empty()) return ParseResult::FileReadError;
    return ParseResult::Success;
}

ParseResult read_pairs_from_string(const std::string& content, DxfPairs& pairs) {
    std::istringstream stream(content);
    std::string line;
    int expected_code = -1;
    bool expecting_code = true;

    while (std::getline(stream, line)) {
        if (expecting_code) {
            const std::string code_line = trim(line);
            if (code_line.empty()) continue; // skip blank lines between pairs
            try {
                expected_code = std::stoi(code_line);
            } catch (...) {
                std::cerr << "[DXF] Failed to parse group code at line with content: \""
                          << code_line << "\"" << std::endl;
                return ParseResult::InvalidFormat;
            }
            expecting_code = false;
        } else {
            // Empty line = empty value
            if (!line.empty() && line.back() == '\r') line.pop_back();
            pairs.push_back({expected_code, line});
            expecting_code = true;
        }
    }

    if (!expecting_code) return ParseResult::TruncatedFile;
    if (pairs.empty()) return ParseResult::FileReadError;
    return ParseResult::Success;
}

// =========================================================================
//  Pair access helpers — navigate the flat pair list
// =========================================================================

// Look ahead at a future pair without advancing.
const DxfPair* peek(const DxfPairs& pairs, const ParserState& state, int offset = 0) {
    size_t idx = state.pos + offset;
    if (idx < pairs.size()) return &pairs[idx];
    return nullptr;
}

// Check if we're at a specific code+value marker (section/entity boundary).
bool at(const DxfPairs& pairs, const ParserState& state, int code, const std::string& value) {
    auto* p = peek(pairs, state);
    return p && p->code == code && iequals(p->value, value);
}

// =========================================================================
//  Value conversion helpers
// =========================================================================

double get_double(const std::map<int, std::string>& props, int code, double default_val = 0.0) {
    auto it = props.find(code);
    if (it == props.end()) return default_val;
    try { return std::stod(it->second); } catch (...) { return default_val; }
}

int get_int(const std::map<int, std::string>& props, int code, int default_val = 0) {
    auto it = props.find(code);
    if (it == props.end()) return default_val;
    try { return std::stoi(it->second); } catch (...) { return default_val; }
}

std::string get_str(const std::map<int, std::string>& props, int code, const std::string& default_val = "") {
    auto it = props.find(code);
    if (it == props.end()) return default_val;
    return it->second;
}

Point3D get_point3d(const std::map<int, std::string>& props,
                     int cx, int cy, int cz) {
    return { get_double(props, cx), get_double(props, cy), get_double(props, cz) };
}

// =========================================================================
//  Build entity record from common DXF group codes
// =========================================================================

void fill_common_props(EntityRecord& rec, const std::map<int, std::string>& props) {
    rec.layer_name = get_str(props, 8, "0");
    rec.handle     = get_str(props, 5, "");

    // Color: code 62, negative = off, 0 = BYBLOCK, 256 = BYLAYER
    int ci = get_int(props, 62, 256);
    if (ci == 0)       { rec.color.index = 0; }        // BYBLOCK
    else if (ci == 256) { rec.color.index = 256; }      // BYLAYER → keep 256
    else if (ci < 0)    { rec.color.index = static_cast<int16_t>(ci); } // off
    else                { rec.color.index = static_cast<int16_t>(ci); }

    // Line type
    rec.line_type_name = get_str(props, 6, "ByLayer");

    // Line weight (code 370)
    int lw = get_int(props, 370, -1);
    if (lw >= 0) rec.line_weight = static_cast<LineWeight>(lw);
}

// =========================================================================
//  Entity parsers — each reads props and returns an Entity variant
// =========================================================================

Entity parse_line(const std::map<int, std::string>& props) {
    LineEntity e;
    e.start     = get_point3d(props, 10, 20, 30);
    e.end       = get_point3d(props, 11, 21, 31);
    e.thickness = get_double(props, 39);
    return e;
}

Entity parse_circle(const std::map<int, std::string>& props) {
    CircleEntity e;
    e.center    = get_point3d(props, 10, 20, 30);
    e.radius    = get_double(props, 40);
    e.thickness = get_double(props, 39);
    return e;
}

Entity parse_arc(const std::map<int, std::string>& props) {
    ArcEntity e;
    e.center      = get_point3d(props, 10, 20, 30);
    e.radius      = get_double(props, 40);
    e.start_angle = get_double(props, 50);
    e.end_angle   = get_double(props, 51);
    e.thickness   = get_double(props, 39);
    return e;
}

Entity parse_ellipse(const std::map<int, std::string>& props) {
    EllipseEntity e;
    e.center             = get_point3d(props, 10, 20, 30);
    e.major_axis_endpoint = get_point3d(props, 11, 21, 31);
    e.axis_ratio         = get_double(props, 40, 1.0);

    // Parametric angles (code 41, 42) — only for partial ellipses
    if (props.count(41)) e.start_angle = get_double(props, 41) * 180.0 / M_PI;
    if (props.count(42)) e.end_angle   = get_double(props, 42) * 180.0 / M_PI;
    return e;
}

Entity parse_text(const std::map<int, std::string>& props) {
    TextEntity e;
    e.insertion_point      = get_point3d(props, 10, 20, 30);
    e.height               = get_double(props, 40, 2.5);
    e.value                = get_str(props, 1);
    e.rotation             = get_double(props, 50);
    e.oblique              = get_double(props, 51);
    e.style                = get_str(props, 7, "Standard");
    e.horizontal_alignment = get_int(props, 72);
    e.vertical_alignment   = get_int(props, 73);
    e.has_alignment_point  = props.count(11) != 0;
    if (e.has_alignment_point)
        e.alignment_point = get_point3d(props, 11, 21, 31);
    return e;
}

Entity parse_mtext(const std::map<int, std::string>& props) {
    MTextEntity e;
    e.insertion_point  = get_point3d(props, 10, 20, 30);
    e.height           = get_double(props, 40, 2.5);
    e.value            = get_str(props, 1);
    e.rotation         = get_double(props, 50);
    e.rect_width       = get_double(props, 41);
    e.attachment_point = get_int(props, 71, 1);
    e.style            = get_str(props, 7, "Standard");
    return e;
}

Entity parse_insert(const std::map<int, std::string>& props) {
    InsertEntity e;
    e.block_name      = get_str(props, 2);
    e.insertion_point = get_point3d(props, 10, 20, 30);
    e.scale_x         = get_double(props, 41, 1.0);
    e.scale_y         = get_double(props, 42, 1.0);
    e.scale_z         = get_double(props, 43, 1.0);
    e.rotation        = get_double(props, 50);
    e.column_count    = static_cast<uint16_t>(get_int(props, 70, 1));
    e.row_count       = static_cast<uint16_t>(get_int(props, 71, 1));
    e.column_spacing  = get_double(props, 44);
    e.row_spacing     = get_double(props, 45);
    return e;
}

Entity parse_point(const std::map<int, std::string>& props) {
    PointEntity e;
    e.position = get_point3d(props, 10, 20, 30);
    return e;
}

Entity parse_solid(const std::map<int, std::string>& props) {
    SolidEntity e;
    e.p1 = get_point3d(props, 10, 20, 30);
    e.p2 = get_point3d(props, 11, 21, 31);
    e.p3 = get_point3d(props, 12, 22, 32);
    e.p4 = get_point3d(props, 13, 23, 33);
    return e;
}

Entity parse_spline(const std::map<int, std::string>& props) {
    SplineEntity e;
    e.degree   = get_int(props, 71, 3);
    e.closed   = (get_int(props, 70) & 1) != 0;
    e.periodic = (get_int(props, 70) & 2) != 0;

    int num_ctrl  = get_int(props, 73);
    int num_fit   = get_int(props, 74);
    int num_knots = get_int(props, 72);

    // Control points: codes 10,20,30 repeated num_ctrl times
    // Fit points: codes 11,21,31 repeated num_fit times
    // Knots: code 40 repeated num_knots times
    // Like LWPOLYLINE, these duplicate codes can't be extracted from a map.
    // The section parser handles them.
    (void)num_ctrl; (void)num_fit; (void)num_knots;
    return e;
}

Entity parse_dimension(const std::map<int, std::string>& props) {
    DimensionEntity e;
    int dim_type = get_int(props, 70);
    switch (dim_type & 0x7F) {
        case 0: e.type = "Rotated"; break;
        case 1: e.type = "Aligned"; break;
        case 2: e.type = "Angular"; break;
        case 3: e.type = "Diameter"; break;
        case 4: e.type = "Radius"; break;
        case 5: e.type = "Angular3Point"; break;
        case 6: e.type = "Ordinate"; break;
        default: e.type = "Unknown";
    }
    e.definition_point = get_point3d(props, 10, 20, 30);
    e.text_midpoint    = get_point3d(props, 11, 21, 31);
    e.text_override    = get_str(props, 1);
    e.text_rotation    = get_double(props, 53);
    return e;
}

Entity parse_hatch(const std::map<int, std::string>& props) {
    HatchEntity e;
    e.pattern_name  = get_str(props, 2, "SOLID");
    e.pattern_scale = get_double(props, 41, 1.0);
    e.pattern_angle = get_double(props, 52);
    e.associative   = get_int(props, 71, 1) != 0;
    return e;
}

void extract_hatch_boundaries(const DxfPairs& pairs, size_t start, size_t end,
                              HatchEntity& hatch) {
    std::vector<Point2D> loop;
    bool in_path = false;
    bool polyline_path = false;
    bool has_x10 = false;
    bool has_x11 = false;
    double x10 = 0.0, x11 = 0.0;

    auto finish_path = [&]() {
        if (loop.size() >= 2) hatch.boundary_loops.push_back(loop);
        loop.clear();
        in_path = false;
        has_x10 = has_x11 = false;
    };

    for (size_t i = start; i < end && i < pairs.size(); ++i) {
        const auto& pair = pairs[i];
        if (pair.code == 92) {
            if (in_path) finish_path();
            try {
                polyline_path = (std::stoi(pair.value) & 2) != 0;
            } catch (...) {
                polyline_path = false;
            }
            in_path = true;
            continue;
        }
        if (!in_path) continue;
        if (pair.code == 97) {
            finish_path();
            continue;
        }

        try {
            if (pair.code == 10) {
                x10 = std::stod(pair.value);
                has_x10 = true;
            } else if (pair.code == 20 && has_x10) {
                loop.push_back({x10, std::stod(pair.value)});
                has_x10 = false;
            } else if (!polyline_path && pair.code == 11) {
                x11 = std::stod(pair.value);
                has_x11 = true;
            } else if (!polyline_path && pair.code == 21 && has_x11) {
                Point2D endpoint{x11, std::stod(pair.value)};
                if (loop.empty() || loop.back().x != endpoint.x || loop.back().y != endpoint.y) {
                    loop.push_back(endpoint);
                }
                has_x11 = false;
            }
        } catch (...) {
            has_x10 = has_x11 = false;
        }
    }
    if (in_path) finish_path();
}

std::string extract_mtext_value(const DxfPairs& pairs, size_t start, size_t end) {
    std::string value;
    for (size_t i = start; i < end && i < pairs.size(); ++i) {
        if (pairs[i].code == 1 || pairs[i].code == 3) value += pairs[i].value;
    }
    return value;
}

void extract_spline_data(const DxfPairs& pairs, size_t start, size_t end,
                         SplineEntity& spline) {
    Point3D control;
    Point3D fit;
    bool has_control_x = false;
    bool has_fit_x = false;

    auto push_control = [&]() {
        if (has_control_x) spline.control_points.push_back(control);
        control = {};
        has_control_x = false;
    };
    auto push_fit = [&]() {
        if (has_fit_x) spline.fit_points.push_back(fit);
        fit = {};
        has_fit_x = false;
    };

    for (size_t i = start; i < end && i < pairs.size(); ++i) {
        const auto& pair = pairs[i];
        try {
            switch (pair.code) {
                case 10: push_control(); control.x = std::stod(pair.value); has_control_x = true; break;
                case 20: control.y = std::stod(pair.value); break;
                case 30: control.z = std::stod(pair.value); break;
                case 11: push_fit(); fit.x = std::stod(pair.value); has_fit_x = true; break;
                case 21: fit.y = std::stod(pair.value); break;
                case 31: fit.z = std::stod(pair.value); break;
                case 40: spline.knots.push_back(std::stod(pair.value)); break;
                default: break;
            }
        } catch (...) {
            // Keep parsing the remaining points; malformed scalar values retain defaults.
        }
    }
    push_control();
    push_fit();
}

// ---- Vertex extraction from sequential pairs (LWPOLYLINE) ----

/**
 * LWPOLYLINE stores vertices as repeated code sequences:
 *   10 x1, 20 y1, 42 bulge1
 *   10 x2, 20 y2, 42 bulge2
 *   ...
 * This function walks the pairs from the current position and extracts them.
 */
void extract_lwpolyline_vertices(const DxfPairs& pairs, size_t start, size_t end,
                                  std::vector<LwPolylineVertex>& vertices) {
    LwPolylineVertex current;
    bool has_x = false, has_y = false;

    for (size_t i = start; i < end && i < pairs.size(); ++i) {
        const auto& p = pairs[i];
        if (p.code == 0) break; // next entity

        switch (p.code) {
            case 10:
                if (has_x && has_y) {
                    vertices.push_back(current);
                    current = {};
                }
                try { current.position.x = std::stod(p.value); } catch (...) {}
                has_x = true; has_y = false;
                break;
            case 20:
                try { current.position.y = std::stod(p.value); } catch (...) {}
                has_y = true;
                break;
            case 42:
                try { current.bulge = std::stod(p.value); } catch (...) {}
                break;
            case 40:
                try { current.start_width = std::stod(p.value); } catch (...) {}
                break;
            case 41:
                try { current.end_width = std::stod(p.value); } catch (...) {}
                break;
            default: break;
        }
    }
    // Push the last vertex
    if (has_x && has_y) {
        vertices.push_back(current);
    }
}

// =========================================================================
//  Section parsers
// =========================================================================

/**
 * Parse the HEADER section.
 * Extract $ACADVER and other variables of interest.
 */
void parse_header_section(const DxfPairs& pairs, ParserState& state) {
    while (state.pos < pairs.size()) {
        if (at(pairs, state, 0, "ENDSEC")) {
            state.pos++; // skip ENDSEC
            return;
        }

        auto& pair = pairs[state.pos];
        if (pair.code == 9) {
            // Variable name (e.g. "$ACADVER", "$EXTMIN")
            std::string var_name = pair.value;
            state.pos++;

            // Read the variable's group codes until the next variable or ENDSEC
            std::string var_value;
            while (state.pos < pairs.size()) {
                auto& vp = pairs[state.pos];
                if (vp.code == 9 || vp.code == 0) break;

                // Most header vars have their value at code 1, 2, 10, 20, 30, 40, 70, 280, etc.
                if (vp.code == 1 || vp.code == 2 || vp.code == 40 || vp.code == 70 || vp.code == 280) {
                    var_value = vp.value;
                }
                // Extents: $EXTMIN (10,20,30), $EXTMAX (10,20,30)
                if (iequals(var_name, "$EXTMIN")) {
                    if (vp.code == 10) state.drawing.info.min_x = std::stod(vp.value);
                    if (vp.code == 20) state.drawing.info.min_y = std::stod(vp.value);
                    if (vp.code == 30) state.drawing.info.min_z = std::stod(vp.value);
                }
                if (iequals(var_name, "$EXTMAX")) {
                    if (vp.code == 10) state.drawing.info.max_x = std::stod(vp.value);
                    if (vp.code == 20) state.drawing.info.max_y = std::stod(vp.value);
                    if (vp.code == 30) state.drawing.info.max_z = std::stod(vp.value);
                }
                // LTSCALE
                if (iequals(var_name, "$LTSCALE") && vp.code == 40) var_value = vp.value;

                state.pos++;
            }

            if (iequals(var_name, "$ACADVER")) {
                state.acad_version = var_value;
                state.drawing.info.version = var_value;
            }
            state.header_vars[var_name] = var_value;
        } else {
            state.pos++;
        }
    }
}

/**
 * Parse the TABLES section.
 * Extracts layer definitions, line types, text styles, etc.
 */
void parse_tables_section(const DxfPairs& pairs, ParserState& state) {
    while (state.pos < pairs.size()) {
        if (at(pairs, state, 0, "ENDSEC")) {
            state.pos++;
            return;
        }

        // Look for table entries
        if (at(pairs, state, 0, "TABLE")) {
            state.pos++;
            continue;
        }

        // LAYER table
        if (at(pairs, state, 0, "LAYER")) {
            state.pos++;
            // Read until we hit ENDTAB or another TABLE
            std::map<int, std::string> props;
            Layer current_layer;

            while (state.pos < pairs.size()) {
                if (at(pairs, state, 0, "ENDTAB") || at(pairs, state, 0, "TABLE")) break;

                auto& pair = pairs[state.pos];
                if (pair.code == 0) {
                    // End of a layer entry — save current and start new
                    if (!current_layer.name.empty()) {
                        state.drawing.layers[current_layer.name] = current_layer;
                        current_layer = {};
                    }
                    if (iequals(pair.value, "LAYER")) {
                        state.pos++; // skip LAYER marker, process next layer
                        continue;
                    }
                    // Other code-0 marker (ENDTAB, TABLE, etc.) — handled by loop condition
                    continue;
                }

                // Collect layer properties
                switch (pair.code) {
                    case 2:  current_layer.name = pair.value; break;
                    case 62: current_layer.color_index = static_cast<int16_t>(std::stoi(pair.value)); break;
                    case 6:  current_layer.line_type_name = pair.value; break;
                    case 70: {
                        int v = std::stoi(pair.value);
                        if (v & 1) current_layer.frozen = true;
                        if (v & 4) current_layer.locked = true;
                        break;
                    }
                    case 370:
                        current_layer.line_weight = static_cast<LineWeight>(std::stoi(pair.value));
                        break;
                    default: break;
                }
                state.pos++;
            }

            if (!current_layer.name.empty()) {
                state.drawing.layers[current_layer.name] = current_layer;
            }
            continue;
        }

        // Skip other table types for now (STYLE, LTYPE, etc.)
        state.pos++;
    }
}

/**
 * Parse the BLOCKS section.
 */
void parse_blocks_section(const DxfPairs& pairs, ParserState& state) {
    BlockRecord current_block;

    while (state.pos < pairs.size()) {
        if (at(pairs, state, 0, "ENDSEC")) {
            if (!current_block.name.empty()) {
                state.drawing.blocks[current_block.name] = current_block;
            }
            state.pos++;
            return;
        }

        if (at(pairs, state, 0, "BLOCK")) {
            // Save previous block
            if (!current_block.name.empty()) {
                state.drawing.blocks[current_block.name] = current_block;
            }
            current_block = {};
            state.pos++;

            // Read block header (codes 2,10,20,30,4 before first entity)
            while (state.pos < pairs.size() && pairs[state.pos].code != 0) {
                auto& pair = pairs[state.pos];
                if (pair.code == 2)  current_block.name = pair.value;
                if (pair.code == 10) current_block.base_point.x = std::stod(pair.value);
                if (pair.code == 20) current_block.base_point.y = std::stod(pair.value);
                if (pair.code == 30) current_block.base_point.z = std::stod(pair.value);
                if (pair.code == 4)  current_block.description = pair.value;
                state.pos++;
            }

            // Parse contained entities until ENDBLK
            while (state.pos < pairs.size() && !at(pairs, state, 0, "ENDBLK")) {
                if (pairs[state.pos].code != 0) { state.pos++; continue; }

                std::string etype = pairs[state.pos].value;
                state.pos++; // skip entity type marker

                std::map<int, std::string> eprops;
                size_t entity_start = state.pos;
                while (state.pos < pairs.size() && pairs[state.pos].code != 0) {
                    eprops[pairs[state.pos].code] = pairs[state.pos].value;
                    state.pos++;
                }

                EntityRecord rec;
                fill_common_props(rec, eprops);
                if (iequals(etype, "LINE"))      rec.entity = parse_line(eprops);
                else if (iequals(etype, "CIRCLE"))   rec.entity = parse_circle(eprops);
                else if (iequals(etype, "ARC"))      rec.entity = parse_arc(eprops);
                else if (iequals(etype, "LWPOLYLINE")) {
                    LwPolylineEntity lwp;
                    lwp.closed = (get_int(eprops, 70) & 1) != 0;
                    lwp.elevation = get_double(eprops, 38);
                    lwp.thickness = get_double(eprops, 39);
                    extract_lwpolyline_vertices(pairs, entity_start, state.pos, lwp.vertices);
                    rec.entity = lwp;
                } else if (iequals(etype, "TEXT")) rec.entity = parse_text(eprops);
                else if (iequals(etype, "MTEXT")) {
                    auto mtext = std::get<MTextEntity>(parse_mtext(eprops));
                    mtext.value = extract_mtext_value(pairs, entity_start, state.pos);
                    rec.entity = mtext;
                }
                else if (iequals(etype, "INSERT")) rec.entity = parse_insert(eprops);
                else if (iequals(etype, "POINT"))  rec.entity = parse_point(eprops);
                else if (iequals(etype, "ELLIPSE")) rec.entity = parse_ellipse(eprops);
                else if (iequals(etype, "SOLID") || iequals(etype, "3DFACE")) rec.entity = parse_solid(eprops);
                else if (iequals(etype, "SPLINE")) {
                    auto spline = std::get<SplineEntity>(parse_spline(eprops));
                    extract_spline_data(pairs, entity_start, state.pos, spline);
                    rec.entity = spline;
                }
                else { continue; } // skip unsupported types in blocks

                current_block.entities.push_back(rec);
            }

            if (at(pairs, state, 0, "ENDBLK")) state.pos++; // skip ENDBLK
            continue;
        }

        state.pos++;
    }
}

/**
 * Parse the ENTITIES section — this is the core of DXF parsing.
 *
 * The ENTITIES section contains all drawing entities. Each entity starts with
 * a group code 0 line giving the entity type, followed by property and
 * geometry group codes. Some entities (POLYLINE, INSERT) have sub-entities
 * that follow immediately.
 */
void parse_entities_section(const DxfPairs& pairs, ParserState& state,
                             const DxfParserOptions& options) {
    EntityRecord current_polyline_rec; // for continuing POLYLINE/VERTEX sequences
    PolylineEntity current_polyline;
    bool in_polyline = false;

    auto finish_polyline = [&]() {
        if (!in_polyline) return;
        if (options.convert_heavy_polylines) {
            LwPolylineEntity converted;
            converted.closed = current_polyline.closed;
            converted.thickness = current_polyline.thickness;
            if (!current_polyline.vertices.empty()) {
                converted.elevation = current_polyline.vertices.front().position.z;
            }
            converted.vertices.reserve(current_polyline.vertices.size());
            for (const auto& vertex : current_polyline.vertices) {
                converted.vertices.push_back({{vertex.position.x, vertex.position.y},
                                              vertex.bulge, vertex.start_width,
                                              vertex.end_width});
            }
            current_polyline_rec.entity = std::move(converted);
        } else {
            current_polyline_rec.entity = current_polyline;
        }
        state.drawing.entities.push_back(current_polyline_rec);
        state.drawing.info.entity_count++;
        in_polyline = false;
    };

    while (state.pos < pairs.size()) {
        if (at(pairs, state, 0, "ENDSEC")) {
            if (in_polyline) {
                finish_polyline();
                state.truncated = true;
            }
            state.pos++;
            return;
        }

        // Check entity limit
        if (options.max_entities > 0 &&
            state.drawing.entities.size() >= options.max_entities) {
            return;
        }

        // Entity boundary
        if (pairs[state.pos].code != 0) {
            state.pos++;
            continue;
        }

        std::string entity_type = pairs[state.pos].value;

        // Handle POLYLINE sub-entities (VERTEX / SEQEND)
        if (in_polyline) {
            if (iequals(entity_type, "VERTEX")) {
                state.pos++;
                std::map<int, std::string> vprops;
                while (state.pos < pairs.size() && pairs[state.pos].code != 0) {
                    vprops[pairs[state.pos].code] = pairs[state.pos].value;
                    state.pos++;
                }
                PolylineVertex v;
                v.position = get_point3d(vprops, 10, 20, 30);
                v.bulge = get_double(vprops, 42);
                v.start_width = get_double(vprops, 40);
                v.end_width   = get_double(vprops, 41);
                current_polyline.vertices.push_back(v);
                continue;
            }
            if (iequals(entity_type, "SEQEND")) {
                finish_polyline();
                state.pos++; // skip SEQEND
                // skip SEQEND properties
                while (state.pos < pairs.size() && pairs[state.pos].code != 0) state.pos++;
                continue;
            }

            // A new top-level entity before SEQEND means the polyline was truncated.
            finish_polyline();
            state.truncated = true;
        }

        // ---- Single entities ----
        state.pos++; // consume the entity type line

        // Collect all properties for this entity
        std::map<int, std::string> props;
        size_t entity_start = state.pos;

        while (state.pos < pairs.size() && pairs[state.pos].code != 0) {
            props[pairs[state.pos].code] = pairs[state.pos].value;
            state.pos++;
        }

        // Build the entity
        Entity entity;
        bool valid = true;

        if (iequals(entity_type, "LINE")) {
            entity = parse_line(props);
        } else if (iequals(entity_type, "CIRCLE")) {
            entity = parse_circle(props);
        } else if (iequals(entity_type, "ARC")) {
            entity = parse_arc(props);
        } else if (iequals(entity_type, "ELLIPSE")) {
            entity = parse_ellipse(props);
        } else if (iequals(entity_type, "LWPOLYLINE")) {
            LwPolylineEntity lwp;
            lwp.elevation  = get_double(props, 38);
            lwp.thickness  = get_double(props, 39);
            lwp.closed     = (get_int(props, 70) & 1) != 0;
            // Extract vertices from the raw pairs
            extract_lwpolyline_vertices(pairs, entity_start, state.pos,
                                         lwp.vertices);
            entity = lwp;
        } else if (iequals(entity_type, "POLYLINE")) {
            current_polyline_rec = {};
            fill_common_props(current_polyline_rec, props);
            current_polyline = {};
            current_polyline.closed    = (get_int(props, 70) & 1) != 0;
            current_polyline.thickness = get_double(props, 39);
            in_polyline = true;
            continue; // wait for VERTEX/SEQEND
        } else if (iequals(entity_type, "TEXT")) {
            entity = parse_text(props);
        } else if (iequals(entity_type, "MTEXT")) {
            auto mtext = std::get<MTextEntity>(parse_mtext(props));
            mtext.value = extract_mtext_value(pairs, entity_start, state.pos);
            entity = mtext;
        } else if (iequals(entity_type, "INSERT")) {
            entity = parse_insert(props);
        } else if (iequals(entity_type, "POINT")) {
            entity = parse_point(props);
        } else if (iequals(entity_type, "SOLID") || iequals(entity_type, "3DFACE")) {
            entity = parse_solid(props);
        } else if (iequals(entity_type, "SPLINE")) {
            auto spline = std::get<SplineEntity>(parse_spline(props));
            extract_spline_data(pairs, entity_start, state.pos, spline);
            entity = spline;
        } else if (iequals(entity_type, "DIMENSION")) {
            entity = parse_dimension(props);
        } else if (iequals(entity_type, "HATCH")) {
            auto hatch = std::get<HatchEntity>(parse_hatch(props));
            extract_hatch_boundaries(pairs, entity_start, state.pos, hatch);
            entity = hatch;
        } else {
            valid = false; // unknown entity type, skip
        }

        if (valid) {
            EntityRecord rec;
            rec.entity = entity;
            fill_common_props(rec, props);
            state.drawing.entities.push_back(rec);
            state.drawing.info.entity_count++;
        }

        // Progress callback
        if (options.on_progress) {
            options.on_progress(state.pos, state.drawing.entities.size());
        }
    }

    if (in_polyline) {
        finish_polyline();
        state.truncated = true;
    }
}

/**
 * Top-level section dispatcher.
 */
ParseResult parse_all_sections(const DxfPairs& pairs, ParserState& state,
                                const DxfParserOptions& options) {
    while (state.pos < pairs.size()) {
        if (at(pairs, state, 0, "EOF")) {
            state.saw_eof = true;
            state.pos++;
            continue;
        }

        // Look for section start: code 0 + "SECTION"
        if (at(pairs, state, 0, "SECTION")) {
            state.saw_section = true;
            state.pos++; // consume SECTION
            // Next pair should be code 2 + section name
            if (state.pos < pairs.size() && pairs[state.pos].code == 2) {
                state.current_section = pairs[state.pos].value;
                state.pos++;

                // Dispatch to section-specific parser
                if (iequals(state.current_section, "HEADER")) {
                    parse_header_section(pairs, state);
                } else if (iequals(state.current_section, "TABLES")) {
                    parse_tables_section(pairs, state);
                } else if (iequals(state.current_section, "BLOCKS")) {
                    parse_blocks_section(pairs, state);
                } else if (iequals(state.current_section, "ENTITIES")) {
                    parse_entities_section(pairs, state, options);
                } else {
                    // Skip unknown sections — scan for ENDSEC
                    while (state.pos < pairs.size() && !at(pairs, state, 0, "ENDSEC")) {
                        state.pos++;
                    }
                    if (at(pairs, state, 0, "ENDSEC")) state.pos++;
                }
            } else {
                return ParseResult::InvalidFormat;
            }
        } else {
            state.pos++;
        }
    }

    // Update drawing info counts
    state.drawing.info.layer_count  = state.drawing.layers.size();
    state.drawing.info.block_count  = state.drawing.blocks.size();

    if (!state.saw_section) return ParseResult::InvalidFormat;
    if (state.truncated || !state.saw_eof) return ParseResult::TruncatedFile;
    return ParseResult::Success;
}

} // anonymous namespace

// =========================================================================
//  Public API
// =========================================================================

const char* to_string(ParseResult result) {
    switch (result) {
        case ParseResult::Success:            return "Success";
        case ParseResult::FileNotFound:       return "File not found";
        case ParseResult::FileReadError:      return "File read error";
        case ParseResult::InvalidFormat:      return "Invalid DXF format";
        case ParseResult::UnsupportedVersion: return "Unsupported DXF version";
        case ParseResult::TruncatedFile:      return "Truncated DXF file";
        case ParseResult::InternalError:      return "Internal parser error";
    }
    return "Unknown";
}

Drawing parse_dxf_file(const std::string& filepath,
                       const DxfParserOptions& options,
                       ParseResult* out_result) {
    Drawing drawing;
    drawing.info.filename = filepath;
    drawing.info.format = "DXF";

    DxfPairs pairs;
    ParseResult result = read_pairs_from_file(filepath, pairs);

    if (result != ParseResult::Success) {
        if (out_result) *out_result = result;
        return drawing;
    }

    ParserState state;
    state.drawing = drawing;

    try {
        result = parse_all_sections(pairs, state, options);
    } catch (const std::exception&) {
        result = ParseResult::InternalError;
    }

    // Update final counts
    state.drawing.info.layer_count  = state.drawing.layers.size();
    state.drawing.info.block_count  = state.drawing.blocks.size();

    if (out_result) *out_result = result;
    return state.drawing;
}

Drawing parse_dxf_string(const std::string& content,
                         const DxfParserOptions& options,
                         ParseResult* out_result) {
    Drawing drawing;
    drawing.info.format = "DXF";

    DxfPairs pairs;
    ParseResult result = read_pairs_from_string(content, pairs);

    if (result != ParseResult::Success) {
        if (out_result) *out_result = result;
        return drawing;
    }

    ParserState state;
    state.drawing = drawing;

    try {
        result = parse_all_sections(pairs, state, options);
    } catch (const std::exception&) {
        result = ParseResult::InternalError;
    }

    state.drawing.info.layer_count  = state.drawing.layers.size();
    state.drawing.info.block_count  = state.drawing.blocks.size();

    if (out_result) *out_result = result;
    return state.drawing;
}

DrawingInfo peek_dxf_header(const std::string& filepath,
                            ParseResult* out_result) {
    DrawingInfo info;
    info.filename = filepath;
    info.format   = "DXF";

    DxfPairs pairs;
    ParseResult result = read_pairs_from_file(filepath, pairs);

    if (result != ParseResult::Success) {
        if (out_result) *out_result = result;
        return info;
    }

    // Quick scan: only parse the HEADER section
    ParserState state;
    for (size_t i = 0; i < pairs.size(); ++i) {
        if (pairs[i].code == 0 && iequals(pairs[i].value, "SECTION")) {
            if (i + 1 < pairs.size() && pairs[i + 1].code == 2 &&
                iequals(pairs[i + 1].value, "HEADER")) {
                state.pos = i + 2;
                state.drawing.info = info;
                try {
                    parse_header_section(pairs, state);
                    info = state.drawing.info;
                } catch (...) {}
                break;
            }
        }
    }

    if (out_result) *out_result = result;
    return info;
}

} // namespace cad
