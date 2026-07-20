#pragma once
/**
 * @file dxf_parser.h
 * @brief Pure C++ parser for AutoCAD DXF (Drawing Exchange Format) files.
 *
 * DXF is a text-based format. Each line in a DXF file is a group-code
 * (integer) followed by a value on the next line. The parser reads
 * these pairs and constructs a structured Drawing object.
 *
 * Supported DXF versions: R12 – R2018 (ASCII).
 * Supported entities: LINE, CIRCLE, ARC, ELLIPSE, LWPOLYLINE, POLYLINE,
 *                      TEXT, MTEXT, INSERT, POINT, SOLID, SPLINE,
 *                      DIMENSION, HATCH.
 */

#include "common_types.h"
#include <string>
#include <functional>
#include <iosfwd>

namespace cad {

/// Callback type for progress reporting.
using ProgressCallback = std::function<void(size_t lines_parsed, size_t entities_found)>;

/// Configuration options for the DXF parser.
struct DxfParserOptions {
    /// If true, expand INSERT block references into the entity list.
    bool expand_inserts = false;

    /// If true, convert legacy POLYLINEs into LwPolylineEntity.
    bool convert_heavy_polylines = true;

    /// Maximum number of entities to parse (0 = unlimited).
    size_t max_entities = 0;

    /// Called periodically during parsing for progress updates.
    ProgressCallback on_progress = nullptr;
};

/// Result status for parse operations.
enum class ParseResult {
    Success,
    FileNotFound,
    FileReadError,
    InvalidFormat,
    UnsupportedVersion,
    TruncatedFile,
    InternalError
};

/// Converts a ParseResult to a human-readable string.
const char* to_string(ParseResult result);

/// Parse a DXF file from a file path.
///
/// @param filepath   Path to the .dxf file.
/// @param options    Parser configuration.
/// @param[out] out_result  Detailed parse status.
/// @return A populated Drawing object. Check out_result for success.
Drawing parse_dxf_file(const std::string& filepath,
                       const DxfParserOptions& options = {},
                       ParseResult* out_result = nullptr);

/// Parse DXF content from an in-memory string.
///
/// @param content    The full DXF file contents as a string.
/// @param options    Parser configuration.
/// @param[out] out_result  Detailed parse status.
/// @return A populated Drawing object.
Drawing parse_dxf_string(const std::string& content,
                         const DxfParserOptions& options = {},
                         ParseResult* out_result = nullptr);

/// Quick check: read only the header to get file metadata without
/// parsing every entity.
///
/// @param filepath   Path to the .dxf file.
/// @param[out] out_result  Detailed parse status.
/// @return A partially populated DrawingInfo (no entities/layers/blocks).
DrawingInfo peek_dxf_header(const std::string& filepath,
                            ParseResult* out_result = nullptr);

} // namespace cad
