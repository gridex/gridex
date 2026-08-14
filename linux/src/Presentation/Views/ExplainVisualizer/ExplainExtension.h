#pragma once
// Application bootstrap: registers an "Explain" button on every QueryEditorView.
// Called once from main() before MainWindow is constructed.

namespace gridex::explain {

// Register the toolbar extension. Safe to call multiple times — only
// the first call wins. The hook is process-wide; new editors created
// after this call pick it up automatically.
void registerQueryEditorExtension();

}
