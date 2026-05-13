#pragma once

// 22px statusbar matching .gx-status in gridex.css.
//
// Left cluster:
//   [green dot] <conn-name>     · tx: <READ|WRITE>     · <N> rows · <M> ms
// Right cluster:
//   SQL · PostgreSQL  · UTF-8 · LF · Spaces: 4 · Ln X, Col Y · Modified|Saved
//
// JetBrains Mono 10.5px throughout; gradient bg-2 → bg-1; 1px black top
// border + inset 1px highlight (border colour) under it.

#include <QStatusBar>

class QLabel;

namespace gridex {

class GxStatusBar : public QStatusBar {
    Q_OBJECT
public:
    explicit GxStatusBar(QWidget* parent = nullptr);

public slots:
    // Left cluster
    void setConnection(const QString& text);    // e.g. "prod-readonly @ db:5432"
    void setTxState(const QString& tx);         // "READ" | "WRITE" | "—"
    void setRowCount(int rows);
    void setQueryTime(int milliseconds);

    // Right cluster
    void setLanguage(const QString& language);  // e.g. "SQL · PostgreSQL"
    void setEncoding(const QString& enc);       // default "UTF-8"
    void setLineEnding(const QString& le);      // default "LF"
    void setIndent(const QString& s);           // default "Spaces: 4"
    void setCursorPos(int line, int col);
    void setDirty(bool dirty);                  // "● Modified" vs "Saved"

    void clearAll();

private:
    void buildSegments();
    QLabel* makeSegment(const QString& gxText = QString());

    // Left cluster widgets
    QLabel* connDot_      = nullptr;
    QLabel* connLabel_    = nullptr;
    QLabel* txLabel_      = nullptr;
    QLabel* rowsLabel_    = nullptr;
    QLabel* timeLabel_    = nullptr;
    // Right cluster widgets
    QLabel* langLabel_    = nullptr;
    QLabel* encLabel_     = nullptr;
    QLabel* leLabel_      = nullptr;
    QLabel* indentLabel_  = nullptr;
    QLabel* cursorLabel_  = nullptr;
    QLabel* dirtyLabel_   = nullptr;
};

}  // namespace gridex
