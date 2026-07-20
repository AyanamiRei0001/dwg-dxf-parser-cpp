#pragma once
/**
 * @file svg_exporter.h
 * @brief Export cad::Drawing to SVG for browser visualization.
 */

#include "cad_parser/common_types.h"
#include <string>

namespace cad {

struct SvgOptions {
    /// Output image width in SVG units (viewBox handles scaling)
    int width  = 1200;
    int height = 900;

    /// Line width for strokes
    double stroke_width = 1.0;

    /// Color per layer or single color
    bool color_by_layer = true;

    /// Default stroke color (when not color_by_layer)
    std::string default_stroke = "black";

    /// Background color
    std::string background = "white";

    /// If true, auto-fit viewBox to drawing extents
    bool auto_fit = true;

    /// Show entity type labels (for debugging)
    bool show_labels = false;

    /// Hide text/mtext annotations (useful for clean geometry export)
    bool hide_text = false;
};

/// Export a Drawing to an SVG string.
std::string export_svg(const Drawing& drawing, const SvgOptions& opts = {});

/// Export to a file. Returns true on success.
bool export_svg_file(const std::string& filepath,
                     const Drawing& drawing,
                     const SvgOptions& opts = {});

} // namespace cad
