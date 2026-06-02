#include "Presentation/Views/ImportSQL/SqlImportWizard.h"

#include <QCheckBox>
#include <QColor>
#include <QFileDialog>
#include <QFile>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QTextBlock>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>
#include <QFutureWatcher>

#include "Core/Protocols/Database/IDatabaseAdapter.h"
#include "Core/Utils/SqlStatementSplitter.h"

namespace gridex {

namespace {

// Build a line-number map: for each statement index, store the 1-based line
// where it begins in the original source.
std::vector<int> buildLineMap(const std::string& src,
                               const std::vector<std::string>& stmts) {
    std::vector<int> lines;
    lines.reserve(stmts.size());
    std::size_t searchFrom = 0;
    int currentLine = 1;
    for (const auto& stmt : stmts) {
        if (stmt.empty()) { lines.push_back(1); continue; }
        const auto pos = src.find(stmt.substr(0, std::min(stmt.size(), std::size_t{40})),
                                  searchFrom);
        if (pos == std::string::npos) {
            lines.push_back(currentLine);
        } else {
            int line = 1;
            for (std::size_t i = 0; i < pos && i < src.size(); ++i)
                if (src[i] == '\n') ++line;
            lines.push_back(line);
            currentLine = line;
            searchFrom = pos + 1;
        }
    }
    return lines;
}

}  // namespace

// ---------------------------------------------------------------------------

SqlImportWizard::SqlImportWizard(IDatabaseAdapter* adapter,
                                 const QString&    filePath,
                                 QWidget*          parent)
    : QDialog(parent), adapter_(adapter), filePath_(filePath)
{
    setWindowTitle(tr("Import SQL Wizard"));
    setMinimumSize(700, 560);
    setAttribute(Qt::WA_DeleteOnClose, false);

    // gxImport* / QLabel[gxRole="meta"] selectors live in
    // resources/style-gx{,-light}.qss under "SQL import wizard".

    auto* root = new QVBoxLayout(this);
    root->setSpacing(8);
    root->setContentsMargins(12, 12, 12, 12);

    // ---- File row ----
    auto* fileRow = new QHBoxLayout;
    fileLabel_ = new QLabel(tr("(no file selected)"), this);
    fileLabel_->setObjectName(QStringLiteral("gxImportFile"));
    fileLabel_->setWordWrap(false);
    fileLabel_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    browseBtn_ = new QPushButton(tr("Browse…"), this);
    browseBtn_->setCursor(Qt::PointingHandCursor);
    fileRow->addWidget(fileLabel_, 1);
    fileRow->addWidget(browseBtn_);
    root->addLayout(fileRow);

    // ---- Meta row: dialect + stmt count ----
    auto* metaRow = new QHBoxLayout;
    dialectLabel_   = new QLabel(this);
    stmtCountLabel_ = new QLabel(this);
    dialectLabel_->setProperty("gxRole", QStringLiteral("meta"));
    stmtCountLabel_->setProperty("gxRole", QStringLiteral("meta"));
    metaRow->addWidget(dialectLabel_);
    metaRow->addStretch();
    metaRow->addWidget(stmtCountLabel_);
    root->addLayout(metaRow);

    // ---- Preview ----
    auto* previewLbl = new QLabel(tr("Preview (first 2 000 chars):"), this);
    previewLbl->setStyleSheet(QStringLiteral("font-size: 11px;"));
    root->addWidget(previewLbl);

    previewEdit_ = new QPlainTextEdit(this);
    previewEdit_->setObjectName(QStringLiteral("gxImportPreview"));
    previewEdit_->setReadOnly(true);
    previewEdit_->setMaximumBlockCount(0);
    previewEdit_->setMinimumHeight(160);
    root->addWidget(previewEdit_, 2);

    // ---- Options ----
    transactionChk_ = new QCheckBox(tr("Run in transaction (rollback all on failure)"), this);
    transactionChk_->setChecked(true);
    continueErrChk_ = new QCheckBox(tr("Continue on error (collect all errors)"), this);
    continueErrChk_->setChecked(false);
    // The two options are mutually exclusive in a meaningful way, but we allow
    // the user to override: if both checked, continueErr takes precedence.
    root->addWidget(transactionChk_);
    root->addWidget(continueErrChk_);

    // ---- Progress ----
    progressBar_ = new QProgressBar(this);
    progressBar_->setObjectName(QStringLiteral("gxImportBar"));
    progressBar_->setRange(0, 100);
    progressBar_->setValue(0);
    progressBar_->setVisible(false);
    progressLabel_ = new QLabel(this);
    progressLabel_->setStyleSheet(QStringLiteral("font-size: 11px;"));
    progressLabel_->setVisible(false);
    root->addWidget(progressBar_);
    root->addWidget(progressLabel_);

    // ---- Error list ----
    auto* errLbl = new QLabel(tr("Errors (double-click to jump to line):"), this);
    errLbl->setStyleSheet(QStringLiteral("font-size: 11px;"));
    root->addWidget(errLbl);

    errorList_ = new QListWidget(this);
    errorList_->setObjectName(QStringLiteral("gxImportErrors"));
    errorList_->setMaximumHeight(120);
    errorList_->setSelectionMode(QAbstractItemView::SingleSelection);
    root->addWidget(errorList_);

    // ---- Button row ----
    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch();
    closeBtn_  = new QPushButton(tr("Close"), this);
    importBtn_ = new QPushButton(tr("Import"), this);
    importBtn_->setProperty("gxKind", QStringLiteral("primary"));
    importBtn_->setDefault(true);
    importBtn_->setEnabled(false);
    closeBtn_->setCursor(Qt::PointingHandCursor);
    importBtn_->setCursor(Qt::PointingHandCursor);
    btnRow->addWidget(closeBtn_);
    btnRow->addWidget(importBtn_);
    root->addLayout(btnRow);

    connect(browseBtn_, &QPushButton::clicked, this, &SqlImportWizard::onBrowse);
    connect(importBtn_, &QPushButton::clicked, this, &SqlImportWizard::onImport);
    connect(closeBtn_,  &QPushButton::clicked, this, &QDialog::accept);
    connect(errorList_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        if (!item) return;
        onErrorDoubleClicked(errorList_->row(item));
    });

    if (!filePath_.isEmpty()) loadPreview();
}

SqlImportWizard::~SqlImportWizard() = default;

void SqlImportWizard::onBrowse() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open SQL File"),
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
        tr("SQL (*.sql);;All files (*)"));
    if (path.isEmpty()) return;
    filePath_ = path;
    loadPreview();
}

void SqlImportWizard::loadPreview() {
    errorList_->clear();
    progressBar_->setValue(0);
    progressBar_->setVisible(false);
    progressLabel_->setVisible(false);

    QFile f(filePath_);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        fileLabel_->setText(tr("Cannot open: %1").arg(filePath_));
        importBtn_->setEnabled(false);
        return;
    }
    const QByteArray raw = f.readAll();
    f.close();

    const std::string buf = raw.toStdString();
    statements_ = splitSqlStatements(buf);
    stmtLines_  = buildLineMap(buf, statements_);

    const QString fname = QFileInfo(filePath_).fileName();
    fileLabel_->setText(QStringLiteral("%1  (%2 bytes)").arg(fname).arg(raw.size()));

    const QString dialect = QString::fromStdString(detectSqlDialectHint(buf));
    dialectLabel_->setText(tr("Dialect hint: %1").arg(dialect));
    stmtCountLabel_->setText(tr("%1 statement(s)").arg(statements_.size()));

    const QString preview = QString::fromUtf8(raw).left(2000);
    previewEdit_->setPlainText(preview);

    importBtn_->setEnabled(!statements_.empty() && adapter_ != nullptr);
}

// ---------------------------------------------------------------------------
// Async import using QtConcurrent + signal/slot queued connection
// ---------------------------------------------------------------------------

void SqlImportWizard::onImport() {
    if (statements_.empty() || !adapter_) return;

    setImporting(true);
    errorList_->clear();
    progressBar_->setRange(0, static_cast<int>(statements_.size()));
    progressBar_->setValue(0);
    progressLabel_->setText(tr("Starting…"));

    const bool useTransaction = transactionChk_->isChecked() && !continueErrChk_->isChecked();
    const bool continueOnErr  = continueErrChk_->isChecked();

    // Capture needed data for the worker; avoid capturing Qt widgets.
    auto stmts    = statements_;
    auto* adapter = adapter_;

    // We emit progress via a lambda that routes through a queued connection.
    // Use a raw signal on *this* via QMetaObject::invokeMethod so the worker
    // thread can safely update the UI.
    auto* watcher = new QFutureWatcher<void>(this);

    connect(watcher, &QFutureWatcher<void>::finished, this, [this, watcher] {
        watcher->deleteLater();
        onImportFinished();
    });

    QFuture<void> fut = QtConcurrent::run([this, stmts, adapter, useTransaction, continueOnErr] {
        const int total = static_cast<int>(stmts.size());

        if (useTransaction) {
            try { adapter->beginTransaction(); }
            catch (const std::exception& e) {
                QMetaObject::invokeMethod(this, "onStatementDone", Qt::QueuedConnection,
                    Q_ARG(int, 0), Q_ARG(int, total),
                    Q_ARG(QString, tr("BEGIN failed: %1").arg(e.what())));
                return;
            }
        }

        bool aborted = false;
        for (int i = 0; i < total; ++i) {
            QString err;
            try {
                adapter->executeRaw(stmts[static_cast<std::size_t>(i)]);
            } catch (const std::exception& e) {
                err = QString::fromUtf8(e.what());
            }

            QMetaObject::invokeMethod(this, "onStatementDone", Qt::QueuedConnection,
                Q_ARG(int, i + 1), Q_ARG(int, total), Q_ARG(QString, err));

            if (!err.isEmpty() && !continueOnErr) {
                aborted = true;
                break;
            }
        }

        if (useTransaction) {
            try {
                if (aborted) adapter->rollbackTransaction();
                else         adapter->commitTransaction();
            } catch (const std::exception& e) {
                const QString msg = aborted
                    ? tr("ROLLBACK failed: %1").arg(e.what())
                    : tr("COMMIT failed: %1").arg(e.what());
                QMetaObject::invokeMethod(this, "onStatementDone", Qt::QueuedConnection,
                    Q_ARG(int, total), Q_ARG(int, total), Q_ARG(QString, msg));
            }
        }
    });

    watcher->setFuture(fut);
}

void SqlImportWizard::onStatementDone(int index, int total, const QString& error) {
    progressBar_->setValue(index);
    progressLabel_->setText(tr("Statement %1 / %2").arg(index).arg(total));

    if (!error.isEmpty()) {
        const int stmtIdx = index - 1;
        const int line = (stmtIdx >= 0 && stmtIdx < static_cast<int>(stmtLines_.size()))
                         ? stmtLines_[static_cast<std::size_t>(stmtIdx)] : 0;
        const QString label = line > 0
            ? QStringLiteral("[line %1] %2").arg(line).arg(error)
            : error;
        auto* item = new QListWidgetItem(label);
        item->setData(Qt::UserRole, line);
        item->setForeground(QColor("#e04b4a"));
        errorList_->addItem(item);
    }
}

void SqlImportWizard::onImportFinished() {
    setImporting(false);
    const int errCount = errorList_->count();
    if (errCount == 0) {
        progressLabel_->setText(tr("Done — all %1 statement(s) executed successfully.")
                                     .arg(statements_.size()));
    } else {
        progressLabel_->setText(tr("Done — %1 error(s). See list below.")
                                     .arg(errCount));
    }
}

void SqlImportWizard::onErrorDoubleClicked(int row) {
    auto* item = errorList_->item(row);
    if (!item) return;
    const int line = item->data(Qt::UserRole).toInt();
QTextDocument* doc = previewEdit_->document();
    if (!doc || line <= 0) return;

    QTextBlock block = doc->findBlockByLineNumber(line - 1);
    if (!block.isValid()) block = doc->lastBlock();

    QTextCursor cur(block);
    previewEdit_->setTextCursor(cur);
    previewEdit_->ensureCursorVisible();
}

void SqlImportWizard::setImporting(bool active) {
    importBtn_->setEnabled(!active);
    browseBtn_->setEnabled(!active);
    transactionChk_->setEnabled(!active);
    continueErrChk_->setEnabled(!active);
    progressBar_->setVisible(true);
    progressLabel_->setVisible(true);
}

}  // namespace gridex
