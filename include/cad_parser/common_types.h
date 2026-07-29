#pragma once
/**
 * @file common_types.h
 * @brief Common CAD entity types shared between DXF and DWG parsers.
 *
 * Defines core geometric primitives and entity representations
 * that both DXF and DWG formats map to after parsing.
 */

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <variant>
#include <map>

namespace cad {

// Portable replacement for the non-standard M_PI macro. Keeping this in the
// shared model header makes all parser and renderer translation units use the
// same angle conversion constant on GCC, Clang, and MSVC.
inline constexpr double kPi = 3.141592653589793238462643383279502884;

// ---- Geometric Primitives ----

/// A 2D point.
struct Point2D {
    double x = 0.0;
    double y = 0.0;
};

/// A 3D point.
struct Point3D {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

/// A bounding box in 2D.
struct BoundingBox2D {
    double min_x = 0.0;
    double min_y = 0.0;
    double max_x = 0.0;
    double max_y = 0.0;

    [[nodiscard]] double width()  const { return max_x - min_x; }
    [[nodiscard]] double height() const { return max_y - min_y; }
};

// ---- Color ----

/// RGB color with optional alpha and color-index fallback.
struct Color {
    uint8_t r = 255;
    uint8_t g = 255;
    uint8_t b = 255;
    uint8_t a = 255;

    /// AutoCAD Color Index (ACI), 0 = BYLAYER, 256 = BYBLOCK.
    /// Set to -1 when an RGB value is used instead.
    int16_t index = -1;
};

// ---- Line Weight & Line Type ----

enum class LineWeight : int16_t {
    BYLAYER    = -1,
    BYBLOCK    = -2,
    DEFAULT    = -3,
    LW_0_00    = 0,
    LW_0_05    = 5,
    LW_0_09    = 9,
    LW_0_13    = 13,
    LW_0_15    = 15,
    LW_0_18    = 18,
    LW_0_20    = 20,
    LW_0_25    = 25,
    LW_0_30    = 30,
    LW_0_35    = 35,
    LW_0_40    = 40,
    LW_0_50    = 50,
    LW_0_53    = 53,
    LW_0_60    = 60,
    LW_0_70    = 70,
    LW_0_80    = 80,
    LW_0_90    = 90,
    LW_1_00    = 100,
    LW_1_06    = 106,
    LW_1_20    = 120,
    LW_1_40    = 140,
    LW_1_58    = 158,
    LW_2_00    = 200,
    LW_2_11    = 211,
};

// ---- Layer ----

/// Represents a CAD layer.
struct Layer {
    std::string name;
    int16_t color_index = 7;         // ACI, default white/black
    LineWeight line_weight = LineWeight::DEFAULT;
    std::string line_type_name = "Continuous";
    bool frozen  = false;
    bool locked  = false;
    bool visible = true;
    bool plotted = true;
};

// ---- Entity Types ----

/// A LINE entity.
struct LineEntity {
    Point3D start;
    Point3D end;
    double thickness = 0.0;          // extrusion thickness
};

/// A CIRCLE entity.
struct CircleEntity {
    Point3D center;
    double radius = 0.0;
    double thickness = 0.0;
};

/// An ARC entity (circular arc).
struct ArcEntity {
    Point3D center;
    double radius     = 0.0;
    double start_angle = 0.0;        // degrees
    double end_angle   = 0.0;        // degrees
    double thickness   = 0.0;
};

/// An ELLIPSE entity.
struct EllipseEntity {
    Point3D center;
    Point3D major_axis_endpoint;      // relative to center
    double axis_ratio = 1.0;          // minor/major
    double start_angle = 0.0;         // degrees
    double end_angle   = 360.0;       // degrees
};

/// A lightweight polyline vertex.
struct LwPolylineVertex {
    Point2D position;
    double bulge       = 0.0;         // arc bulge factor
    double start_width = 0.0;
    double end_width   = 0.0;
};

/// A lightweight POLYLINE (LWPolyline).
struct LwPolylineEntity {
    std::vector<LwPolylineVertex> vertices;
    bool closed    = false;
    double elevation = 0.0;
    double thickness = 0.0;
};

/// A legacy heavy POLYLINE vertex.
struct PolylineVertex {
    Point3D position;
    double bulge       = 0.0;
    double start_width = 0.0;
    double end_width   = 0.0;
};

/// A legacy heavy POLYLINE.
struct PolylineEntity {
    std::vector<PolylineVertex> vertices;
    bool closed    = false;
    double thickness = 0.0;
};

/// A TEXT entity (single-line).
struct TextEntity {
    Point3D insertion_point;
    Point3D alignment_point;
    bool has_alignment_point = false;
    double height      = 2.5;
    std::string value;
    double rotation     = 0.0;        // degrees
    double oblique      = 0.0;        // degrees
    std::string style   = "Standard";
    int32_t horizontal_alignment = 0;  // 0=left, 1=center, 2=right, 3=aligned, 4=middle, 5=fit
    int32_t vertical_alignment   = 0;  // 0=baseline, 1=bottom, 2=middle, 3=top
};

/// An MTEXT entity (multi-line text).
struct MTextEntity {
    Point3D insertion_point;
    double height      = 2.5;
    std::string value;
    double rotation     = 0.0;
    double rect_width   = 0.0;
    int32_t attachment_point = 1;     // 1=TopLeft, 2=TopCenter, ...
    std::string style   = "Standard";
};

/// An INSERT (block reference) entity.
struct InsertEntity {
    std::string block_name;
    Point3D insertion_point;
    double scale_x = 1.0;
    double scale_y = 1.0;
    double scale_z = 1.0;
    double rotation = 0.0;            // degrees
    uint16_t column_count = 1;
    uint16_t row_count    = 1;
    double column_spacing = 0.0;
    double row_spacing    = 0.0;
};

/// A POINT entity.
struct PointEntity {
    Point3D position;
};

/// A SOLID / 3DFACE entity (4-point fill).
struct SolidEntity {
    Point3D p1, p2, p3, p4;
};

/// A SPLINE entity.
struct SplineEntity {
    std::vector<Point3D> control_points;
    std::vector<Point3D> fit_points;
    std::vector<double> knots;
    int32_t degree          = 3;
    bool closed             = false;
    bool periodic           = false;
};

/// A DIMENSION entity (simplified).
struct DimensionEntity {
    std::string type;                 // "Aligned", "Rotated", "Radial", "Diameter", "Angular", "Ordinate"
    Point3D definition_point;
    Point3D text_midpoint;
    std::string text_override;
    double text_rotation = 0.0;
};

/// A HATCH entity (simplified).
struct HatchEntity {
    std::string pattern_name;
    double pattern_scale = 1.0;
    double pattern_angle = 0.0;
    bool associative = true;
    std::vector<std::vector<Point2D>> boundary_loops;
};

/// The unified entity type — any CAD entity.
using Entity = std::variant<
    LineEntity,
    CircleEntity,
    ArcEntity,
    EllipseEntity,
    LwPolylineEntity,
    PolylineEntity,
    TextEntity,
    MTextEntity,
    InsertEntity,
    PointEntity,
    SolidEntity,
    SplineEntity,
    DimensionEntity,
    HatchEntity
>;

/// Entity with common header data.
struct EntityRecord {
    Entity entity;
    std::string layer_name = "0";
    Color color;
    std::string line_type_name = "ByLayer";
    LineWeight line_weight = LineWeight::BYLAYER;
    std::string handle;               // DXF handle / DWG object ID
};

// ---- Block Definition ----

/// A block definition containing its own entity list.
struct BlockRecord {
    std::string name;
    Point3D base_point;
    std::vector<EntityRecord> entities;
    std::string description;
};

// ---- Drawing Info ----

/// High-level drawing metadata.
struct DrawingInfo {
    std::string filename;
    std::string format;               // "DXF" or "DWG"
    std::string version;              // e.g. "AC1027" (DXF 2013-2017)
    double min_x = 0, min_y = 0, min_z = 0;
    double max_x = 0, max_y = 0, max_z = 0;
    size_t entity_count  = 0;
    size_t layer_count   = 0;
    size_t block_count   = 0;
};

// ---- Parsed Drawing ----

/// The complete parsed drawing data structure.
struct Drawing {
    DrawingInfo info;
    std::vector<EntityRecord> entities;
    std::map<std::string, Layer> layers;
    std::map<std::string, BlockRecord> blocks;
};

/// Return a human-readable entity type name (e.g. "LINE", "CIRCLE").
std::string entity_type_name(const Entity& e);

} // namespace cad
