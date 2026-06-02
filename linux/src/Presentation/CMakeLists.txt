find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets Concurrent Svg)

qt_add_library(gridex-presentation STATIC
    Theme/ThemeManager.cpp
    ViewModels/AIChatViewModel.cpp
    ViewModels/ConnectionListViewModel.cpp
    ViewModels/WorkspaceState.cpp
    Views/AIChat/AIChatView.cpp
    Views/Backup/BackupProgressDialog.cpp
    Views/ConnectionForm/ConnectionFormDialog.cpp
    Views/CreateTable/CreateTableDialog.cpp
    Views/DataGrid/DataGridView.cpp
    Views/DatabaseSwitcher/DatabaseSwitcherDialog.cpp
    Views/ERDiagram/ERDiagramView.cpp
    Views/ERDiagram/ERDiagramExporter.cpp
    Views/DataGrid/QueryResultModel.cpp
    Views/Details/DetailsPanel.cpp
    Views/FilterBar/FilterBarView.cpp
    Views/FunctionDetail/FunctionDetailView.cpp
    Views/QueryLog/QueryLogPanel.cpp
    Views/Redis/RedisKeyDetailView.cpp
    Views/MongoDB/MongoCollectionView.cpp
    Views/TableStructure/TableStructureView.cpp
    Views/Home/HomeBrandingPanel.cpp
    Views/Main/WorkspaceView.cpp
    Views/QueryEditor/QueryEditorView.cpp
    Views/QueryEditor/SqlHighlighter.cpp
    Views/Settings/SettingsDialog.cpp
    Views/Chrome/GxToolbar.cpp
    Views/Chrome/GxStatusBar.cpp
    Views/Chrome/GxIcons.cpp
    Views/Chrome/GxActivityBar.cpp
    Views/QueryEditor/Autocomplete/AutocompleteProvider.cpp
    Views/QueryEditor/Autocomplete/CompletionPopup.cpp
    Views/QueryEditor/Autocomplete/SqlContextParser.cpp
    Views/Sidebar/ConnectionRowWidget.cpp
    Views/Sidebar/ConnectionSidebar.cpp
    Views/ImportSQL/SqlImportWizard.cpp
    Views/Sidebar/WorkspaceSidebar.cpp
    Views/Sidebar/SidebarPanelStack.cpp
    Views/TableList/TableGridView.cpp
    Views/StatusBar/StatusBarView.cpp
    Views/TabBar/WorkspaceTabBar.cpp
    Windows/Main/MainWindow.cpp
    Views/MCP/MCPWindowState.cpp
    Views/MCP/MCPWindow.cpp
    Views/MCP/MCPOverviewView.cpp
    Views/MCP/MCPConnectionsView.cpp
    Views/MCP/MCPActivityView.cpp
    Views/MCP/MCPSetupView.cpp
    Views/MCP/MCPAdvancedView.cpp
)

qt_add_resources(gridex-presentation "assets"
    PREFIX "/"
    BASE ${CMAKE_SOURCE_DIR}/resources
    FILES
        ${CMAKE_SOURCE_DIR}/resources/logo.png
        ${CMAKE_SOURCE_DIR}/resources/style-dark.qss
        ${CMAKE_SOURCE_DIR}/resources/style-light.qss
        ${CMAKE_SOURCE_DIR}/resources/style-gx.qss
        ${CMAKE_SOURCE_DIR}/resources/style-gx-light.qss
)

target_include_directories(gridex-presentation PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(gridex-presentation PUBLIC
    gridex-core
    gridex-data
    gridex-services
    gridex-domain
    gridex-app
    Qt6::Widgets
    Qt6::Concurrent
    Qt6::Svg
)
