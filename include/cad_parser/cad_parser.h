#pragma once
/**
 * @file cad_parser.h
 * @brief Unified convenience header — include this to get everything.
 *
 * Usage:
 * @code
 *   #include <cad_parser/cad_parser.h>
 *
 *   cad::Drawing drawing;
 *   cad::ParseResult result;
 *   drawing = cad::parse_dxf_file("floorplan.dxf", {}, &result);
 *   if (result != cad::ParseResult::Success) {
 *       std::cerr << "Parse error: " << cad::to_string(result) << "\n";
 *   }
 *   for (auto& rec : drawing.entities) {
 *       std::visit([](auto& e) { ... }, rec.entity);
 *   }
 * @endcode
 */

#include "common_types.h"
#include "dxf_parser.h"
#include "dwg_parser.h"

namespace cad {

/// Auto-detect format from file extension and parse.
///
/// @param filepath  Path to .dxf or .dwg file.
/// @return A populated Drawing. Check the returned Drawing::info::format
///         and entity count to confirm success.
inline Drawing parse_file(const std::string& filepath) {
    // Determine format by extension
    auto dot = filepath.rfind('.');
    if (dot != std::string::npos) {
        std::string ext = filepath.substr(dot);
        for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        if (ext == ".dwg") {
            DwgParseResult r;
            auto d = parse_dwg_file(filepath, {}, &r);
            d.info.format = "DWG";
            return d;
        }
    }

    // Default: try DXF
    ParseResult r;
    auto d = parse_dxf_file(filepath, {}, &r);
    d.info.format = "DXF";
    return d;
}

} // namespace cad
