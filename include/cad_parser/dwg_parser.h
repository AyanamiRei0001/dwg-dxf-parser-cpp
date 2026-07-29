#pragma once
/**
 * @file dwg_parser.h
 * @brief DWG file parser interface.
 *
 * DWG is Autodesk's proprietary binary format. This parser provides two
 * backends for reading DWG files:
 *
 *  1. libredwg CLI backend (default) — calls the `dwgread` tool from
 *     GNU LibreDWG (https://www.gnu.org/software/libredwg/) through a
 *     platform-native subprocess pipe. Requires dwgread on PATH (or an
 *     explicit path, including dwgread.exe on Windows).
 *
 *  2. libredwg C API backend (optional) — links directly against
 *     libredwg. Enable with -DCAD_USE_LIBREDWG_API=ON in CMake.
 *
 * Without either backend installed, the parser will report an error
 * indicating the missing dependency.
 */

#include "common_types.h"
#include <string>
#include <functional>
#include <iosfwd>

namespace cad {

/// Callback for progress reporting during DWG parsing.
using DwgProgressCallback = std::function<void(size_t bytes_read, size_t entities_found)>;

/// DWG parser configuration.
struct DwgParserOptions {
    /// Path to the `dwgread` binary (CLI backend).
    /// If empty, searches PATH.
    std::string dwgread_path;

    /// Preferred output format from dwgread: "dxf", "json", or "svg".
    /// Default "dxf" reuses the DXF parser for unified output.
    std::string output_format = "json";

    /// Callback for progress.
    DwgProgressCallback on_progress = nullptr;
};

/// Result status for DWG parse operations.
enum class DwgParseResult {
    Success,
    FileNotFound,
    FileReadError,
    InvalidFormat,
    UnsupportedVersion,
    LibreDwgNotFound,     // dwgread not installed
    LibreDwgError,        // dwgread returned non-zero
    InternalError
};

const char* to_string(DwgParseResult result);

/// Parse a DWG file.
///
/// Uses libredwg's dwgread under the hood — either via CLI subprocess
/// or linked library, depending on build configuration.
///
/// @param filepath   Path to the .dwg file.
/// @param options    Parser configuration.
/// @param[out] out_result  Detailed parse status.
/// @return A populated Drawing object.
Drawing parse_dwg_file(const std::string& filepath,
                       const DwgParserOptions& options = {},
                       DwgParseResult* out_result = nullptr);

/// Quick check: read DWG header only.
///
/// @param filepath   Path to the .dwg file.
/// @param[out] out_result  Detailed parse status.
/// @return Partially populated DrawingInfo.
DrawingInfo peek_dwg_header(const std::string& filepath,
                            const DwgParserOptions& options = {},
                            DwgParseResult* out_result = nullptr);

/// Check whether libredwg CLI tools are available on this system.
bool is_libredwg_available(std::string* out_version = nullptr);

} // namespace cad
