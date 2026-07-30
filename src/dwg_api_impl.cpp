/**
 * @file dwg_api_impl.cpp
 * @brief DWG parser backend using libredwg-0.13 C API (linked statically).
 */
#include "dwg_api_internal.h"

#include <cmath>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

extern "C" {
#include <dwg.h>
#include <dwg_api.h>
}

namespace cad {
namespace dwg_api {

namespace {

inline std::string safe_str(const char* s) { return s ? s : ""; }

// r2007+ DWGs store BITCODE_T fields as UTF-16.  Reading those pointers as
// char strings truncates names at the first ASCII character and produces
// invalid UTF-8 in SVG output.  LibreDWG owns the conversion details.
std::string utf8_text(void* object, const char* type, const char* field,
                      const char* fallback) {
    char* text = nullptr;
    int is_new = 0;
    if (object && dwg_dynapi_entity_utf8text(object, type, field, &text,
                                              &is_new, nullptr) && text) {
        std::string value(text);
        if (is_new) std::free(text);
        return value;
    }
    return safe_str(fallback);
}

inline Point3D to_pt(const Dwg_Bitcode_3BD& p) { return { p.x, p.y, p.z }; }
inline Point3D to_pt3(const Dwg_Bitcode_2RD& p) { return { p.x, p.y, 0.0 }; }
inline Point3D to_pt3(const Dwg_Bitcode_2BD& p) { return { p.x, p.y, 0.0 }; }
inline Point3D to_pt(const Dwg_SPLINE_control_point& p) { return { p.x, p.y, p.z }; }

inline Color to_color(const Dwg_Color& c) {
    Color col;
    if (c.index >= 0 && c.index <= 256) col.index = static_cast<int16_t>(c.index);
    const uint32_t rgb = static_cast<uint32_t>(c.rgb) & 0x00FFFFFFu;
    col.r = static_cast<uint8_t>((rgb >> 16) & 0xFF);
    col.g = static_cast<uint8_t>((rgb >> 8) & 0xFF);
    col.b = static_cast<uint8_t>(rgb & 0xFF);
    return col;
}

// LibreDWG represents this drawing's layer ACI colors as CMC method 0xC3
// with the ACI value in the low byte (for example 0xC3000004 for cyan).
// Preserve normal true-color values, which need a richer public type than a
// Layer's color_index, but decode this standard ACI representation.
inline int16_t layer_color_index(const Dwg_Color& color) {
    const uint32_t raw = static_cast<uint32_t>(color.rgb);
    const uint32_t payload = raw & 0x00FFFFFFu;
    if (color.index == 256 && ((raw >> 24) & 0xFFu) == 0xC3u &&
        payload <= 255u) {
        return static_cast<int16_t>(payload);
    }
    return static_cast<int16_t>(color.index);
}
inline Color entity_color(const Dwg_Object_Entity* ent) {
    return ent ? to_color(ent->color) : Color();
}
inline double rad2deg(double r) { return r * 180.0 / kPi; }

// Handle references in malformed drawings can point outside libredwg's
// resolved-reference table. Build a trusted index once instead of rescanning
// every reference and object for each entity.
class ObjectReferenceIndex {
public:
    explicit ObjectReferenceIndex(const Dwg_Data* dwg) {
        if (!dwg || !dwg->object || !dwg->object_ref) return;

        std::unordered_set<const Dwg_Object*> objects;
        objects.reserve(dwg->num_objects);
        for (BITCODE_BL i = 0; i < dwg->num_objects; ++i) {
            objects.insert(&dwg->object[i]);
        }

        references_.reserve(dwg->num_object_refs);
        for (BITCODE_BL i = 0; i < dwg->num_object_refs; ++i) {
            Dwg_Object_Ref* ref = dwg->object_ref[i];
            if (ref && ref->obj && objects.count(ref->obj)) {
                references_.emplace(ref, ref->obj);
            }
        }
    }

    Dwg_Object* find(BITCODE_H ref) const {
        if (!ref) return nullptr;
        auto it = references_.find(ref);
        return it == references_.end() ? nullptr : it->second;
    }

private:
    std::unordered_map<const Dwg_Object_Ref*, Dwg_Object*> references_;
};

std::string resolve_layer(const Dwg_Object_Entity* ent,
                          const ObjectReferenceIndex& references) {
    if (!ent || !ent->layer) return "0";
    Dwg_Object* layer_obj = references.find(ent->layer);
    if (!layer_obj || layer_obj->fixedtype != DWG_TYPE_LAYER ||
        !layer_obj->tio.object || !layer_obj->tio.object->tio.LAYER) return "0";

    auto* layer = layer_obj->tio.object->tio.LAYER;
    return utf8_text(layer, "LAYER", "name", layer->name);
}

std::string resolve_block_name(BITCODE_H block_ref,
                               const ObjectReferenceIndex& references) {
    Dwg_Object* block_obj = references.find(block_ref);
    if (!block_obj || block_obj->fixedtype != DWG_TYPE_BLOCK_HEADER ||
        !block_obj->tio.object || !block_obj->tio.object->tio.BLOCK_HEADER) return "";
    auto* block = block_obj->tio.object->tio.BLOCK_HEADER;
    return utf8_text(block, "BLOCK_HEADER", "name", block->name);
}

EntityRecord make_rec(Dwg_Object* obj, const ObjectReferenceIndex& references) {
    EntityRecord rec;
    auto* ent = obj->tio.entity;
    if (ent) {
        rec.layer_name = resolve_layer(ent, references);
        rec.color = entity_color(ent);
        char b[32];
        std::snprintf(b, sizeof(b), "%" PRIX64,
                      static_cast<uint64_t>(obj->handle.value));
        rec.handle = b;
    }
    return rec;
}

// ---- Entity converters (each null-checks its tio pointer) ----

Entity conv_line(Dwg_Object* o, Dwg_Data*) {
    LineEntity e;
    if (auto* L = o->tio.entity->tio.LINE) {
        e.start = to_pt(L->start); e.end = to_pt(L->end);
        e.thickness = L->thickness;
    }
    return e;
}
Entity conv_circle(Dwg_Object* o, Dwg_Data*) {
    CircleEntity e;
    if (auto* c = o->tio.entity->tio.CIRCLE) {
        e.center = to_pt(c->center); e.radius = c->radius; e.thickness = c->thickness;
    }
    return e;
}
Entity conv_arc(Dwg_Object* o, Dwg_Data*) {
    ArcEntity e;
    if (auto* a = o->tio.entity->tio.ARC) {
        e.center = to_pt(a->center); e.radius = a->radius;
        e.start_angle = rad2deg(a->start_angle); e.end_angle = rad2deg(a->end_angle);
        e.thickness = a->thickness;
    }
    return e;
}
Entity conv_ellipse(Dwg_Object* o, Dwg_Data*) {
    EllipseEntity e;
    if (auto* ell = o->tio.entity->tio.ELLIPSE) {
        e.center = to_pt(ell->center); e.major_axis_endpoint = to_pt(ell->sm_axis);
        e.axis_ratio = ell->axis_ratio;
        e.start_angle = rad2deg(ell->start_angle); e.end_angle = rad2deg(ell->end_angle);
    }
    return e;
}
Entity conv_lwpolyline(Dwg_Object* o, Dwg_Data*) {
    LwPolylineEntity e;
    if (auto* lwp = o->tio.entity->tio.LWPOLYLINE) {
        // DWG stores the closed bit at 512. DXF serializes the same state as bit 1.
        e.closed = (lwp->flag & 512) != 0;
        e.elevation = lwp->elevation; e.thickness = lwp->thickness;
        for (BITCODE_BL i = 0; i < lwp->num_points; ++i) {
            LwPolylineVertex v;
            v.position = { lwp->points[i].x, lwp->points[i].y };
            if (i < lwp->num_bulges) v.bulge = lwp->bulges[i];
            if (lwp->widths && i < lwp->num_widths) {
                v.start_width = lwp->widths[i].start; v.end_width = lwp->widths[i].end;
            }
            e.vertices.push_back(v);
        }
    }
    return e;
}
Entity conv_polyline(Dwg_Object* o, Dwg_Data*) {
    PolylineEntity e;
    if (auto* pl = o->tio.entity->tio.POLYLINE_3D)
        e.closed = (pl->flag & 1) != 0;
    return e;
}
Entity conv_text(Dwg_Object* o, Dwg_Data*) {
    TextEntity e;
    if (auto* t = o->tio.entity->tio.TEXT) {
        e.insertion_point = to_pt3(t->ins_pt); e.height = t->height;
        e.value = utf8_text(t, "TEXT", "text_value", t->text_value);
        e.rotation = rad2deg(t->rotation);
        e.oblique = rad2deg(t->oblique_angle);
        e.horizontal_alignment = t->horiz_alignment;
        e.vertical_alignment = t->vert_alignment;
        e.has_alignment_point = e.horizontal_alignment != 0 || e.vertical_alignment != 0;
        if (e.has_alignment_point) e.alignment_point = to_pt3(t->alignment_pt);
    }
    return e;
}
Entity conv_mtext(Dwg_Object* o, Dwg_Data*) {
    MTextEntity e;
    if (auto* mt = o->tio.entity->tio.MTEXT) {
        e.insertion_point = to_pt(mt->ins_pt); e.height = mt->text_height;
        e.value = utf8_text(mt, "MTEXT", "text", mt->text);
        e.rect_width = mt->rect_width;
        e.attachment_point = mt->attachment;
    }
    return e;
}
Entity conv_insert(Dwg_Object* o, const ObjectReferenceIndex& references) {
    InsertEntity e;
    if (auto* ins = o->tio.entity->tio.INSERT) {
        e.block_name = resolve_block_name(ins->block_header, references);
        if (e.block_name.empty())
            e.block_name = utf8_text(ins, "INSERT", "block_name", ins->block_name);
        e.insertion_point = to_pt(ins->ins_pt);
        e.scale_x = ins->scale.x; e.scale_y = ins->scale.y; e.scale_z = ins->scale.z;
        e.rotation = rad2deg(ins->rotation);
        e.column_count = static_cast<uint16_t>(ins->num_cols);
        e.row_count    = static_cast<uint16_t>(ins->num_rows);
        e.column_spacing = ins->col_spacing; e.row_spacing = ins->row_spacing;
    }
    return e;
}
Entity conv_point(Dwg_Object* o, Dwg_Data*) {
    PointEntity e;
    if (auto* pt = o->tio.entity->tio.POINT)
        e.position = { pt->x, pt->y, pt->z };
    return e;
}
Entity conv_solid(Dwg_Object* o, Dwg_Data*) {
    SolidEntity e;
    if (o->fixedtype == DWG_TYPE__3DFACE) {
        if (auto* f = o->tio.entity->tio._3DFACE) {
            e.p1 = to_pt(f->corner1); e.p2 = to_pt(f->corner2);
            e.p3 = to_pt(f->corner3); e.p4 = to_pt(f->corner4);
        }
    } else {
        if (auto* s = o->tio.entity->tio.SOLID) {
            e.p1 = to_pt3(s->corner1); e.p2 = to_pt3(s->corner2);
            e.p3 = to_pt3(s->corner3); e.p4 = to_pt3(s->corner4);
        }
    }
    return e;
}
Entity conv_spline(Dwg_Object* o, Dwg_Data*) {
    SplineEntity e;
    if (auto* spl = o->tio.entity->tio.SPLINE) {
        e.degree = spl->degree; e.closed = spl->closed_b; e.periodic = spl->periodic;
        for (BITCODE_BL i = 0; i < spl->num_ctrl_pts; ++i)
            e.control_points.push_back(to_pt(spl->ctrl_pts[i]));
        for (BITCODE_BS i = 0; i < spl->num_fit_pts; ++i)
            e.fit_points.push_back(to_pt(spl->fit_pts[i]));
        for (BITCODE_BL i = 0; i < spl->num_knots; ++i)
            e.knots.push_back(spl->knots[i]);
    }
    return e;
}
Entity conv_dimension(Dwg_Object* o, Dwg_Data*) {
    DimensionEntity e;
    auto* dim = reinterpret_cast<Dwg_Entity_DIMENSION_LINEAR*>(o->tio.entity->tio.DIMENSION_LINEAR);
    if (!dim) return e;
    switch (o->fixedtype) {
        case DWG_TYPE_DIMENSION_ORDINATE: e.type = "Ordinate"; break;
        case DWG_TYPE_DIMENSION_LINEAR:   e.type = "Rotated"; break;
        case DWG_TYPE_DIMENSION_ALIGNED:  e.type = "Aligned"; break;
        case DWG_TYPE_DIMENSION_ANG3PT:   e.type = "Angular"; break;
        case DWG_TYPE_DIMENSION_ANG2LN:   e.type = "Angular"; break;
        case DWG_TYPE_DIMENSION_RADIUS:   e.type = "Radius"; break;
        case DWG_TYPE_DIMENSION_DIAMETER: e.type = "Diameter"; break;
        default: e.type = "Unknown";
    }
    e.definition_point = to_pt(dim->def_pt);
    e.text_midpoint    = to_pt3(dim->text_midpt);
    e.text_override    = safe_str(dim->user_text);
    return e;
}
Entity conv_hatch(Dwg_Object* o, Dwg_Data*) {
    HatchEntity e;
    if (auto* h = o->tio.entity->tio.HATCH) {
        e.pattern_name = utf8_text(h, "HATCH", "name", h->name);
        e.pattern_scale = h->scale_spacing;
        e.pattern_angle = h->angle; e.associative = h->is_associative;
    }
    return e;
}

bool convert_entity(Dwg_Object* obj, Dwg_Data* dwg, EntityRecord& rec,
                    const ObjectReferenceIndex& references) {
    if (!obj || obj->supertype != DWG_SUPERTYPE_ENTITY || !obj->tio.entity) return false;
    rec = make_rec(obj, references);
    bool ok = true;
#define CASE(T, F) case DWG_TYPE_##T: rec.entity = F(obj, dwg); break
    switch (obj->fixedtype) {
        CASE(LINE, conv_line); CASE(CIRCLE, conv_circle); CASE(ARC, conv_arc);
        CASE(ELLIPSE, conv_ellipse); CASE(LWPOLYLINE, conv_lwpolyline);
        CASE(POLYLINE_2D, conv_polyline); CASE(POLYLINE_3D, conv_polyline);
        CASE(POLYLINE_MESH, conv_polyline); CASE(POLYLINE_PFACE, conv_polyline);
        CASE(TEXT, conv_text); CASE(MTEXT, conv_mtext);
        case DWG_TYPE_INSERT: rec.entity = conv_insert(obj, references); break;
        CASE(POINT, conv_point); CASE(SOLID, conv_solid);
        case DWG_TYPE__3DFACE: rec.entity = conv_solid(obj, dwg); break;
        CASE(SPLINE, conv_spline);
        CASE(DIMENSION_ORDINATE, conv_dimension); CASE(DIMENSION_LINEAR, conv_dimension);
        CASE(DIMENSION_ALIGNED, conv_dimension); CASE(DIMENSION_ANG3PT, conv_dimension);
        CASE(DIMENSION_ANG2LN, conv_dimension); CASE(DIMENSION_RADIUS, conv_dimension);
        CASE(DIMENSION_DIAMETER, conv_dimension);
        CASE(HATCH, conv_hatch);
        default: ok = false;
    }
#undef CASE
    return ok;
}

void append_owned_entities(Dwg_Object* block_header, Dwg_Data* dwg,
                           std::vector<EntityRecord>& destination,
                           const ObjectReferenceIndex& references) {
    if (!block_header || block_header->fixedtype != DWG_TYPE_BLOCK_HEADER ||
        !block_header->tio.object || !block_header->tio.object->tio.BLOCK_HEADER) return;

    auto* header = block_header->tio.object->tio.BLOCK_HEADER;
    const BITCODE_BL max_entities = std::max<BITCODE_BL>(header->num_owned, 1);
    std::set<BITCODE_BL> visited;
    for (Dwg_Object* obj = get_first_owned_entity(block_header);
         obj && static_cast<BITCODE_BL>(visited.size()) <= max_entities;
         obj = get_next_owned_entity(block_header, obj)) {
        if (!visited.insert(obj->index).second) break;
        EntityRecord rec;
        if (convert_entity(obj, dwg, rec, references)) destination.push_back(std::move(rec));
    }
}

bool is_space_block(const std::string& name) {
    return name == "*Model_Space" || name == "*Paper_Space" ||
           name.rfind("*Paper_Space", 0) == 0;
}

} // anonymous namespace

// ---- Public API ----

bool is_available(std::string* out_version) {
    if (out_version)
        *out_version = "libredwg C API " + std::to_string(LIBREDWG_VERSION_MAJOR)
                       + "." + std::to_string(LIBREDWG_VERSION_MINOR);
    return true;
}

Drawing parse_file(const std::string& filepath,
                   const DwgParserOptions& options, DwgParseResult* out_result) {
    Drawing d; d.info.filename = filepath; d.info.format = "DWG"; (void)options;

    { std::ifstream t(filepath, std::ios::binary);
      if (!t.is_open()) { if (out_result) *out_result = DwgParseResult::FileNotFound; return d; } }

    Dwg_Data dwg; memset(&dwg, 0, sizeof(dwg));
    dwg.opts = DWG_OPTS_MINIMAL;
    int error = dwg_read_file(filepath.c_str(), &dwg);

    if (error >= DWG_ERR_CRITICAL) {
        std::cerr << "[DWG API] Critical error " << error << "\n";
        if (out_result) *out_result = DwgParseResult::LibreDwgError;
        dwg_free(&dwg);
        return d;
    }
    if (error) {
        std::cerr << "[DWG API] Non-critical warnings:";
        if (error & 1)  std::cerr << " WRONGCRC";
        if (error & 2)  std::cerr << " NOTYETSUPPORTED";
        if (error & 4)  std::cerr << " UNHANDLEDCLASS";
        if (error & 8)  std::cerr << " INVALIDTYPE";
        if (error & 16) std::cerr << " INVALIDHANDLE";
        if (error & 32) std::cerr << " INVALIDEED";
        if (error & 64) std::cerr << " VALUEOUTOFBOUNDS";
        std::cerr << " (data may be incomplete)\n";
    }

    // Header
    d.info.version = safe_str(dwg_version_type(dwg.header.version));
    d.info.min_x = dwg_model_x_min(&dwg); d.info.max_x = dwg_model_x_max(&dwg);
    d.info.min_y = dwg_model_y_min(&dwg); d.info.max_y = dwg_model_y_max(&dwg);
    const ObjectReferenceIndex references(&dwg);

    // Layers
    for (BITCODE_BL i = 0; i < dwg.num_objects; ++i) {
        Dwg_Object* obj = &dwg.object[i];
        if (!obj || obj->fixedtype != DWG_TYPE_LAYER || !obj->tio.object) continue;
        auto* lv = obj->tio.object->tio.LAYER; if (!lv) continue;
        Layer lay;
        lay.name = utf8_text(lv, "LAYER", "name", lv->name);
        lay.color_index = layer_color_index(lv->color);
        lay.frozen = lv->frozen;
        lay.locked = lv->locked;
        // LibreDWG has called this field both "off" and "on" across releases.
        // A negative layer colour index is the stable API representation of off.
        lay.visible = lv->color.index >= 0;
        if (!lay.name.empty()) d.layers[lay.name] = lay;
    }

    // Collect block definitions from their owned-entity lists. Iterating every
    // entity in Dwg_Data would incorrectly render block definitions at their
    // local coordinates as if they belonged to model space.
    for (BITCODE_BL i = 0; i < dwg.num_objects; ++i) {
        Dwg_Object* obj = &dwg.object[i];
        if (obj->fixedtype != DWG_TYPE_BLOCK_HEADER || !obj->tio.object ||
            !obj->tio.object->tio.BLOCK_HEADER) continue;
        auto* header = obj->tio.object->tio.BLOCK_HEADER;
        BlockRecord block;
        block.name = utf8_text(header, "BLOCK_HEADER", "name", header->name);
        if (block.name.empty() || is_space_block(block.name)) continue;
        block.base_point = to_pt(header->base_pt);
        block.description = safe_str(header->description);
        append_owned_entities(obj, &dwg, block.entities, references);
        if (!block.entities.empty()) d.blocks[block.name] = std::move(block);
    }

    // Model space owns the visible top-level entities. This deliberately
    // excludes block-local geometry, paper space, and dictionary subentities.
    if (Dwg_Object* model_space = dwg_model_space_object(&dwg)) {
        append_owned_entities(model_space, &dwg, d.entities, references);
    } else {
        std::cerr << "[DWG API] Model space unavailable; using flat entity fallback\n";
        for (BITCODE_BL i = 0; i < dwg.num_objects; ++i) {
            EntityRecord rec;
            if (convert_entity(&dwg.object[i], &dwg, rec, references)) d.entities.push_back(std::move(rec));
        }
    }

    d.info.entity_count = d.entities.size();
    d.info.layer_count = d.layers.size(); d.info.block_count = d.blocks.size();
    dwg_free(&dwg);
    if (out_result) *out_result = DwgParseResult::Success;
    return d;
}

DrawingInfo peek_header(const std::string& fp, const DwgParserOptions& opts,
                        DwgParseResult* r) {
    return parse_file(fp, opts, r).info;
}

} // namespace dwg_api
} // namespace cad
