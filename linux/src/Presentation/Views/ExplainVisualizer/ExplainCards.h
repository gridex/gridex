#pragma once
// Small framed widgets used by the Explain Visualizer's loaded state:
// summary metric cards, the warning InfoBar, recommendation cards, and
// timing waterfall rows. Each is a thin QFrame composition — no custom
// paintEvent so they pick up the system palette without extra work.

#include "Presentation/Views/ExplainVisualizer/ExplainPlanTypes.h"
#include "Presentation/Views/ExplainVisualizer/ExplainPlanHeuristics.h"

#include <QFrame>
#include <QString>

class QLabel;
class QPushButton;
class QProgressBar;

namespace gridex::explain {

class MetricCard : public QFrame {
    Q_OBJECT
public:
    MetricCard(const QString& glyph,
               const QString& label,
               const QString& value,
               const QString& unit,
               const QString& delta,
               double barPct,
               ::gridex::explainviz::NodeLevel tone,
               QWidget* parent = nullptr);
};

class InfoBarLite : public QFrame {
    Q_OBJECT
public:
    explicit InfoBarLite(QWidget* parent = nullptr);
    // Show an issue or hide if title is empty.
    void setMessage(const QString& title,
                    const QString& detail,
                    ::gridex::explainviz::IssueSeverity sev);
    void clear();
private:
    QLabel* titleLbl_ = nullptr;
    QLabel* detailLbl_ = nullptr;
};

class RecCard : public QFrame {
    Q_OBJECT
public:
    explicit RecCard(const ::gridex::explainviz::Recommendation& r,
                     QWidget* parent = nullptr);
signals:
    void previewRequested(const QString& sql);
private:
    QString sql_;
};

class WaterfallRow : public QFrame {
    Q_OBJECT
public:
    WaterfallRow(const QString& glyph,
                 const QString& label,
                 double startMs,
                 double durMs,
                 double maxMs,
                 ::gridex::explainviz::NodeLevel level,
                 QWidget* parent = nullptr);
};

}
