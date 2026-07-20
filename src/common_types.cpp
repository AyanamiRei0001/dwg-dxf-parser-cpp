#include "cad_parser/common_types.h"

namespace cad {

// This file is intentionally light — the core data structures are in the header.
// Add serialization/diagnostic helpers here as needed.

// ---- Entity visitor helpers ----

namespace detail {

// Helper to get human-readable entity type name from a variant.
struct EntityNameVisitor {
    std::string operator()(const LineEntity&)        const { return "LINE"; }
    std::string operator()(const CircleEntity&)      const { return "CIRCLE"; }
    std::string operator()(const ArcEntity&)         const { return "ARC"; }
    std::string operator()(const EllipseEntity&)     const { return "ELLIPSE"; }
    std::string operator()(const LwPolylineEntity&)  const { return "LWPOLYLINE"; }
    std::string operator()(const PolylineEntity&)    const { return "POLYLINE"; }
    std::string operator()(const TextEntity&)        const { return "TEXT"; }
    std::string operator()(const MTextEntity&)       const { return "MTEXT"; }
    std::string operator()(const InsertEntity&)      const { return "INSERT"; }
    std::string operator()(const PointEntity&)       const { return "POINT"; }
    std::string operator()(const SolidEntity&)       const { return "SOLID"; }
    std::string operator()(const SplineEntity&)      const { return "SPLINE"; }
    std::string operator()(const DimensionEntity&)   const { return "DIMENSION"; }
    std::string operator()(const HatchEntity&)       const { return "HATCH"; }
};

} // namespace detail

std::string entity_type_name(const Entity& e) {
    return std::visit(detail::EntityNameVisitor{}, e);
}

} // namespace cad
