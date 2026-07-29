#include "CadViewer.h"
#include <QGraphicsPathItem>
#include <QGraphicsEllipseItem>
#include <QGraphicsTextItem>
#include <QGraphicsPolygonItem>
#include <QFontMetricsF>
#include <QFileInfo>
#include <QPen>
#include <QBrush>
#include <QPainterPath>
#include <QTextDocument>
#include <QTextCodec>
#include <QRegularExpression>
#include <QTimer>
#include <QImage>
#include <QPainter>
#include <sstream>
#include <iomanip>

// =============================================================================
// Text helpers
// =============================================================================

// Convert CAD text (potentially GBK-encoded) to UTF-8 for Qt display.
static QString decodeCadText(const std::string& raw) {
    if (raw.empty()) return QString();
    QString result = QString::fromUtf8(raw.c_str());
    if (result.contains(QChar(0xFFFD))) {
        QTextCodec* gbk = QTextCodec::codecForName("GBK");
        if (gbk) result = gbk->toUnicode(raw.c_str());
        else     result = QString::fromLocal8Bit(raw.c_str());
    }
    return result;
}

// Strip AutoCAD MTEXT formatting codes.
static QString cleanMText(const QString& raw) {
    QString s = raw;
    s.remove(QRegularExpression(R"(\{\\f[^;]*;|\\f[^;]*;)"));
    s.remove(QRegularExpression(R"(\\C\d+;)"));
    s.remove(QRegularExpression(R"(\\[LH])"));
    s.replace("\\P", "\n").replace("\\p", "\n");
    s.remove('{').remove('}');
    return s.trimmed();
}

// =============================================================================
// ACI color table & logic — matches routedesigner's CAD_COLOR[] + getAttributesColor
// =============================================================================

static const QRgb kAciTable[256] = {
    0x000000,0xFF0000,0xFFFF00,0x00FF00,0x00FFFF,0x0000FF,0xFF00FF,0x000000,
    0x404040,0xC0C0C0,0xFF0000,0xFFAAAA,0xBD0000,0xBD7E7E,0x810000,0x815656,
    0x680000,0x684545,0x4F0000,0x4F3535,0xFF3F00,0xFFBFAA,0xBD2E00,0xBD8B7E,
    0x811F00,0x815F56,0x681900,0x684C45,0x4F1300,0x4F3B35,0xFF7F00,0xFFD4AA,
    0xBD5E00,0xBD9C7E,0x814000,0x816B56,0x683400,0x685645,0x4F2700,0x4F4235,
    0xFFBF00,0xFFEAAA,0xBD8D00,0xBDAC7E,0x816000,0x817656,0x684E00,0x685F45,
    0x4F3B00,0x4F4935,0xFFFF00,0xFFFFAA,0xBDBD00,0xBDBD7E,0x818100,0x818156,
    0x686800,0x686845,0x4F4F00,0x4F4F35,0xBFFF00,0xEAFFAA,0x8DBD00,0xACBD7E,
    0x608100,0x768156,0x4E6800,0x5F6845,0x3B4F00,0x494F35,0x7FFF00,0xD4FFAA,
    0x5EBD00,0x9CBD7E,0x408100,0x6B8156,0x346800,0x566845,0x274F00,0x424F35,
    0x3FFF00,0xBFFFAA,0x2EBD00,0x8BBD7E,0x1F8100,0x5F8156,0x196800,0x4C6845,
    0x134F00,0x3B4F35,0x00FF00,0xAAFFAA,0x00BD00,0x7EBD7E,0x008100,0x568156,
    0x006800,0x456845,0x004F00,0x354F35,0x00FF3F,0xAAFFBF,0x00BD2E,0x7EBD8B,
    0x00811F,0x56815F,0x006819,0x45684C,0x004F13,0x354F3B,0x00FF7F,0xAAFFD4,
    0x00BD5E,0x7EBD9C,0x008140,0x56816B,0x006834,0x456856,0x004F27,0x354F42,
    0x00FFBF,0xAAFFEA,0x00BD8D,0x7EBDAC,0x008160,0x568176,0x00684E,0x45685F,
    0x004F3B,0x354F49,0x00FFFF,0xAAFFFF,0x00BDBD,0x7EBDBD,0x008181,0x568181,
    0x006868,0x456868,0x004F4F,0x354F4F,0x00BFFF,0xAAEAFF,0x008DBD,0x7EACBD,
    0x006081,0x567681,0x004E68,0x455F68,0x003B4F,0x35494F,0x007FFF,0xAAD4FF,
    0x005EBD,0x7E9CBD,0x004081,0x566B81,0x003468,0x455668,0x00274F,0x35424F,
    0x003FFF,0xAABFFF,0x002EBD,0x7E8BBD,0x001F81,0x565F81,0x001968,0x454C68,
    0x00134F,0x353B4F,0x0000FF,0xAAAAFF,0x0000BD,0x7E7EBD,0x000081,0x565681,
    0x000068,0x454568,0x00004F,0x35354F,0x3F00FF,0xBFAAFF,0x2E00BD,0x8B7EBD,
    0x1F0081,0x5F5681,0x190068,0x4C4568,0x13004F,0x3B354F,0x7F00FF,0xD4AAFF,
    0x5E00BD,0x9C7EBD,0x400081,0x6B5681,0x340068,0x564568,0x27004F,0x42354F,
    0xBF00FF,0xEAAAFF,0x8D00BD,0xAC7EBD,0x600081,0x765681,0x4E0068,0x5F4568,
    0x3B004F,0x494F35,0xFF00FF,0xFFAAFF,0xBD00BD,0xBD7EBD,0x810081,0x815681,
    0x680068,0x684568,0x4F004F,0x4F354F,0xFF00BF,0xFFAAEA,0xBD008D,0xBD7EAC,
    0x810060,0x815676,0x68004E,0x68455F,0x4F003B,0x4F3549,0xFF007F,0xFFAAD4,
    0xBD005E,0xBD7E9C,0x810040,0x81566B,0x680034,0x684556,0x4F0027,0x4F3542,
    0xFF003F,0xFFAABF,0xBD002E,0xBD7E8B,0x81001F,0x81565F,0x680019,0x68454C,
    0x4F0013,0x4F353B,0x333333,0x505050,0x696969,0x828282,0xBEBEBE,0xFFFFFF,
};

// ACI→QColor, matching routedesigner getAttributesColor():
// 256=BYLAYER→use layer color_index, 0=BYBLOCK→black, 1-255=ACI table
static QColor aciToColor(int index, int layerColorIndex = 7) {
    if (index == 256) index = layerColorIndex;
    if (index == 0 || index < 0 || index >= 256) return QColor(Qt::black);
    return QColor::fromRgb(kAciTable[index]);
}

QColor CadViewerWindow::layerColor(const std::string& name, int) {
    auto it = m_layerColors.find(name);
    if (it != m_layerColors.end()) return it->second;
    auto lit = m_drawing.layers.find(name);
    QColor c = aciToColor(lit != m_drawing.layers.end() ? lit->second.color_index : 7);
    m_layerColors[name] = c;
    return c;
}

static QColor recordColor(CadViewerWindow* viewer, const cad::EntityRecord& rec,
                          const std::string& layer, const QColor& byBlockColor) {
    if (rec.color.index == 0) return byBlockColor;
    int layerCi = 7;
    auto it = viewer->m_drawing.layers.find(layer);
    if (it != viewer->m_drawing.layers.end()) layerCi = it->second.color_index;
    if (rec.color.index > 0 && rec.color.index < 256)
        return aciToColor(rec.color.index, layerCi);
    return viewer->layerColor(layer, layerCi);
}

// =============================================================================
// Block expansion helpers
// =============================================================================
static QTransform makeXform(double tx, double ty, double sx, double sy, double rot,
                            const cad::Point3D& base = {}) {
    QTransform t;
    t.translate(tx, -ty);
    t.rotate(-rot);
    t.scale(sx, sy);
    t.translate(-base.x, base.y);
    return t;
}
static QPointF xf(const QTransform& t, double x, double y) { return t.map(QPointF(x, -y)); }

static void appendPolylineSegment(QPainterPath& path, double x1, double y1,
                                  double x2, double y2, double bulge) {
    if (std::abs(bulge) < 1e-12) {
        path.lineTo(x2, -y2);
        return;
    }
    const double dx = x2 - x1, dy = y2 - y1;
    const double factor = (1.0 - bulge * bulge) / (4.0 * bulge);
    const double cx = (x1 + x2) * 0.5 - dy * factor;
    const double cy = (y1 + y2) * 0.5 + dx * factor;
    const double radius = std::hypot(x1 - cx, y1 - cy);
    const double start = std::atan2(y1 - cy, x1 - cx) * 180.0 / cad::kPi;
    const double span = -4.0 * std::atan(bulge) * 180.0 / cad::kPi;
    path.arcTo(cx - radius, -cy - radius, radius * 2.0, radius * 2.0,
               start, span);
}

template<typename Vertices>
static QPainterPath polylinePath(const Vertices& vertices, bool closed) {
    QPainterPath path;
    if (vertices.empty()) return path;
    path.moveTo(vertices[0].position.x, -vertices[0].position.y);
    const size_t segments = closed ? vertices.size() : vertices.size() - 1;
    for (size_t i = 0; i < segments; ++i) {
        const auto& from = vertices[i];
        const auto& to = vertices[(i + 1) % vertices.size()];
        appendPolylineSegment(path, from.position.x, from.position.y,
                              to.position.x, to.position.y, from.bulge);
    }
    return path;
}

static void renderBlock(QGraphicsScene* scene, const cad::BlockRecord& blk,
                        const QTransform& xform, CadViewerWindow* viewer,
                        const std::string& inheritedLayer,
                        const QColor& inheritedColor,
                        std::set<std::string>& activeBlocks) {
    for (auto& rec : blk.entities) {
        const std::string& layer = rec.layer_name == "0" ? inheritedLayer : rec.layer_name;
        if (!viewer->m_visibleLayers.empty() && !viewer->m_visibleLayers.count(layer)) continue;
        QColor c = recordColor(viewer, rec, layer, inheritedColor);
        QPen pen(c); pen.setWidth(0); pen.setCosmetic(true);
        QBrush brush(Qt::NoBrush);
        std::visit([&](auto&& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, cad::LineEntity>) {
                scene->addLine(QLineF(xf(xform,e.start.x,e.start.y), xf(xform,e.end.x,e.end.y)), pen);
            } else if constexpr (std::is_same_v<T, cad::CircleEntity>) {
                QPainterPath path;
                path.addEllipse(e.center.x-e.radius, -e.center.y-e.radius,
                                e.radius*2, e.radius*2);
                scene->addPath(xform.map(path), pen, brush);
            } else if constexpr (std::is_same_v<T, cad::ArcEntity>) {
                double span=e.end_angle-e.start_angle; if(span<0)span+=360;
                QPainterPath path;
                path.arcMoveTo(e.center.x-e.radius,-e.center.y-e.radius,
                               e.radius*2,e.radius*2,e.start_angle);
                path.arcTo(e.center.x-e.radius,-e.center.y-e.radius,
                           e.radius*2,e.radius*2,e.start_angle,-span);
                scene->addPath(xform.map(path), pen);
            } else if constexpr (std::is_same_v<T, cad::LwPolylineEntity>) {
                scene->addPath(xform.map(polylinePath(e.vertices, e.closed)), pen);
            } else if constexpr (std::is_same_v<T, cad::PolylineEntity>) {
                scene->addPath(xform.map(polylinePath(e.vertices, e.closed)), pen);
            } else if constexpr (std::is_same_v<T, cad::SolidEntity>) {
                QPolygonF polygon;
                polygon << xf(xform, e.p1.x, e.p1.y) << xf(xform, e.p2.x, e.p2.y)
                        << xf(xform, e.p3.x, e.p3.y) << xf(xform, e.p4.x, e.p4.y);
                scene->addPolygon(polygon, pen, QBrush(QColor(c.red(), c.green(), c.blue(), 40)));
            } else if constexpr (std::is_same_v<T, cad::InsertEntity>) {
                auto nested = viewer->m_drawing.blocks.find(e.block_name);
                if (nested == viewer->m_drawing.blocks.end() ||
                    nested->second.entities.empty() || !activeBlocks.insert(e.block_name).second) {
                    return;
                }
                const QTransform child = makeXform(e.insertion_point.x, e.insertion_point.y,
                                                    e.scale_x, e.scale_y, e.rotation,
                                                    nested->second.base_point) * xform;
                renderBlock(scene, nested->second, child, viewer, layer, c, activeBlocks);
                activeBlocks.erase(e.block_name);
            }
        }, rec.entity);
    }
}

// =============================================================================
// Entity visitor
// =============================================================================
struct EntityPainter {
    QGraphicsScene* scene;
    CadViewerWindow* viewer;
    QColor color;
    std::string currentLayer = "0";
    QPen pen;
    QBrush brush;

    EntityPainter(QGraphicsScene* s, CadViewerWindow* v) : scene(s), viewer(v) {
        pen.setWidth(0); pen.setCosmetic(true); brush.setStyle(Qt::NoBrush);
    }
    void setColor(const QColor& c) { color = c; pen.setColor(c); }
    bool isVisible(const std::string& l) const {
        auto& vl = viewer->m_visibleLayers;
        return vl.empty() || vl.count(l) || vl.count("*");
    }
    void operator()(const cad::LineEntity& e)       { scene->addLine(e.start.x,-e.start.y,e.end.x,-e.end.y,pen); }
    void operator()(const cad::CircleEntity& e)     { auto r=e.radius; scene->addEllipse(e.center.x-r,-e.center.y-r,r*2,r*2,pen,brush); }
    void operator()(const cad::ArcEntity& e) {
        double span=e.end_angle-e.start_angle; if(span<0)span+=360;
        QPainterPath p; double cx=e.center.x,cy=-e.center.y,r=e.radius;
        p.arcMoveTo(cx-r,cy-r,r*2,r*2,e.start_angle); p.arcTo(cx-r,cy-r,r*2,r*2,e.start_angle,-span);
        scene->addPath(p,pen);
    }
    void operator()(const cad::EllipseEntity& e) {
        double rx=std::hypot(e.major_axis_endpoint.x,e.major_axis_endpoint.y),ry=rx*e.axis_ratio;
        double ang=std::atan2(e.major_axis_endpoint.y,e.major_axis_endpoint.x)*180.0/cad::kPi;
        auto* it=scene->addEllipse(e.center.x-rx,-e.center.y-ry,rx*2,ry*2,pen,brush);
        it->setTransform(QTransform().translate(e.center.x,-e.center.y).rotate(-ang).translate(-e.center.x,e.center.y));
    }
    void operator()(const cad::LwPolylineEntity& e) {
        scene->addPath(polylinePath(e.vertices, e.closed),pen);
    }
    void operator()(const cad::PolylineEntity& e) {
        scene->addPath(polylinePath(e.vertices, e.closed),pen);
    }
    void operator()(const cad::TextEntity& e) {
        QString txt=cleanMText(decodeCadText(e.value)); if(txt.isEmpty())return;
        auto* it=scene->addText(txt);
        it->document()->setDocumentMargin(0.0);
        QFont f("Noto Sans CJK SC,WenQuanYi,SimHei,sans-serif");
        f.setPixelSize(std::max(1, static_cast<int>(std::lround(e.height))));
        it->setFont(f); it->setRotation(-e.rotation); it->setDefaultTextColor(color);

        const QFontMetricsF metrics(f);
        const QRectF bounds = it->boundingRect();
        const bool is_two_point_width = e.horizontal_alignment == 3 ||
                                        e.horizontal_alignment == 5;
        const cad::Point3D& anchor = e.has_alignment_point && !is_two_point_width
            ? e.alignment_point : e.insertion_point;
        qreal x = anchor.x;
        qreal y = -anchor.y;
        if (e.horizontal_alignment == 1 || e.horizontal_alignment == 4) x -= bounds.width() / 2.0;
        else if (e.horizontal_alignment == 2) x -= bounds.width();

        // CAD's default TEXT insertion point is its baseline, while Qt positions
        // a QGraphicsTextItem by its top-left corner.
        if (e.vertical_alignment == 0) y -= metrics.ascent();
        else if (e.vertical_alignment == 1) y -= bounds.height();
        else if (e.vertical_alignment == 2) y -= bounds.height() / 2.0;
        it->setPos(x, y);
    }
    void operator()(const cad::MTextEntity& e) {
        QString txt=cleanMText(decodeCadText(e.value)); if(txt.isEmpty())return;
        auto* it=scene->addText(txt); it->setPos(e.insertion_point.x,-e.insertion_point.y);
        it->setDefaultTextColor(color);
        QFont f("Noto Sans CJK SC,WenQuanYi,SimHei,sans-serif");
        f.setPixelSize(std::max(1, static_cast<int>(std::lround(e.height)))); it->setFont(f);
    }
    void operator()(const cad::InsertEntity& e) {
        if (e.block_name.empty()) return;
        auto it = viewer->m_drawing.blocks.find(e.block_name);
        if (it != viewer->m_drawing.blocks.end() && !it->second.entities.empty()) {
            QTransform xf = makeXform(e.insertion_point.x,e.insertion_point.y,
                                      e.scale_x,e.scale_y,e.rotation,it->second.base_point);
            std::set<std::string> activeBlocks{e.block_name};
            renderBlock(scene, it->second, xf, viewer, currentLayer, color, activeBlocks);
        }
        // INSERTs without block geometry are skipped (no dot clutter)
    }
    void operator()(const cad::PointEntity& e) {
        scene->addEllipse(e.position.x-2,-e.position.y-2,4,4,QPen(color),QBrush(color));
    }
    void operator()(const cad::SolidEntity& e) {
        QPolygonF poly; poly<<QPointF(e.p1.x,-e.p1.y)<<QPointF(e.p2.x,-e.p2.y)<<QPointF(e.p3.x,-e.p3.y)<<QPointF(e.p4.x,-e.p4.y);
        scene->addPolygon(poly,pen,QBrush(QColor(color.red(),color.green(),color.blue(),40)));
    }
    void operator()(const cad::SplineEntity& e) {
        if (e.control_points.size() < 2) return;
        QPainterPath p;
        p.moveTo(e.control_points[0].x, -e.control_points[0].y);
        for(size_t i=1;i<e.control_points.size();++i)p.lineTo(e.control_points[i].x,-e.control_points[i].y);
        QPen dp(pen); dp.setStyle(Qt::DashLine); scene->addPath(p,dp);
    }
    void operator()(const cad::DimensionEntity& e) {
        QPen dp(QColor(150,150,150)); dp.setWidth(0); dp.setCosmetic(true);
        scene->addLine(e.definition_point.x,-e.definition_point.y,e.text_midpoint.x,-e.text_midpoint.y,dp);
    }
    void operator()(const cad::HatchEntity& e) {
        for(auto& loop:e.boundary_loops){if(loop.size()<2)continue; QPolygonF poly; for(auto& p:loop)poly<<QPointF(p.x,-p.y);
        scene->addPolygon(poly,pen,QBrush(QColor(color.red(),color.green(),color.blue(),40))); break;}
    }
};

// =============================================================================
// CadViewerWindow
// =============================================================================
CadViewerWindow::CadViewerWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("CAD Viewer"); resize(1400,900);
    m_scene = new QGraphicsScene(this); m_view = new CadGraphicsView(this);
    m_view->setScene(m_scene); setCentralWidget(m_view);

    m_infoLabel = new QLabel("Ready"); m_coordLabel = new QLabel("x: --  y: --");
    statusBar()->addWidget(m_infoLabel,1); statusBar()->addPermanentWidget(m_coordLabel);

    QToolBar* tb = addToolBar("Main");
    tb->addAction("📂 Open", this, &CadViewerWindow::onOpen); tb->addSeparator();
    tb->addAction("⊞ Fit",  this, &CadViewerWindow::onFit);
    tb->addAction("1:1",    this, &CadViewerWindow::onReset); tb->addSeparator();

    QDockWidget* ld = new QDockWidget("Layers", this);
    m_layerList = new QListWidget; m_layerList->setSelectionMode(QAbstractItemView::NoSelection);
    connect(m_layerList, &QListWidget::itemChanged, this, &CadViewerWindow::onLayerToggled);
    ld->setWidget(m_layerList); addDockWidget(Qt::RightDockWidgetArea, ld);

    auto* hint = m_scene->addText("📂 Open .dxf/.dwg  |  🖱 Scroll=Zoom  |  Drag=Pan");
    hint->setDefaultTextColor(QColor(180,180,180)); hint->setPos(0,0);
}

void CadViewerWindow::buildLayerPanel() {
    m_layerList->clear();
    for (auto& [name, layer] : m_drawing.layers) {
        auto* item = new QListWidgetItem(QString::fromStdString(name));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Checked);
        item->setForeground(layerColor(name, layer.color_index));
        m_layerList->addItem(item); m_visibleLayers.insert(name);
    }
}

void CadViewerWindow::onLayerToggled(QListWidgetItem* item) {
    std::string name = item->text().toStdString();
    if (item->checkState() == Qt::Checked) m_visibleLayers.insert(name);
    else m_visibleLayers.erase(name);
    buildScene();
}

void CadViewerWindow::buildScene() {
    m_scene->clear();
    m_scene->setBackgroundBrush(QBrush(QColor(250, 250, 250)));  // light gray, matches routedesigner

    if (m_drawing.entities.empty()) return;
    EntityPainter painter(m_scene, this);

    for (auto& rec : m_drawing.entities) {
        if (!painter.isVisible(rec.layer_name)) continue;
        // Use entity's own color with BYLAYER/BYBLOCK logic, matching routedesigner
        int layerCi = 7;
        auto lit = m_drawing.layers.find(rec.layer_name);
        if (lit != m_drawing.layers.end()) layerCi = lit->second.color_index;
        painter.currentLayer = rec.layer_name;
        painter.setColor(aciToColor(rec.color.index, layerCi));
        std::visit(painter, rec.entity);
    }
    QTimer::singleShot(50, this, &CadViewerWindow::onFit);
}

void CadViewerWindow::onFit() {
    m_view->fitInView(m_scene->itemsBoundingRect(), Qt::KeepAspectRatio);
    if (m_view->transform().m11() > 10) { m_view->resetTransform(); m_view->scale(10,10); }
}
void CadViewerWindow::onReset() { m_view->resetTransform(); }
void CadViewerWindow::onOpen() {
    QString path = QFileDialog::getOpenFileName(this, "Open CAD File", "",
        "CAD Files (*.dxf *.dwg *.DXF *.DWG);;All Files (*)");
    if (!path.isEmpty()) loadFile(path);
}

bool CadViewerWindow::loadFile(const QString& path) {
    m_filepath = path; m_scene->clear(); m_layerColors.clear(); m_visibleLayers.clear();
    m_infoLabel->setText("Parsing: " + path + " ..."); QApplication::processEvents();

    std::string ext = path.toStdString();
    auto dot = ext.rfind('.');
    if (dot != std::string::npos) { ext = ext.substr(dot); for(auto& c:ext) c=std::tolower(c); }

    if (ext == ".dwg") {
        cad::DwgParseResult r; m_drawing = cad::parse_dwg_file(path.toStdString(), {}, &r);
        if (r != cad::DwgParseResult::Success) {
            QMessageBox::warning(this,"Error",QString("DWG parse failed: ")+cad::to_string(r)); return false;
        }
    } else {
        cad::ParseResult r; m_drawing = cad::parse_dxf_file(path.toStdString(), {}, &r);
        if (r != cad::ParseResult::Success) {
            QMessageBox::warning(this,"Error",QString("DXF parse failed: ")+cad::to_string(r)); return false;
        }
    }

    std::ostringstream ss;
    ss << "File: " << m_drawing.info.filename
       << " | Entities: " << m_drawing.info.entity_count
       << " | Layers: " << m_drawing.info.layer_count
       << " | Blocks: " << m_drawing.info.block_count;
    m_infoLabel->setText(QString::fromStdString(ss.str()));
    buildLayerPanel(); buildScene();
    setWindowTitle(QString("CAD Viewer — %1").arg(QFileInfo(path).fileName()));
    return true;
}

bool CadViewerWindow::renderToImage(const QString& path, const QSize& size) {
    if (size.width() <= 0 || size.height() <= 0 || m_scene->items().empty()) return false;

    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(250, 250, 250));

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF bounds = m_scene->itemsBoundingRect();
    if (bounds.isEmpty()) return false;
    const QRectF target = QRectF(image.rect()).adjusted(24, 24, -24, -24);
    m_scene->render(&painter, target, bounds, Qt::KeepAspectRatio);
    painter.end();
    return image.save(path);
}
