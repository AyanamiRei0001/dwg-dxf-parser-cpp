/**
 * @file svg_exporter.cpp
 * @brief Convert cad::Drawing entities to SVG elements.
 *
 * SVG coordinate system: Y-axis goes DOWN. CAD Y-axis goes UP.
 * We flip Y: svg_y = max_y - (cad_y - min_y)
 */

#include "svg_exporter.h"

#include <cmath>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <set>
#include <sstream>
#include <iomanip>

namespace cad {

namespace {

std::string rgb_hex(int r, int g, int b) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", r, g, b);
    return buf;
}

std::string aci_color(int index) {
    static const char* basic[] = {
        "#000000", "#FF0000", "#FFFF00", "#00FF00", "#00FFFF",
        "#0000FF", "#FF00FF", "#000000", "#808080", "#C0C0C0"
    };
    if (index >= 0 && index <= 9) return basic[index];
    if (index >= 250 && index <= 255) {
        static const int gray[] = {51, 80, 105, 130, 190, 255};
        return rgb_hex(gray[index - 250], gray[index - 250], gray[index - 250]);
    }
    if (index < 10 || index > 249) return "#000000";

    const int shade = index % 10;
    const double hue = ((index / 10) - 1) * 15.0;
    const double saturation = (shade % 2 == 0) ? 1.0 : 1.0 / 3.0;
    static const double values[] = {1.0, 1.0, 0.74, 0.74, 0.51,
                                    0.51, 0.41, 0.41, 0.31, 0.31};
    const double value = values[shade];
    const double chroma = value * saturation;
    const double h = hue / 60.0;
    const double x = chroma * (1.0 - std::abs(std::fmod(h, 2.0) - 1.0));
    double r = 0, g = 0, b = 0;
    if (h < 1) { r = chroma; g = x; }
    else if (h < 2) { r = x; g = chroma; }
    else if (h < 3) { g = chroma; b = x; }
    else if (h < 4) { g = x; b = chroma; }
    else if (h < 5) { r = x; b = chroma; }
    else { r = chroma; b = x; }
    const double m = value - chroma;
    return rgb_hex(static_cast<int>(std::round((r + m) * 255)),
                   static_cast<int>(std::round((g + m) * 255)),
                   static_cast<int>(std::round((b + m) * 255)));
}

std::string clean_mtext(const std::string& raw) {
    std::string out;
    for (size_t i = 0; i < raw.size();) {
        if (raw[i] == '{' || raw[i] == '}') { ++i; continue; }
        if (raw[i] != '\\') { out += raw[i++]; continue; }

        size_t slash_count = 0;
        while (i < raw.size() && raw[i] == '\\') { ++slash_count; ++i; }
        if (i >= raw.size()) { out.append(slash_count, '\\'); break; }

        const char code = raw[i++];
        if (code == 'P' || code == 'p') { out += '\n'; continue; }
        if (code == '~') { out += ' '; continue; }
        if (code == 'L' || code == 'l' || code == 'O' || code == 'o' ||
            code == 'K' || code == 'k') continue;
        if (std::strchr("ACFfHhQqTtWw", code)) {
            while (i < raw.size() && raw[i] != ';') ++i;
            if (i < raw.size()) ++i;
            continue;
        }
        out.append(slash_count, '\\');
        out += code;
    }
    return out;
}

struct CadTransform {
    double a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;

    Point2D map(double x, double y) const {
        return {a * x + c * y + tx, b * x + d * y + ty};
    }
};

CadTransform compose(const CadTransform& outer, const CadTransform& inner) {
    return {
        outer.a * inner.a + outer.c * inner.b,
        outer.b * inner.a + outer.d * inner.b,
        outer.a * inner.c + outer.c * inner.d,
        outer.b * inner.c + outer.d * inner.d,
        outer.a * inner.tx + outer.c * inner.ty + outer.tx,
        outer.b * inner.tx + outer.d * inner.ty + outer.ty
    };
}

struct EntityBounds {
    bool valid = false;
    double min_x = 0, min_y = 0, max_x = 0, max_y = 0;

    void add(const Point2D& point) {
        if (!std::isfinite(point.x) || !std::isfinite(point.y)) return;
        if (!valid) {
            min_x = max_x = point.x;
            min_y = max_y = point.y;
            valid = true;
            return;
        }
        min_x = std::min(min_x, point.x); min_y = std::min(min_y, point.y);
        max_x = std::max(max_x, point.x); max_y = std::max(max_y, point.y);
    }
};

void add_entity_bounds(const Entity& entity, const Drawing& drawing,
                       const CadTransform& transform, EntityBounds& bounds,
                       std::set<std::string>& active_blocks);

void add_insert_bounds(const InsertEntity& insert, const Drawing& drawing,
                       const CadTransform& parent, EntityBounds& bounds,
                       std::set<std::string>& active_blocks) {
    auto it = drawing.blocks.find(insert.block_name);
    if (it == drawing.blocks.end() || active_blocks.count(insert.block_name)) return;
    active_blocks.insert(insert.block_name);
    const auto& block = it->second;
    const double angle = insert.rotation * M_PI / 180.0;
    const double cs = std::cos(angle), sn = std::sin(angle);
    for (uint16_t row = 0; row < std::max<uint16_t>(1, insert.row_count); ++row) {
        for (uint16_t column = 0; column < std::max<uint16_t>(1, insert.column_count); ++column) {
            const double offset_x = column * insert.column_spacing;
            const double offset_y = row * insert.row_spacing;
            CadTransform local;
            local.a = cs * insert.scale_x; local.c = -sn * insert.scale_y;
            local.b = sn * insert.scale_x; local.d =  cs * insert.scale_y;
            local.tx = insert.insertion_point.x + cs * offset_x - sn * offset_y
                     - local.a * block.base_point.x - local.c * block.base_point.y;
            local.ty = insert.insertion_point.y + sn * offset_x + cs * offset_y
                     - local.b * block.base_point.x - local.d * block.base_point.y;
            const CadTransform combined = compose(parent, local);
            for (const auto& record : block.entities) {
                add_entity_bounds(record.entity, drawing, combined, bounds, active_blocks);
            }
        }
    }
    active_blocks.erase(insert.block_name);
}

void add_entity_bounds(const Entity& entity, const Drawing& drawing,
                       const CadTransform& transform, EntityBounds& bounds,
                       std::set<std::string>& active_blocks) {
    auto add = [&](double x, double y) { bounds.add(transform.map(x, y)); };
    std::visit([&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, LineEntity>) {
            add(value.start.x, value.start.y); add(value.end.x, value.end.y);
        } else if constexpr (std::is_same_v<T, CircleEntity>) {
            for (int i = 0; i < 32; ++i) {
                const double a = i * 2.0 * M_PI / 32.0;
                add(value.center.x + value.radius * std::cos(a),
                    value.center.y + value.radius * std::sin(a));
            }
        } else if constexpr (std::is_same_v<T, ArcEntity>) {
            double span = value.end_angle - value.start_angle;
            if (span < 0) span += 360.0;
            const int steps = std::max(2, static_cast<int>(std::ceil(span / 15.0)));
            for (int i = 0; i <= steps; ++i) {
                const double a = (value.start_angle + span * i / steps) * M_PI / 180.0;
                add(value.center.x + value.radius * std::cos(a),
                    value.center.y + value.radius * std::sin(a));
            }
        } else if constexpr (std::is_same_v<T, EllipseEntity>) {
            const double major = std::hypot(value.major_axis_endpoint.x,
                                            value.major_axis_endpoint.y);
            const double rotation = std::atan2(value.major_axis_endpoint.y,
                                               value.major_axis_endpoint.x);
            double span = value.end_angle - value.start_angle;
            if (span <= 0) span += 360.0;
            for (int i = 0; i <= 48; ++i) {
                const double a = (value.start_angle + span * i / 48.0) * M_PI / 180.0;
                const double x = major * std::cos(a);
                const double y = major * value.axis_ratio * std::sin(a);
                add(value.center.x + x * std::cos(rotation) - y * std::sin(rotation),
                    value.center.y + x * std::sin(rotation) + y * std::cos(rotation));
            }
        } else if constexpr (std::is_same_v<T, LwPolylineEntity> ||
                             std::is_same_v<T, PolylineEntity>) {
            for (const auto& vertex : value.vertices) add(vertex.position.x, vertex.position.y);
        } else if constexpr (std::is_same_v<T, TextEntity>) {
            add(value.insertion_point.x, value.insertion_point.y);
            add(value.insertion_point.x + value.height * std::max<size_t>(1, value.value.size()) * 0.7,
                value.insertion_point.y + value.height);
        } else if constexpr (std::is_same_v<T, MTextEntity>) {
            add(value.insertion_point.x, value.insertion_point.y);
            add(value.insertion_point.x + std::max(value.rect_width, value.height),
                value.insertion_point.y + value.height);
        } else if constexpr (std::is_same_v<T, InsertEntity>) {
            add_insert_bounds(value, drawing, transform, bounds, active_blocks);
        } else if constexpr (std::is_same_v<T, PointEntity>) {
            add(value.position.x, value.position.y);
        } else if constexpr (std::is_same_v<T, SolidEntity>) {
            add(value.p1.x,value.p1.y); add(value.p2.x,value.p2.y);
            add(value.p3.x,value.p3.y); add(value.p4.x,value.p4.y);
        } else if constexpr (std::is_same_v<T, SplineEntity>) {
            for (const auto& point : value.control_points) add(point.x, point.y);
            for (const auto& point : value.fit_points) add(point.x, point.y);
        } else if constexpr (std::is_same_v<T, DimensionEntity>) {
            add(value.definition_point.x, value.definition_point.y);
            add(value.text_midpoint.x, value.text_midpoint.y);
        } else if constexpr (std::is_same_v<T, HatchEntity>) {
            for (const auto& loop : value.boundary_loops)
                for (const auto& point : loop) add(point.x, point.y);
        }
    }, entity);
}

struct SvgContext {
    std::ostringstream os;
    SvgOptions opts;
    double min_x, min_y, max_x, max_y;
    double scale_x, scale_y;
    double offset_x, offset_y;
    double svg_w, svg_h;
    const Drawing* drawing = nullptr;

    void compute_bounds(const Drawing& d) {
        if (opts.auto_fit) {
            EntityBounds bounds;
            std::set<std::string> active_blocks;
            for (const auto& record : d.entities) {
                add_entity_bounds(record.entity, d, {}, bounds, active_blocks);
            }
            if (bounds.valid) {
                min_x = bounds.min_x; min_y = bounds.min_y;
                max_x = bounds.max_x; max_y = bounds.max_y;
            } else {
                min_x = d.info.min_x; min_y = d.info.min_y;
                max_x = d.info.max_x; max_y = d.info.max_y;
            }
        } else {
            // Use drawing extents or fallback
            min_x = d.info.min_x; min_y = d.info.min_y;
            max_x = d.info.max_x; max_y = d.info.max_y;
        }
        // Guard against zero-size drawings
        if (max_x - min_x < 1.0) { max_x = min_x + 100; }
        if (max_y - min_y < 1.0) { max_y = min_y + 100; }

        // Add 5% padding
        double dx = (max_x - min_x) * 0.05;
        double dy = (max_y - min_y) * 0.05;
        min_x -= dx; max_x += dx;
        min_y -= dy; max_y += dy;

        double dw = max_x - min_x;
        double dh = max_y - min_y;
        double aspect = dw / dh;

        if (aspect > (double)opts.width / opts.height) {
            svg_w = opts.width;
            svg_h = opts.width / aspect;
        } else {
            svg_h = opts.height;
            svg_w = opts.height * aspect;
        }

        scale_x = svg_w / dw;
        scale_y = svg_h / dh;
        offset_x = min_x;
        offset_y = max_y; // SVG y=0 is top, CAD y=max is top
    }

    // Convert CAD coords → SVG coords (Y flipped)
    double sx(double x) const { return (x - offset_x) * scale_x; }
    double sy(double y) const { return (offset_y - y) * scale_y; } // flip Y

    std::string layer_color(const std::string& name) const {
        if (!opts.color_by_layer) return opts.default_stroke;
        if (drawing) {
            auto it = drawing->layers.find(name);
            if (it != drawing->layers.end()) return aci_color(std::abs(it->second.color_index));
        }
        return opts.default_stroke;
    }

    std::string entity_color(const EntityRecord& record, const std::string& layer,
                             const std::string& by_block_color) const {
        if (!opts.color_by_layer) return opts.default_stroke;
        if (record.color.index == 0) return by_block_color;
        if (record.color.index > 0 && record.color.index < 256)
            return aci_color(record.color.index);
        return layer_color(layer);
    }
};

struct SvgVisitor {
    SvgContext& ctx;
    const Drawing& drawing;
    std::string layer;
    std::string color;
    std::set<std::string> active_blocks;

    SvgVisitor(SvgContext& context, const Drawing& source)
        : ctx(context), drawing(source), color(context.opts.default_stroke) {}

    void set_entity_color(const EntityRecord& record, const std::string& ln,
                          const std::string& by_block_color) {
        layer = ln;
        color = ctx.entity_color(record, ln, by_block_color);
    }

    // ---- Entity renderers ----

    void operator()(const LineEntity& e) {
        ctx.os << "<line x1=\"" << ctx.sx(e.start.x) << "\" y1=\"" << ctx.sy(e.start.y)
               << "\" x2=\"" << ctx.sx(e.end.x) << "\" y2=\"" << ctx.sy(e.end.y)
               << "\" stroke=\"" << color << "\" stroke-width=\"" << ctx.opts.stroke_width
               << "\" vector-effect=\"non-scaling-stroke\"/>\n";
    }
    void operator()(const CircleEntity& e) {
        double cx = ctx.sx(e.center.x), cy = ctx.sy(e.center.y);
        double r = e.radius * ctx.scale_x; // approximate
        ctx.os << "<circle cx=\"" << cx << "\" cy=\"" << cy << "\" r=\"" << r
               << "\" fill=\"none\" stroke=\"" << color << "\" stroke-width=\"" << ctx.opts.stroke_width
               << "\" vector-effect=\"non-scaling-stroke\"/>\n";
    }
    void operator()(const ArcEntity& e) {
        // Convert arc to SVG path: M start_x,start_y A rx,ry xrot large, sweep end_x,end_y
        double cx = ctx.sx(e.center.x), cy = ctx.sy(e.center.y);
        double r = e.radius * ctx.scale_x;

        double sa = e.start_angle * M_PI / 180.0;
        double ea = e.end_angle   * M_PI / 180.0;
        double x1 = cx + r * std::cos(sa), y1 = cy - r * std::sin(sa);
        double x2 = cx + r * std::cos(ea), y2 = cy - r * std::sin(ea);

        // Determine large-arc and sweep flags
        double diff = ea - sa;
        if (diff < 0) diff += 2 * M_PI;
        int large = (diff > M_PI) ? 1 : 0;
        int sweep = 0; // CAD counter-clockwise after flipping the Y axis

        ctx.os << "<path d=\"M " << x1 << " " << y1
               << " A " << r << " " << r << " 0 " << large << " " << sweep
               << " " << x2 << " " << y2
               << "\" fill=\"none\" stroke=\"" << color << "\" stroke-width=\""
               << ctx.opts.stroke_width << "\" vector-effect=\"non-scaling-stroke\"/>\n";
    }
    void operator()(const EllipseEntity& e) {
        // Simplified as filled + stroked ellipse
        double cx = ctx.sx(e.center.x), cy = ctx.sy(e.center.y);
        double rx = std::hypot(e.major_axis_endpoint.x, e.major_axis_endpoint.y) * ctx.scale_x;
        double ry = rx * e.axis_ratio;
        double angle = std::atan2(e.major_axis_endpoint.y, e.major_axis_endpoint.x) * 180.0 / M_PI;
        ctx.os << "<ellipse cx=\"" << cx << "\" cy=\"" << cy
               << "\" rx=\"" << rx << "\" ry=\"" << ry << "\""
               << " transform=\"rotate(" << -angle << " " << cx << " " << cy << ")\""
               << " fill=\"none\" stroke=\"" << color << "\" stroke-width=\""
               << ctx.opts.stroke_width << "\" vector-effect=\"non-scaling-stroke\"/>\n";
    }
    void operator()(const LwPolylineEntity& e) {
        if (e.vertices.empty()) return;
        ctx.os << "<path d=\"";
        ctx.os << "M " << ctx.sx(e.vertices[0].position.x) << " "
               << ctx.sy(e.vertices[0].position.y);
        const size_t segments = e.closed ? e.vertices.size() : e.vertices.size() - 1;
        for (size_t i = 0; i < segments; ++i) {
            const auto& from = e.vertices[i];
            const auto& to = e.vertices[(i + 1) % e.vertices.size()];
            const double x = ctx.sx(to.position.x), y = ctx.sy(to.position.y);
            if (std::abs(from.bulge) < 1e-12) {
                ctx.os << " L " << x << " " << y;
                continue;
            }
            const double chord = std::hypot(to.position.x - from.position.x,
                                            to.position.y - from.position.y);
            const double radius = chord * (1.0 + from.bulge * from.bulge)
                                / (4.0 * std::abs(from.bulge)) * ctx.scale_x;
            const double angle = 4.0 * std::atan(from.bulge);
            ctx.os << " A " << radius << " " << radius << " 0 "
                   << (std::abs(angle) > M_PI ? 1 : 0) << " "
                   << (from.bulge > 0 ? 0 : 1) << " " << x << " " << y;
        }
        ctx.os << "\" fill=\"none\" stroke=\"" << color << "\" stroke-width=\""
               << ctx.opts.stroke_width << "\" vector-effect=\"non-scaling-stroke\"/>\n";
    }
    void operator()(const PolylineEntity& e) {
        if (e.vertices.empty()) return;
        ctx.os << "<path d=\"";
        for (size_t i = 0; i < e.vertices.size(); ++i) {
            auto& v = e.vertices[i];
            double x = ctx.sx(v.position.x), y = ctx.sy(v.position.y);
            if (i == 0) ctx.os << "M " << x << " " << y;
            else ctx.os << " L " << x << " " << y;
        }
        if (e.closed) {
            auto& v0 = e.vertices[0];
            ctx.os << " L " << ctx.sx(v0.position.x) << " " << ctx.sy(v0.position.y);
        }
        ctx.os << "\" fill=\"none\" stroke=\"" << color << "\" stroke-width=\""
               << ctx.opts.stroke_width << "\" vector-effect=\"non-scaling-stroke\"/>\n";
    }
    void operator()(const TextEntity& e) {
        ctx.os << "<text x=\"" << ctx.sx(e.insertion_point.x)
               << "\" y=\"" << ctx.sy(e.insertion_point.y)
               << "\" font-size=\"" << (e.height * ctx.scale_x * 0.8)
               << "\" fill=\"" << color << "\" font-family=\"sans-serif\""
               << " transform=\"rotate(" << -e.rotation << " "
               << ctx.sx(e.insertion_point.x) << " " << ctx.sy(e.insertion_point.y) << ")\""
               << ">" << escape_xml(e.value) << "</text>\n";
    }
    void operator()(const MTextEntity& e) {
        const std::string text = clean_mtext(e.value);
        std::istringstream lines(text);
        std::string line_text;
        int line_number = 0;
        ctx.os << "<text x=\"" << ctx.sx(e.insertion_point.x)
               << "\" y=\"" << ctx.sy(e.insertion_point.y)
               << "\" font-size=\"" << (e.height * ctx.scale_x * 0.8)
               << "\" fill=\"" << color << "\" font-family=\"sans-serif\">";
        while (std::getline(lines, line_text)) {
            ctx.os << "<tspan x=\"" << ctx.sx(e.insertion_point.x) << "\"";
            if (line_number++ > 0) ctx.os << " dy=\"1.2em\"";
            ctx.os << ">" << escape_xml(line_text) << "</tspan>";
        }
        ctx.os << "</text>\n";
    }
    void operator()(const InsertEntity& e) {
        if (e.block_name.empty()) return;
        auto block_it = drawing.blocks.find(e.block_name);
        if (block_it == drawing.blocks.end() || block_it->second.entities.empty() ||
            active_blocks.count(e.block_name)) return;

        active_blocks.insert(e.block_name);
        const auto& block = block_it->second;
        const double radians = e.rotation * M_PI / 180.0;
        const double cs = std::cos(radians), sn = std::sin(radians);
        const auto previous_layer = layer;
        const auto previous_color = color;
        for (uint16_t row = 0; row < std::max<uint16_t>(1, e.row_count); ++row) {
            for (uint16_t column = 0; column < std::max<uint16_t>(1, e.column_count); ++column) {
                const double local_x = column * e.column_spacing;
                const double local_y = row * e.row_spacing;
                const double insert_x = e.insertion_point.x + cs * local_x - sn * local_y;
                const double insert_y = e.insertion_point.y + sn * local_x + cs * local_y;
                // Child geometry is already emitted in SVG coordinates. Map
                // the CAD insert affine transform through the CAD->SVG basis
                // change, rather than mixing CAD coordinates with SVG ones.
                const double a = cs * e.scale_x;
                const double b = sn * e.scale_x;
                const double c = -sn * e.scale_y;
                const double d = cs * e.scale_y;
                const double tx = insert_x - a * block.base_point.x - c * block.base_point.y;
                const double ty = insert_y - b * block.base_point.x - d * block.base_point.y;

                const double sx = ctx.scale_x;
                const double sy = ctx.scale_y;
                const double ox = -sx * ctx.offset_x;
                const double oy = sy * ctx.offset_y;
                const double ma = a;
                const double mb = -sy * b / sx;
                const double mc = -sx * c / sy;
                const double md = d;
                const double me = sx * tx + ox - ma * ox - mc * oy;
                const double mf = -sy * ty + oy - mb * ox - md * oy;

                ctx.os << "<g transform=\"matrix(" << ma << " " << mb << " " << mc << " "
                       << md << " " << me << " " << mf << ")\">\n";
                for (const auto& rec : block.entities) {
                    const std::string effective_layer = rec.layer_name == "0" ? previous_layer : rec.layer_name;
                    set_entity_color(rec, effective_layer, previous_color);
                    std::visit(*this, rec.entity);
                }
                ctx.os << "</g>\n";
            }
        }
        layer = previous_layer;
        color = previous_color;
        active_blocks.erase(e.block_name);
    }
    void operator()(const PointEntity& e) {
        double x = ctx.sx(e.position.x), y = ctx.sy(e.position.y);
        ctx.os << "<circle cx=\"" << x << "\" cy=\"" << y << "\" r=\"2\" fill=\"" << color << "\"/>\n";
    }
    void operator()(const SolidEntity& e) {
        ctx.os << "<polygon points=\""
               << ctx.sx(e.p1.x) << "," << ctx.sy(e.p1.y) << " "
               << ctx.sx(e.p2.x) << "," << ctx.sy(e.p2.y) << " "
               << ctx.sx(e.p3.x) << "," << ctx.sy(e.p3.y);
        if (e.p4.x != e.p3.x || e.p4.y != e.p3.y) {
            ctx.os << " " << ctx.sx(e.p4.x) << "," << ctx.sy(e.p4.y);
        }
        ctx.os << "\" fill=\"" << color << "\" fill-opacity=\"0.3\" stroke=\"" << color << "\"/>\n";
    }
    void operator()(const SplineEntity& e) {
        // Render control points as connected line segments
        if (e.control_points.size() < 2) return;
        ctx.os << "<polyline points=\"";
        for (auto& p : e.control_points) {
            ctx.os << ctx.sx(p.x) << "," << ctx.sy(p.y) << " ";
        }
        ctx.os << "\" fill=\"none\" stroke=\"" << color << "\" stroke-width=\""
               << ctx.opts.stroke_width << "\" stroke-dasharray=\"4,2\""
               << " vector-effect=\"non-scaling-stroke\"/>\n";
    }
    void operator()(const DimensionEntity& e) {
        // Simple dimension: draw a line + text
        double x1 = ctx.sx(e.definition_point.x), y1 = ctx.sy(e.definition_point.y);
        double x2 = ctx.sx(e.text_midpoint.x), y2 = ctx.sy(e.text_midpoint.y);
        ctx.os << "<line x1=\"" << x1 << "\" y1=\"" << y1
               << "\" x2=\"" << x2 << "\" y2=\"" << y2
               << "\" stroke=\"#888\" stroke-width=\"0.5\" vector-effect=\"non-scaling-stroke\"/>\n";
        if (!e.text_override.empty()) {
            ctx.os << "<text x=\"" << x2 << "\" y=\"" << y2
                   << "\" font-size=\"8\" fill=\"#888\">" << escape_xml(e.text_override) << "</text>\n";
        }
    }
    void operator()(const HatchEntity& e) {
        // Simple: fill the first boundary loop
        for (auto& loop : e.boundary_loops) {
            if (loop.size() < 2) continue;
            ctx.os << "<polygon points=\"";
            for (auto& p : loop) ctx.os << ctx.sx(p.x) << "," << ctx.sy(p.y) << " ";
            ctx.os << "\" fill=\"" << color << "\" fill-opacity=\"0.15\" stroke=\"" << color
                   << "\" stroke-width=\"0.3\"/>\n";
            break; // only first loop
        }
    }

    static std::string escape_xml(const std::string& s) {
        std::string out;
        for (size_t i = 0; i < s.size();) {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            if (c < 0x20 && c != '\n' && c != '\r' && c != '\t') {
                out += "\xEF\xBF\xBD";
                ++i;
                continue;
            }
            switch (c) {
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '&': out += "&amp;"; break;
            case '"': out += "&quot;"; break;
                default:
                    if (c < 0x80) {
                        out += static_cast<char>(c);
                        ++i;
                        continue;
                    }

                    size_t width = 0;
                    if (c >= 0xC2 && c <= 0xDF) width = 2;
                    else if (c >= 0xE0 && c <= 0xEF) width = 3;
                    else if (c >= 0xF0 && c <= 0xF4) width = 4;
                    bool valid = width != 0 && i + width <= s.size();
                    for (size_t j = 1; valid && j < width; ++j) {
                        valid = (static_cast<unsigned char>(s[i + j]) & 0xC0) == 0x80;
                    }
                    if (valid) {
                        out.append(s, i, width);
                        i += width;
                    } else {
                        out += "\xEF\xBF\xBD";
                        ++i;
                    }
                    continue;
            }
            ++i;
        }
        return out;
    }
};

} // anonymous namespace

// =========================================================================
//  Public API
// =========================================================================

std::string export_svg(const Drawing& drawing, const SvgOptions& opts) {
    SvgContext ctx;
    ctx.opts = opts;
    ctx.drawing = &drawing;

    if (drawing.entities.empty()) {
        return "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"200\" height=\"100\">"
               "<text x=\"10\" y=\"30\" font-size=\"14\">(Empty drawing)</text></svg>";
    }

    ctx.compute_bounds(drawing);

    // SVG header
    ctx.os << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    ctx.os << "<svg xmlns=\"http://www.w3.org/2000/svg\" "
           << "width=\"" << ctx.opts.width << "\" height=\"" << ctx.opts.height << "\" "
           << "viewBox=\"0 0 " << ctx.svg_w << " " << ctx.svg_h << "\">\n";

    // Background
    ctx.os << "<rect width=\"" << ctx.svg_w << "\" height=\"" << ctx.svg_h
           << "\" fill=\"" << ctx.opts.background << "\"/>\n";

    // Group for entities
    ctx.os << "<g id=\"cad-entities\">\n";

    SvgVisitor vis(ctx, drawing);

    for (auto& rec : drawing.entities) {
        // Skip text if hide_text is set
        if (ctx.opts.hide_text) {
            if (std::holds_alternative<TextEntity>(rec.entity) ||
                std::holds_alternative<MTextEntity>(rec.entity)) {
                continue;
            }
        }
        vis.set_entity_color(rec, rec.layer_name, ctx.opts.default_stroke);
        std::visit(vis, rec.entity);
    }

    ctx.os << "</g>\n";

    // Info overlay (top-left)
    ctx.os << "<g transform=\"translate(10,20)\">\n"
           << "<text x=\"0\" y=\"0\" font-size=\"11\" fill=\"#333\" font-family=\"monospace\">"
           << "File: " << SvgVisitor::escape_xml(drawing.info.filename)
           << " | Entities: " << drawing.info.entity_count
           << " | Layers: " << drawing.info.layer_count
           << "</text>\n"
           << "</g>\n";

    ctx.os << "</svg>\n";
    return ctx.os.str();
}

bool export_svg_file(const std::string& filepath,
                     const Drawing& drawing,
                     const SvgOptions& opts) {
    std::ofstream out(filepath);
    if (!out.is_open()) return false;
    out << export_svg(drawing, opts);
    return out.good();
}

} // namespace cad
