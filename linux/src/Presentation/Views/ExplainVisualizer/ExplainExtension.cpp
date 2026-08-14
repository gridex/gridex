#include "Presentation/Views/ExplainVisualizer/ExplainExtension.h"

#include "Core/Enums/DatabaseType.h"
#include "Core/Models/Database/RowValue.h"
#include "Core/Models/Query/QueryResult.h"
#include "Core/Protocols/Database/IDatabaseAdapter.h"
#include "Presentation/Views/QueryEditor/QueryEditorView.h"

#include "Presentation/Views/ExplainVisualizer/ExplainPlanParser.h"
#include "Presentation/Views/ExplainVisualizer/ExplainVisualizerWidget.h"

#include <QClipboard>
#include <QDialog>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QPushButton>
#include <QString>
#include <QVBoxLayout>

#include <atomic>
#include <memory>

namespace gridex::explain {

namespace {

void runExplain(::gridex::QueryEditorView* editor, ExplainVisualizerWidget* w);

// One reusable dialog per editor. Created lazily on first click and
// stored via QObject's dynamic-property bag (cast through QObject* so we
// don't need Q_DECLARE_METATYPE(QDialog*) anywhere).
QDialog* getOrCreateDialog(::gridex::QueryEditorView* editor,
                            ExplainVisualizerWidget*& outWidget) {
    static const char* kProp = "gridex.explain.dialog";
    if (auto* obj = editor->property(kProp).value<QObject*>()) {
        if (auto* existing = qobject_cast<QDialog*>(obj)) {
            outWidget = existing->findChild<ExplainVisualizerWidget*>();
            return existing;
        }
    }
    auto* dlg = new QDialog(editor->window());
    dlg->setWindowTitle(QObject::tr("Explain plan"));
    dlg->resize(1100, 800);
    auto* lay = new QVBoxLayout(dlg);
    lay->setContentsMargins(0, 0, 0, 0);
    auto* w = new ExplainVisualizerWidget(dlg);
    lay->addWidget(w);
    outWidget = w;
    editor->setProperty(kProp, QVariant::fromValue<QObject*>(dlg));
    QObject::connect(w, &ExplainVisualizerWidget::copyPlanRequested,
                     [](const QString& json) {
                         QGuiApplication::clipboard()->setText(json);
                     });
    QObject::connect(w, &ExplainVisualizerWidget::previewSqlRequested,
                     editor, [editor](const QString& sql) {
                         editor->setSql(sql);
                     });
    QObject::connect(w, &ExplainVisualizerWidget::runRequested,
                     editor, [editor, w]() {
                         runExplain(editor, w);
                     });
    return dlg;
}

// Run EXPLAIN against the active adapter and push to the widget. PG-only;
// for any other engine we surface a friendly message.
void runExplain(::gridex::QueryEditorView* editor, ExplainVisualizerWidget* w) {
    auto* adapter = editor->adapter();
    if (!adapter) {
        w->showError(QObject::tr("Connect to a Postgres database first."));
        return;
    }
    if (adapter->databaseType() != ::gridex::DatabaseType::PostgreSQL) {
        w->showError(QObject::tr("Explain Visualizer currently supports PostgreSQL only."));
        return;
    }
    const QString sql = editor->currentSql().trimmed();
    if (sql.isEmpty()) {
        w->showError(QObject::tr("Editor is empty — type a query first."));
        return;
    }
    w->showLoading();

    // Synchronous execution — runs on the UI thread for now to mirror the
    // existing Run button path. A future improvement: move to a worker
    // via QtConcurrent so the spinner stays animated.
    std::string wrapped = "EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON) ";
    // Strip trailing semicolon: PG dislikes "EXPLAIN ... SELECT 1;" in
    // some client paths (treated as two statements by the protocol).
    QString clean = sql;
    while (clean.endsWith(';')) clean.chop(1);
    wrapped += clean.toStdString();
    try {
        auto result = adapter->executeRaw(wrapped);
        if (result.rows.empty() || result.rows.front().empty()) {
            w->showError(QObject::tr("EXPLAIN returned no rows."));
            return;
        }
        // PG returns the plan in column 0 of the single row. The PG
        // adapter may surface it as either a plain string or our Json
        // RowValue variant — accept both.
        const auto& cell = result.rows.front().front();
        std::string jsonUtf8;
        if (cell.isString())      jsonUtf8 = cell.asString();
        else if (cell.isJson())   jsonUtf8 = cell.asJson();
        else {
            w->showError(QObject::tr("EXPLAIN returned unexpected cell type."));
            return;
        }
        // UTF-8 → wstring. ASCII bytes (the only kind in EXPLAIN JSON
        // for typical workloads) map 1:1. Non-ASCII relation names would
        // need a proper UTF-8 → UTF-32 decode; deferred until reported.
        std::wstring jsonW = QString::fromUtf8(jsonUtf8).toStdWString();

        auto doc = std::make_shared<::gridex::explainviz::PlanDocument>();
        doc->originalSql = sql.toStdWString();
        std::wstring err;
        if (!::gridex::explainviz::ExplainPlanParser::parse(jsonW, *doc, err)) {
            w->showError(QObject::tr("Parse failed: %1")
                          .arg(QString::fromWCharArray(err.data(), int(err.size()))));
            return;
        }
        w->setPlan(doc);
    } catch (const std::exception& e) {
        w->showError(QString::fromUtf8(e.what()));
    } catch (...) {
        w->showError(QObject::tr("EXPLAIN failed with an unknown error."));
    }
}

} // namespace

void registerQueryEditorExtension() {
    static std::atomic<bool> registered{false};
    bool expected = false;
    if (!registered.compare_exchange_strong(expected, true)) return;

    ::gridex::QueryEditorView::registerExtension(
        [](::gridex::QueryEditorView* editor) {
            auto* btn = new QPushButton(QObject::tr("📊 Explain"), editor);
            btn->setToolTip(QObject::tr("Run EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON) and visualize the plan"));
            QObject::connect(btn, &QPushButton::clicked, editor, [editor]() {
                ExplainVisualizerWidget* w = nullptr;
                auto* dlg = getOrCreateDialog(editor, w);
                if (!w) return;
                runExplain(editor, w);
                if (!dlg->isVisible()) dlg->show();
                dlg->raise();
                dlg->activateWindow();
            });
            editor->addToolbarWidget(btn);
        });
}

} // namespace gridex::explain
