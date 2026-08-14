#pragma once
// A single row in the plan tree. Mirror of the Windows BuildNodeRow:
//   [glyph badge] [op + relation(mono) + pill | detail(elided) | cost(mono)] [%time + heat bar 140px]

#include "Presentation/Views/ExplainVisualizer/ExplainPlanTypes.h"

#include <QFrame>
#include <memory>

namespace gridex::explain {

class HeatBar : public QFrame {
    Q_OBJECT
public:
    HeatBar(::gridex::explainviz::NodeLevel level,
            double pctFill,
            QWidget* parent = nullptr);
protected:
    void paintEvent(QPaintEvent*) override;
private:
    ::gridex::explainviz::NodeLevel level_;
    double pct_; // 0..100
};

class PlanNodeRow : public QFrame {
    Q_OBJECT
public:
    PlanNodeRow(std::shared_ptr<::gridex::explainviz::PlanNode> node,
                int depth,
                QWidget* parent = nullptr);

    const QString& nodeId() const { return nodeId_; }
    void setSelected(bool on);

signals:
    void clicked(const QString& nodeId);

protected:
    void mousePressEvent(QMouseEvent* e) override;
    void enterEvent(QEnterEvent* e) override;
    void leaveEvent(QEvent* e) override;

private:
    QString nodeId_;
    bool selected_ = false;
    bool hovered_ = false;
    void applyStyle();
    ::gridex::explainviz::NodeLevel level_;
};

}
