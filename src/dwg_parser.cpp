/**
 * @file dwg_parser.cpp
 * @brief DWG file parser using libredwg as a backend.
 *
 * DWG is a proprietary binary format. This implementation uses GNU LibreDWG's
 * `dwgread` command-line tool to convert DWG → JSON, then parses the JSON
 * output into our unified Drawing structure.
 *
 * Without libredwg installed, the parser gracefully reports an error.
 *
 * Reference: https://www.gnu.org/software/libredwg/
 */

#include "cad_parser/dwg_parser.h"
#include "cad_parser/dxf_parser.h"   // for DXF fallback
#include "cad_parser/common_types.h"

#ifdef CAD_HAS_LIBREDWG_API
#include "dwg_api_internal.h"
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace cad {

// =========================================================================
//  Internal helpers
// =========================================================================

namespace {

#ifndef CAD_HAS_LIBREDWG_API

std::string trim_dwg(std::string s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

using PipeCloser = int (*)(FILE*);

FILE* open_process_pipe(const char* command) {
#ifdef _WIN32
    return _popen(command, "r");
#else
    return popen(command, "r");
#endif
}

int close_process_pipe(FILE* pipe) {
#ifdef _WIN32
    return _pclose(pipe);
#else
    return pclose(pipe);
#endif
}

std::string shell_quote(const std::string& value) {
#ifdef _WIN32
    // _popen invokes cmd.exe. Quoting is sufficient for normal executable and
    // drawing paths, including paths containing spaces.
    std::string quoted = "\"";
    for (char c : value) {
        if (c == '\"') quoted += "\\\"";
        else quoted += c;
    }
    return quoted + "\"";
#else
    std::string quoted = "'";
    for (char c : value) {
        if (c == '\'') quoted += "'\\''";
        else quoted += c;
    }
    return quoted + "'";
#endif
}

// Run a command and capture stdout. Both POSIX popen/pclose and the MSVC
// _popen/_pclose equivalents use a shell, but report process status differently.
int run_command(const std::string& cmd, std::string& output) {
    std::array<char, 4096> buffer{};
    std::unique_ptr<FILE, PipeCloser> pipe(open_process_pipe(cmd.c_str()),
                                            close_process_pipe);

    if (!pipe) return -1;

    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe.get()) != nullptr) {
        output += buffer.data();
    }

    int status = close_process_pipe(pipe.release());
    if (status == -1) return -1;
#ifdef _WIN32
    // _pclose returns the child process exit code directly.
    return status;
#else
    // POSIX pclose returns a wait status.
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
#endif
}

bool is_regular_file_path(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error;
}

std::string remove_surrounding_quotes(std::string value) {
    value = trim_dwg(std::move(value));
    if (value.size() >= 2 && value.front() == '\"' && value.back() == '\"') {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

std::string find_dwgread_on_path(const std::vector<std::string>& filenames) {
    const char* raw_path = std::getenv("PATH");
    if (!raw_path) return "";

#ifdef _WIN32
    constexpr char separator = ';';
#else
    constexpr char separator = ':';
#endif

    std::string path_list(raw_path);
    size_t begin = 0;
    while (begin <= path_list.size()) {
        const size_t end = path_list.find(separator, begin);
        std::string entry = remove_surrounding_quotes(
            path_list.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
        const std::filesystem::path directory = entry.empty() ? "." : entry;
        for (const auto& filename : filenames) {
            const std::filesystem::path candidate = directory / filename;
            if (is_regular_file_path(candidate)) return candidate.string();
        }
        if (end == std::string::npos) break;
        begin = end + 1;
    }

    return "";
}

// Try to find dwgread in PATH, then in Unix package locations. Windows uses
// dwgread.exe from PATH or an explicit DwgParserOptions::dwgread_path.
std::string find_dwgread(const std::string& explicit_path) {
    if (!explicit_path.empty()) return explicit_path;

    std::vector<std::string> names;
#ifdef _WIN32
    names = {"dwgread.exe", "dwgread"};
#else
    names = {"dwgread"};
#endif

    if (const std::string found = find_dwgread_on_path(names); !found.empty()) {
        return found;
    }

#ifndef _WIN32
    static const char* candidates[] = {
        "/usr/bin/dwgread",
        "/usr/local/bin/dwgread",
        "/opt/libredwg/bin/dwgread",
        "/usr/lib/x86_64-linux-gnu/libredwg/bin/dwgread",
    };

    for (const auto* path : candidates) {
        if (is_regular_file_path(path)) return path;
    }
#endif

    return "";
}

// =========================================================================
//  mini JSON parser — just enough to extract DWG data from dwgread output
// =========================================================================

// A very lightweight JSON value for parsing dwgread's JSON output.
// We only need to handle objects, arrays, strings, numbers, and null.
struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    std::string string_value;
    double number_value = 0.0;
    bool bool_value = false;
    std::vector<JsonValue> array_items;
    std::map<std::string, JsonValue> object_items;
};

// Forward declarations for recursive descent.
JsonValue parse_json_value(const std::string& json, size_t& pos);

void skip_whitespace(const std::string& json, size_t& pos) {
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
           json[pos] == '\n' || json[pos] == '\r')) {
        ++pos;
    }
}

std::string parse_json_string(const std::string& json, size_t& pos) {
    std::string result;
    if (pos >= json.size() || json[pos] != '"') return result;
    ++pos; // skip opening quote

    while (pos < json.size()) {
        char c = json[pos++];
        if (c == '"') return result;
        if (c == '\\' && pos < json.size()) {
            char escaped = json[pos++];
            switch (escaped) {
                case '"':  result += '"'; break;
                case '\\': result += '\\'; break;
                case '/':  result += '/'; break;
                case 'n':  result += '\n'; break;
                case 't':  result += '\t'; break;
                case 'r':  result += '\r'; break;
                default:   result += escaped; break;
            }
        } else {
            result += c;
        }
    }
    return result;
}

JsonValue parse_json_object(const std::string& json, size_t& pos) {
    JsonValue obj;
    obj.type = JsonValue::Type::Object;
    ++pos; // skip '{'

    skip_whitespace(json, pos);
    if (pos < json.size() && json[pos] == '}') {
        ++pos;
        return obj;
    }

    while (pos < json.size()) {
        skip_whitespace(json, pos);
        std::string key = parse_json_string(json, pos);
        skip_whitespace(json, pos);

        if (pos < json.size() && json[pos] == ':') {
            ++pos;
            skip_whitespace(json, pos);
            obj.object_items[key] = parse_json_value(json, pos);
        }

        skip_whitespace(json, pos);
        if (pos < json.size() && json[pos] == ',') {
            ++pos;
            continue;
        }
        if (pos < json.size() && json[pos] == '}') {
            ++pos;
            break;
        }
    }
    return obj;
}

JsonValue parse_json_array(const std::string& json, size_t& pos) {
    JsonValue arr;
    arr.type = JsonValue::Type::Array;
    ++pos; // skip '['

    skip_whitespace(json, pos);
    if (pos < json.size() && json[pos] == ']') {
        ++pos;
        return arr;
    }

    while (pos < json.size()) {
        skip_whitespace(json, pos);
        arr.array_items.push_back(parse_json_value(json, pos));
        skip_whitespace(json, pos);
        if (pos < json.size() && json[pos] == ',') {
            ++pos;
            continue;
        }
        if (pos < json.size() && json[pos] == ']') {
            ++pos;
            break;
        }
    }
    return arr;
}

JsonValue parse_json_value(const std::string& json, size_t& pos) {
    skip_whitespace(json, pos);
    if (pos >= json.size()) return {};

    char c = json[pos];

    if (c == '"') {
        JsonValue v;
        v.type = JsonValue::Type::String;
        v.string_value = parse_json_string(json, pos);
        return v;
    }
    if (c == '{') {
        return parse_json_object(json, pos);
    }
    if (c == '[') {
        return parse_json_array(json, pos);
    }
    if (c == 't' || c == 'f') {
        // true / false
        JsonValue v;
        v.type = JsonValue::Type::Bool;
        if (json.substr(pos, 4) == "true")  { v.bool_value = true;  pos += 4; }
        if (json.substr(pos, 5) == "false") { v.bool_value = false; pos += 5; }
        return v;
    }
    if (c == 'n') {
        // null
        JsonValue v;
        v.type = JsonValue::Type::Null;
        pos += 4; // skip "null"
        return v;
    }
    // Number
    JsonValue v;
    v.type = JsonValue::Type::Number;
    size_t start = pos;
    if (json[pos] == '-') ++pos;
    while (pos < json.size() && std::isdigit(static_cast<unsigned char>(json[pos]))) ++pos;
    if (pos < json.size() && json[pos] == '.') {
        ++pos;
        while (pos < json.size() && std::isdigit(static_cast<unsigned char>(json[pos]))) ++pos;
    }
    if (pos < json.size() && (json[pos] == 'e' || json[pos] == 'E')) {
        ++pos;
        if (json[pos] == '+' || json[pos] == '-') ++pos;
        while (pos < json.size() && std::isdigit(static_cast<unsigned char>(json[pos]))) ++pos;
    }
    try {
        v.number_value = std::stod(json.substr(start, pos - start));
    } catch (...) {
        v.number_value = 0.0;
    }
    return v;
}

// Convenience: get a string from a JSON object, with default.
std::string jstr(const JsonValue& obj, const std::string& key, const std::string& def = "") {
    auto it = obj.object_items.find(key);
    if (it != obj.object_items.end() && it->second.type == JsonValue::Type::String) {
        return it->second.string_value;
    }
    return def;
}

double jnum(const JsonValue& obj, const std::string& key, double def = 0.0) {
    auto it = obj.object_items.find(key);
    if (it != obj.object_items.end() && it->second.type == JsonValue::Type::Number) {
        return it->second.number_value;
    }
    return def;
}

int jint(const JsonValue& obj, const std::string& key, int def = 0) {
    auto it = obj.object_items.find(key);
    if (it != obj.object_items.end() && it->second.type == JsonValue::Type::Number) {
        return static_cast<int>(it->second.number_value);
    }
    return def;
}

// =========================================================================
//  Parse dwgread's JSON output into our Drawing structure
// =========================================================================

/**
 * dwgread -O json produces JSON with this rough structure:
 * {
 *   "header": { ... },
 *   "objects": { "layers": [...], "blocks": [...], "entities": [...] }
 * }
 *
 * This function walks that JSON and populates a Drawing.
 */
Drawing parse_dwg_json(const std::string& json_str) {
    Drawing drawing;
    drawing.info.format = "DWG";

    size_t pos = 0;
    JsonValue root = parse_json_value(json_str, pos);
    if (root.type != JsonValue::Type::Object) return drawing;

    // --- Parse header ---
    auto header_it = root.object_items.find("header");
    if (header_it != root.object_items.end()) {
        auto& hdr = header_it->second;
        if (hdr.type == JsonValue::Type::Object) {
            drawing.info.version = jstr(hdr, "version", "");

            // Extents
            auto ext_it = hdr.object_items.find("extents");
            if (ext_it != hdr.object_items.end() && ext_it->second.type == JsonValue::Type::Object) {
                auto& ext = ext_it->second;
                drawing.info.min_x = jnum(ext, "min_x");
                drawing.info.min_y = jnum(ext, "min_y");
                drawing.info.min_z = jnum(ext, "min_z");
                drawing.info.max_x = jnum(ext, "max_x");
                drawing.info.max_y = jnum(ext, "max_y");
                drawing.info.max_z = jnum(ext, "max_z");
            }
        }
    }

    // --- Parse objects ---
    auto objects_it = root.object_items.find("objects");
    if (objects_it == root.object_items.end()) return drawing;
    auto& objects = objects_it->second;
    if (objects.type != JsonValue::Type::Object) return drawing;

    // Layers
    auto layers_it = objects.object_items.find("layers");
    if (layers_it != objects.object_items.end() && layers_it->second.type == JsonValue::Type::Array) {
        for (auto& lv : layers_it->second.array_items) {
            if (lv.type == JsonValue::Type::Object) {
                Layer layer;
                layer.name        = jstr(lv, "name");
                layer.color_index = static_cast<int16_t>(jint(lv, "color", 7));
                layer.line_type_name = jstr(lv, "line_type", "Continuous");
                layer.frozen  = jint(lv, "frozen") != 0;
                layer.locked  = jint(lv, "locked") != 0;
                layer.visible = jint(lv, "visible", 1) != 0;
                if (!layer.name.empty()) {
                    drawing.layers[layer.name] = layer;
                }
            }
        }
    }

    // Blocks
    auto blocks_it = objects.object_items.find("blocks");
    if (blocks_it != objects.object_items.end() && blocks_it->second.type == JsonValue::Type::Array) {
        for (auto& bv : blocks_it->second.array_items) {
            if (bv.type == JsonValue::Type::Object) {
                BlockRecord blk;
                blk.name        = jstr(bv, "name");
                blk.description = jstr(bv, "description");
                blk.base_point  = { jnum(bv, "base_x"), jnum(bv, "base_y"), jnum(bv, "base_z") };
                if (!blk.name.empty()) {
                    drawing.blocks[blk.name] = blk;
                }
            }
        }
    }

    // Entities
    auto entities_it = objects.object_items.find("entities");
    if (entities_it != objects.object_items.end() && entities_it->second.type == JsonValue::Type::Array) {
        for (auto& ev : entities_it->second.array_items) {
            if (ev.type != JsonValue::Type::Object) continue;

            std::string etype = jstr(ev, "type");
            EntityRecord rec;
            rec.layer_name = jstr(ev, "layer", "0");
            rec.handle     = jstr(ev, "handle", "");

            if (etype == "LINE") {
                LineEntity e;
                e.start = { jnum(ev, "start_x"), jnum(ev, "start_y"), jnum(ev, "start_z") };
                e.end   = { jnum(ev, "end_x"),   jnum(ev, "end_y"),   jnum(ev, "end_z") };
                rec.entity = e;
            } else if (etype == "CIRCLE") {
                CircleEntity e;
                e.center = { jnum(ev, "center_x"), jnum(ev, "center_y"), jnum(ev, "center_z") };
                e.radius = jnum(ev, "radius");
                rec.entity = e;
            } else if (etype == "ARC") {
                ArcEntity e;
                e.center      = { jnum(ev, "center_x"), jnum(ev, "center_y"), jnum(ev, "center_z") };
                e.radius      = jnum(ev, "radius");
                e.start_angle = jnum(ev, "start_angle");
                e.end_angle   = jnum(ev, "end_angle");
                rec.entity = e;
            } else if (etype == "ELLIPSE") {
                EllipseEntity e;
                e.center = { jnum(ev, "center_x"), jnum(ev, "center_y"), jnum(ev, "center_z") };
                e.major_axis_endpoint = { jnum(ev, "major_x"), jnum(ev, "major_y"), jnum(ev, "major_z") };
                e.axis_ratio = jnum(ev, "axis_ratio", 1.0);
                rec.entity = e;
            } else if (etype == "LWPOLYLINE" || etype == "POLYLINE") {
                LwPolylineEntity e;
                e.closed = jint(ev, "closed") != 0;
                // Parse vertices array
                auto verts_it = ev.object_items.find("vertices");
                if (verts_it != ev.object_items.end() && verts_it->second.type == JsonValue::Type::Array) {
                    for (auto& vv : verts_it->second.array_items) {
                        if (vv.type == JsonValue::Type::Object) {
                            LwPolylineVertex v;
                            v.position = { jnum(vv, "x"), jnum(vv, "y") };
                            v.bulge = jnum(vv, "bulge");
                            e.vertices.push_back(v);
                        }
                    }
                }
                rec.entity = e;
            } else if (etype == "TEXT") {
                TextEntity e;
                e.insertion_point = { jnum(ev, "x"), jnum(ev, "y"), jnum(ev, "z") };
                e.height = jnum(ev, "height", 2.5);
                e.value  = jstr(ev, "text");
                e.rotation = jnum(ev, "rotation");
                rec.entity = e;
            } else if (etype == "MTEXT") {
                MTextEntity e;
                e.insertion_point = { jnum(ev, "x"), jnum(ev, "y"), jnum(ev, "z") };
                e.height  = jnum(ev, "height", 2.5);
                e.value   = jstr(ev, "text");
                rec.entity = e;
            } else if (etype == "INSERT") {
                InsertEntity e;
                e.block_name      = jstr(ev, "block_name");
                e.insertion_point = { jnum(ev, "x"), jnum(ev, "y"), jnum(ev, "z") };
                e.scale_x = jnum(ev, "scale_x", 1.0);
                e.scale_y = jnum(ev, "scale_y", 1.0);
                e.scale_z = jnum(ev, "scale_z", 1.0);
                e.rotation = jnum(ev, "rotation");
                rec.entity = e;
            } else if (etype == "POINT") {
                PointEntity e;
                e.position = { jnum(ev, "x"), jnum(ev, "y"), jnum(ev, "z") };
                rec.entity = e;
            } else if (etype == "SPLINE") {
                SplineEntity e;
                e.degree = jint(ev, "degree", 3);
                e.closed = jint(ev, "closed") != 0;
                rec.entity = e;
            } else {
                // Unknown entity type — store as a point at origin as placeholder
                continue;
            }

            drawing.entities.push_back(rec);
            drawing.info.entity_count++;
        }
    }

    drawing.info.layer_count = drawing.layers.size();
    drawing.info.block_count = drawing.blocks.size();

    return drawing;
}

#endif

} // anonymous namespace

// =========================================================================
//  Public API
// =========================================================================

const char* to_string(DwgParseResult result) {
    switch (result) {
        case DwgParseResult::Success:          return "Success";
        case DwgParseResult::FileNotFound:     return "File not found";
        case DwgParseResult::FileReadError:    return "File read error";
        case DwgParseResult::InvalidFormat:    return "Invalid DWG format";
        case DwgParseResult::UnsupportedVersion: return "Unsupported DWG version";
        case DwgParseResult::LibreDwgNotFound: return "libredwg (dwgread) not found — install GNU LibreDWG";
        case DwgParseResult::LibreDwgError:    return "libredwg returned an error";
        case DwgParseResult::InternalError:    return "Internal parser error";
    }
    return "Unknown";
}

bool is_libredwg_available(std::string* out_version) {
#ifdef CAD_HAS_LIBREDWG_API
    return dwg_api::is_available(out_version);
#else
    std::string dwgread = find_dwgread("");
    if (dwgread.empty()) return false;

    if (out_version) {
        std::string output;
        int rc = run_command(shell_quote(dwgread) + " --version 2>&1", output);
        if (rc == 0) {
            *out_version = trim_dwg(output);
        } else {
            *out_version = "(unknown)";
        }
    }
    return !dwgread.empty();
#endif
}

Drawing parse_dwg_file(const std::string& filepath,
                       const DwgParserOptions& options,
                       DwgParseResult* out_result) {
#ifdef CAD_HAS_LIBREDWG_API
    // Primary path: use libredwg C API (direct linkage, no subprocess)
    return dwg_api::parse_file(filepath, options, out_result);
#else
    // Fallback path: use dwgread CLI subprocess
    Drawing drawing;
    drawing.info.filename = filepath;
    drawing.info.format   = "DWG";

    // Check file exists
    {
        std::ifstream test(filepath, std::ios::binary);
        if (!test.is_open()) {
            if (out_result) *out_result = DwgParseResult::FileNotFound;
            return drawing;
        }
    }

    // Try DXF output format from dwgread first, then JSON
    std::string dwgread_path = find_dwgread(options.dwgread_path);

    if (dwgread_path.empty()) {
        if (out_result) *out_result = DwgParseResult::LibreDwgNotFound;
        return drawing;
    }

    std::string output_format = options.output_format;
    std::string json_output;

    if (output_format == "json") {
        // Use dwgread -O json
        std::string cmd = shell_quote(dwgread_path) + " -O json " +
                          shell_quote(filepath) + " 2>&1";
        int rc = run_command(cmd, json_output);
        if (rc != 0) {
            if (out_result) *out_result = DwgParseResult::LibreDwgError;
            return drawing;
        }

        drawing = parse_dwg_json(json_output);
        drawing.info.filename = filepath;
        drawing.info.format   = "DWG";

    } else if (output_format == "dxf") {
        // Use dwgread -O dxf to convert DWG → DXF, then parse with our DXF parser
        std::string cmd = shell_quote(dwgread_path) + " -O dxf " +
                          shell_quote(filepath) + " 2>&1";
        int rc = run_command(cmd, json_output); // reuse json_output as dxf string
        if (rc != 0) {
            if (out_result) *out_result = DwgParseResult::LibreDwgError;
            return drawing;
        }

        ParseResult dxf_result;
        drawing = parse_dxf_string(json_output, {}, &dxf_result);
        drawing.info.filename = filepath;
        drawing.info.format   = "DWG";

        if (dxf_result != ParseResult::Success) {
            if (out_result) *out_result = DwgParseResult::LibreDwgError;
            return drawing;
        }
    }

    if (out_result) *out_result = DwgParseResult::Success;
    return drawing;
#endif
}

DrawingInfo peek_dwg_header(const std::string& filepath,
                            const DwgParserOptions& options,
                            DwgParseResult* out_result) {
#ifdef CAD_HAS_LIBREDWG_API
    return dwg_api::peek_header(filepath, options, out_result);
#else
    DrawingInfo info;
    info.filename = filepath;
    info.format   = "DWG";

    std::string dwgread_path = find_dwgread(options.dwgread_path);
    if (dwgread_path.empty()) {
        if (out_result) *out_result = DwgParseResult::LibreDwgNotFound;
        return info;
    }

    std::string json_output;
    std::string cmd = shell_quote(dwgread_path) + " -O json " +
                      shell_quote(filepath) + " 2>&1";
    int rc = run_command(cmd, json_output);
    if (rc != 0) {
        if (out_result) *out_result = DwgParseResult::LibreDwgError;
        return info;
    }

    // Parse just the header
    Drawing drawing = parse_dwg_json(json_output);
    info = drawing.info;
    info.filename = filepath;
    info.format   = "DWG";

    if (out_result) *out_result = DwgParseResult::Success;
    return info;
#endif
}

} // namespace cad
