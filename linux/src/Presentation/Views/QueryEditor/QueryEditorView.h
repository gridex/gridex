#pragma once

#include <QWidget>
#include <functional>
#include <memory>

class QHBoxLayout;
class QLabel;
class QMenu;
class QPlainTextEdit;
class QPushButton;
class QSplitter;
class QTableView;
class QTimer;

namespace gridex {

class IDatabaseAdapter;
class QueryResultModel;
class SqlHighlighter;
class AutocompleteProvider;
class CompletionPopup;
class SqlContextParser;
struct CompletionItem;

// Split view: SQL editor on top + result grid on bottom.
// Ctrl+Enter / Run button executes the current text via adapter.executeRaw.
class QueryEditorView : public QWidget {
    Q_OBJECT

public:
    explicit QueryEditorView(QWidget* parent = nullptr);
    ~QueryEditorView() override;

    void setAdapter(IDatabaseAdapter* adapter);
    void setSql(const QString& sql);

    // Read-only accessors used by toolbar extensions registered via
    // registerExtension() — kept simple, no caching.
    [[nodiscard]] IDatabaseAdapter* adapter() const noexcept { return adapter_; }
    [[nodiscard]] QString currentSql() const;

    // Append a widget (typically a QPushButton) to the toolbar. Inserts
    // just before the trailing Save button so the layout order stays
    // consistent regardless of how many extensions are installed.
    void addToolbarWidget(QWidget* w);

    // Process-wide extension hook. Any callback registered here is
    // invoked exactly once per QueryEditorView instance, after buildUi(),
    // so extensions can attach toolbar widgets / signals without OSS
    // knowing the extension exists.
    using ExtensionFactory = std::function<void(QueryEditorView*)>;
    static void registerExtension(ExtensionFactory f);

public slots:
    // External triggers (toolbar Run / Export) — same code paths as the
    // editor's own internal run button.
    void onRunClicked();
    void exportResultAsCsv();
    void exportResultAsSql();
    void exportResultAsJson();

signals:
    void queryExecuted(const QString& sql, int rowCount, int durationMs);
    void saveQueryRequested(const QString& sql);

private slots:
    void maybeShowCompletions();

private:
    void buildUi();
    bool eventFilter(QObject* obj, QEvent* event) override;

    // Autocomplete helpers
    void triggerCompletionNow();              // force (Ctrl+Space)
    void hidePopup();
    void acceptCompletion(const CompletionItem& item);
    void reloadSchema();                      // async fetch via adapter

    IDatabaseAdapter* adapter_ = nullptr;

    QPlainTextEdit*   editor_      = nullptr;
    SqlHighlighter*   hl_          = nullptr;
    QPushButton*      runBtn_      = nullptr;
    QLabel*           statusLbl_   = nullptr;
    QHBoxLayout*      toolbarLay_  = nullptr;  // owned by the `top` widget
    QSplitter*        splitter_    = nullptr;
    QTableView*       resultView_  = nullptr;
    QueryResultModel* resultModel_ = nullptr;

    QPushButton*      exportResultBtn_  = nullptr;
    QMenu*            exportResultMenu_ = nullptr;

    std::unique_ptr<AutocompleteProvider> provider_;
    std::unique_ptr<SqlContextParser>     parser_;
    CompletionPopup*  popup_       = nullptr;
    QTimer*           debounce_    = nullptr;
};

}
