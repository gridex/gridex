#include "Presentation/Views/Backup/BackupProgressDialog.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProcessEnvironment>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include "Core/Enums/DatabaseType.h"

namespace gridex {

namespace {

struct DumpSpec {
    QString     program;
    QStringList args;
    bool        stdoutToFile = false;
    bool        stdinFromFile = false;
};

std::optional<DumpSpec> makeBackupSpec(const ConnectionConfig& c, const QString& outPath) {
    DumpSpec s;
    switch (c.databaseType) {
        case DatabaseType::PostgreSQL:
            s.program = QStringLiteral("pg_dump");
            s.args << QStringLiteral("-h") << QString::fromStdString(c.host.value_or("localhost"))
                   << QStringLiteral("-p") << QString::number(c.port.value_or(5432))
                   << QStringLiteral("-U") << QString::fromStdString(c.username.value_or(""))
                   << QStringLiteral("-f") << outPath
                   << QString::fromStdString(c.database.value_or(""));
            return s;
        case DatabaseType::MySQL:
            s.program = QStringLiteral("mysqldump");
            s.args << QStringLiteral("-h") << QString::fromStdString(c.host.value_or("localhost"))
                   << QStringLiteral("-P") << QString::number(c.port.value_or(3306))
                   << QStringLiteral("-u") << QString::fromStdString(c.username.value_or(""))
                   << (QStringLiteral("--result-file=") + outPath)
                   << QString::fromStdString(c.database.value_or(""));
            return s;
        case DatabaseType::SQLite:
            s.program = QStringLiteral("sqlite3");
            s.args << QString::fromStdString(c.filePath.value_or("")) << QStringLiteral(".dump");
            s.stdoutToFile = true;
            return s;
        default:
            return std::nullopt;
    }
}

std::optional<DumpSpec> makeRestoreSpec(const ConnectionConfig& c, const QString& inPath) {
    DumpSpec s;
    switch (c.databaseType) {
        case DatabaseType::PostgreSQL:
            s.program = QStringLiteral("psql");
            s.args << QStringLiteral("-h") << QString::fromStdString(c.host.value_or("localhost"))
                   << QStringLiteral("-p") << QString::number(c.port.value_or(5432))
                   << QStringLiteral("-U") << QString::fromStdString(c.username.value_or(""))
                   << QStringLiteral("-d") << QString::fromStdString(c.database.value_or(""))
                   << QStringLiteral("-f") << inPath;
            return s;
        case DatabaseType::MySQL:
            s.program = QStringLiteral("mysql");
            s.args << QStringLiteral("-h") << QString::fromStdString(c.host.value_or("localhost"))
                   << QStringLiteral("-P") << QString::number(c.port.value_or(3306))
                   << QStringLiteral("-u") << QString::fromStdString(c.username.value_or(""))
                   << QString::fromStdString(c.database.value_or(""));
            s.stdinFromFile = true;
            return s;
        case DatabaseType::SQLite:
            s.program = QStringLiteral("sqlite3");
            s.args << QString::fromStdString(c.filePath.value_or(""));
            s.stdinFromFile = true;
            return s;
        default:
            return std::nullopt;
    }
}

}  // namespace

BackupProgressDialog::BackupProgressDialog(Mode mode,
                                           const ConnectionConfig& cfg,
                                           const std::optional<std::string>& password,
                                           const QString& filePath,
                                           QWidget* parent)
    : QDialog(parent)
    , mode_(mode)
    , cfg_(cfg)
    , password_(password)
    , filePath_(filePath)
{
    const QString dbName = QString::fromStdString(cfg_.database.value_or(cfg_.name));
    buildUi(dbName);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    resize(560, 360);
}

BackupProgressDialog::~BackupProgressDialog() {
    if (process_ && process_->state() != QProcess::NotRunning) {
        process_->terminate();
        process_->waitForFinished(500);
        if (process_->state() != QProcess::NotRunning)
            process_->kill();
    }
}

void BackupProgressDialog::buildUi(const QString& dbName) {
    const bool isBackup = (mode_ == Mode::Backup);
    setWindowTitle(isBackup ? tr("Backup Database") : tr("Restore Database"));

    // gxBackupHeader / gxBackupBar / gxBackupLog selectors live in
    // resources/style-gx{,-light}.qss under "Backup progress dialog".

    auto* root = new QVBoxLayout(this);
    root->setSpacing(10);
    root->setContentsMargins(16, 14, 16, 14);

    headerLabel_ = new QLabel(
        isBackup ? tr("Backing up %1...").arg(dbName)
                 : tr("Restoring %1...").arg(dbName),
        this);
    headerLabel_->setObjectName(QStringLiteral("gxBackupHeader"));
    root->addWidget(headerLabel_);

    progressBar_ = new QProgressBar(this);
    progressBar_->setObjectName(QStringLiteral("gxBackupBar"));
    progressBar_->setRange(0, 0);  // indeterminate
    progressBar_->setTextVisible(false);
    progressBar_->setFixedHeight(6);
    root->addWidget(progressBar_);

    logView_ = new QPlainTextEdit(this);
    logView_->setObjectName(QStringLiteral("gxBackupLog"));
    logView_->setReadOnly(true);
    logView_->setMaximumBlockCount(500);
    logView_->setMinimumHeight(180);
    root->addWidget(logView_, 1);

    auto* btnRow = new QHBoxLayout;
    btnRow->setSpacing(8);
    btnRow->addStretch();

    cancelBtn_ = new QPushButton(tr("Cancel"), this);
    connect(cancelBtn_, &QPushButton::clicked, this, &BackupProgressDialog::onCancel);
    btnRow->addWidget(cancelBtn_);

    closeBtn_ = new QPushButton(tr("Close"), this);
    closeBtn_->setProperty("gxKind", QStringLiteral("primary"));
    closeBtn_->setEnabled(false);
    connect(closeBtn_, &QPushButton::clicked, this, &QDialog::accept);
    btnRow->addWidget(closeBtn_);

    root->addLayout(btnRow);
}

void BackupProgressDialog::start() {
    const auto spec = (mode_ == Mode::Backup)
        ? makeBackupSpec(cfg_, filePath_)
        : makeRestoreSpec(cfg_, filePath_);

    if (!spec) {
        appendLog(tr("Error: database type not supported for dump operations."));
        markDone(false);
        return;
    }

    process_ = new QProcess(this);

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    if (password_ && !password_->empty()) {
        if (cfg_.databaseType == DatabaseType::PostgreSQL)
            env.insert(QStringLiteral("PGPASSWORD"), QString::fromStdString(*password_));
        else if (cfg_.databaseType == DatabaseType::MySQL)
            env.insert(QStringLiteral("MYSQL_PWD"), QString::fromStdString(*password_));
    }
    process_->setProcessEnvironment(env);

    if (spec->stdoutToFile)
        process_->setStandardOutputFile(filePath_, QIODevice::Truncate);
    if (spec->stdinFromFile)
        process_->setStandardInputFile(filePath_);

    connect(process_, &QProcess::readyReadStandardOutput,
            this, &BackupProgressDialog::onReadyRead);
    connect(process_, &QProcess::readyReadStandardError,
            this, &BackupProgressDialog::onReadyRead);
    connect(process_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int code, QProcess::ExitStatus) { onFinished(code, 0); });

    appendLog(tr("Starting %1...").arg(spec->program));
    process_->start(spec->program, spec->args);

    if (!process_->waitForStarted(5000)) {
        appendLog(tr("Failed to start '%1'. Ensure it is installed and on PATH.").arg(spec->program));
        markDone(false);
    }
}

void BackupProgressDialog::onReadyRead() {
    if (!process_) return;
    const auto outData = process_->readAllStandardOutput();
    const auto errData = process_->readAllStandardError();
    if (!outData.isEmpty())
        appendLog(QString::fromUtf8(outData).trimmed());
    if (!errData.isEmpty())
        appendLog(QString::fromUtf8(errData).trimmed());
}

void BackupProgressDialog::onFinished(int exitCode, int /*exitStatus*/) {
    // Drain any remaining output
    if (process_) {
        const auto out = process_->readAllStandardOutput();
        const auto err = process_->readAllStandardError();
        if (!out.isEmpty()) appendLog(QString::fromUtf8(out).trimmed());
        if (!err.isEmpty()) appendLog(QString::fromUtf8(err).trimmed());
    }
    markDone(exitCode == 0,
             exitCode != 0 ? tr("Process exited with code %1").arg(exitCode) : QString{});
}

void BackupProgressDialog::onCancel() {
    if (!process_ || process_->state() == QProcess::NotRunning) {
        reject();
        return;
    }
    cancelBtn_->setEnabled(false);
    appendLog(tr("Cancelling..."));
    process_->terminate();
    QTimer::singleShot(2000, this, [this] {
        if (process_ && process_->state() != QProcess::NotRunning)
            process_->kill();
    });
}

void BackupProgressDialog::appendLog(const QString& line) {
    if (line.trimmed().isEmpty()) return;
    logView_->appendPlainText(line);
}

void BackupProgressDialog::markDone(bool ok, const QString& extra) {
    succeeded_ = ok;
    progressBar_->setRange(0, 1);
    progressBar_->setValue(1);

    const bool isBackup = (mode_ == Mode::Backup);
    if (ok) {
        setWindowTitle(isBackup ? tr("Backup Complete") : tr("Restore Complete"));
        headerLabel_->setText(isBackup ? tr("Backup finished successfully.")
                                       : tr("Restore finished successfully."));
    } else {
        setWindowTitle(isBackup ? tr("Backup Failed") : tr("Restore Failed"));
        headerLabel_->setText(isBackup ? tr("Backup failed.")
                                       : tr("Restore failed."));
        if (!extra.isEmpty()) appendLog(extra);
    }

    cancelBtn_->setEnabled(false);
    closeBtn_->setEnabled(true);
    closeBtn_->setDefault(true);
    closeBtn_->setFocus();
}

}  // namespace gridex
