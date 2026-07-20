#pragma once
#include <QGraphicsView>
#include <QGraphicsScene>
#include <QWheelEvent>
#include <QLabel>
#include <QMainWindow>
#include <QListWidget>
#include <QDockWidget>
#include <QFileDialog>
#include <QStatusBar>
#include <QToolBar>
#include <QMessageBox>
#include <QSplitter>
#include <QApplication>
#include <QSize>
#include <cmath>
#include <map>
#include <set>
#include "cad_parser/cad_parser.h"

class CadGraphicsView : public QGraphicsView {
    Q_OBJECT
public:
    CadGraphicsView(QWidget* parent = nullptr) : QGraphicsView(parent) {
        setRenderHint(QPainter::Antialiasing);
        setDragMode(QGraphicsView::ScrollHandDrag);
        setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
        setResizeAnchor(QGraphicsView::AnchorUnderMouse);
        setViewportUpdateMode(QGraphicsView::SmartViewportUpdate);
        setOptimizationFlags(QGraphicsView::DontAdjustForAntialiasing);
        setBackgroundBrush(QBrush(QColor(250, 250, 250)));
        setFrameShape(QFrame::NoFrame);
    }
protected:
    void wheelEvent(QWheelEvent* e) override {
        double factor = (e->delta() > 0) ? 1.15 : 1.0 / 1.15;
        scale(factor, factor);
    }
};

class CadViewerWindow : public QMainWindow {
    Q_OBJECT
public:
    CadViewerWindow(QWidget* parent = nullptr);
    ~CadViewerWindow() = default;

    bool loadFile(const QString& path);
    bool renderToImage(const QString& path, const QSize& size);
    QColor layerColor(const std::string& name, int colorIndex);

    // Public data for entity visitors
    CadGraphicsView*    m_view;
    QGraphicsScene*     m_scene;
    QListWidget*        m_layerList;
    QLabel*             m_infoLabel;
    QLabel*             m_coordLabel;
    QString             m_filepath;
    cad::Drawing        m_drawing;
    std::map<std::string, QColor> m_layerColors;
    std::set<std::string> m_visibleLayers;

private slots:
    void onOpen();
    void onFit();
    void onReset();
    void onLayerToggled(QListWidgetItem* item);

private:
    void buildScene();
    void buildLayerPanel();
};
