#pragma once
// Native Qt Explain Plan Visualizer.
//
// Four states (Empty/Loading/Loaded/Error) cycled via QStackedWidget.
// Host pushes a parsed PlanDocument via setPlan(); the widget owns
// rendering of the metric cards, plan tree, flyout, waterfall, and
// recommendation cards. User actions (Run, Copy plan, Preview SQL) are
// emitted as signals so the host can decide what to do with them.

#include "Presentation/Views/ExplainVisualizer/ExplainPlanTypes.h"
#include "Presentation/Views/ExplainVisualizer/ExplainPlanHeuristics.h"

#include <QWidget>
#include <memory>
#include <vector>

class QFrame;
class QLabel;
class QPushButton;
class QStackedWidget;
class QCheckBox;
class QVBoxLayout;

namespace gridex::explain {

class InfoBarLite;
class PlanNodeRow;

class ExplainVisualizerWidget : public QWidget {
    Q_OBJECT
public:
    explicit ExplainVisualizerWidget(QWidget* parent = nullptr);

    void setPlan(std::shared_ptr<::gridex::explainviz::PlanDocument> doc);
    void showEmpty();
    void showLoading(const QString& text = QString());
    void showError(const QString& message);
    void reset() { showEmpty(); }

signals:
    void runRequested();
    void copyPlanRequested(const QString& rawJson);
    void previewSqlRequested(const QString& sql);

private:
    void buildUi();
    QWidget* buildEmptyState();
    QWidget* buildLoadingState();
    QWidget* buildErrorState();
    QWidget* buildLoadedState();

    void renderInfoBar();
    void renderMetrics();
    void renderPlanTree();
    void renderFlyout();
    void renderWaterfall();
    void renderRecs();

    void selectNode(const QString& id);
    void clearLayout(QVBoxLayout* lay);

    void buildNodeRowRecursive(
        const std::shared_ptr<::gridex::explainviz::PlanNode>& node,
        int depth);

    // State.
    QStackedWidget* stack_ = nullptr;
    QLabel* loadingLbl_ = nullptr;
    QLabel* errorLbl_ = nullptr;

    // Loaded-state widgets.
    QLabel* metaLbl_ = nullptr;
    InfoBarLite* infoBar_ = nullptr;
    QFrame* metricsCard_ = nullptr;
    QVBoxLayout* metricsLay_ = nullptr;
    QFrame* treeCard_ = nullptr;
    QVBoxLayout* treeLay_ = nullptr;
    QFrame* flyoutCard_ = nullptr;
    QVBoxLayout* flyoutLay_ = nullptr;
    QVBoxLayout* waterfallLay_ = nullptr;
    QVBoxLayout* recsLay_ = nullptr;
    QCheckBox* hotOnlyChk_ = nullptr;
    QLabel* planCountLbl_ = nullptr;

    // Doc + derived.
    std::shared_ptr<::gridex::explainviz::PlanDocument> doc_;
    std::vector<::gridex::explainviz::Issue> issues_;
    std::vector<::gridex::explainviz::Recommendation> recs_;
    QString selectedNodeId_;
    std::vector<PlanNodeRow*> treeRows_;
};

}
