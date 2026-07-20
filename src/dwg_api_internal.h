/**
 * @file dwg_api_internal.h
 * @brief Internal declarations for the libredwg C API backend.
 *
 * This header is NOT part of the public API — it's only included
 * when CAD_HAS_LIBREDWG_API is defined and the implementation in
 * dwg_api_impl.cpp is compiled in.
 */

#pragma once

#include "cad_parser/dwg_parser.h"
#include "cad_parser/common_types.h"

namespace cad {
namespace dwg_api {

/// Parse a DWG file using the libredwg C API directly.
Drawing parse_file(const std::string& filepath,
                   const DwgParserOptions& options,
                   DwgParseResult* out_result);

/// Quick header peek using the C API.
DrawingInfo peek_header(const std::string& filepath,
                        const DwgParserOptions& options,
                        DwgParseResult* out_result);

/// Check if the API backend was successfully linked.
bool is_available(std::string* out_version = nullptr);

} // namespace dwg_api
} // namespace cad
