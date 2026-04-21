// ERDiagramView.swift
// Gridex
//
// SwiftUI wrapper for the ER diagram canvas.

import SwiftUI

struct ERDiagramView: View {
    let schema: String?
    @EnvironmentObject private var appState: AppState
    @StateObject private var viewModel = ERDiagramViewModel()

    var body: some View {
        ZStack {
            if viewModel.isLoading {
                VStack(spacing: 12) {
                    ProgressView()
                        .scaleEffect(0.8)
                    Text("Loading schema…")
                        .font(.system(size: 12))
                        .foregroundStyle(.secondary)
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else if viewModel.tables.isEmpty {
                VStack(spacing: 8) {
                    Image(systemName: "rectangle.3.group")
                        .font(.system(size: 32))
                        .foregroundStyle(.quaternary)
                    Text("No tables found")
                        .font(.system(size: 13))
                        .foregroundStyle(.secondary)
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                ERDiagramCanvasWrapper(viewModel: viewModel)
            }

            // Toolbar overlay — top-right
            if !viewModel.isLoading && !viewModel.tables.isEmpty {
                VStack {
                    HStack {
                        Spacer()
                        diagramToolbar
                    }
                    Spacer()
                }
                .padding(12)
            }

            // ClickHouse has no foreign keys — flag the diagram so users don't
            // mistake the missing relations for a bug.
            if appState.activeAdapter?.databaseType == .clickhouse && !viewModel.isLoading && !viewModel.tables.isEmpty {
                VStack {
                    Spacer()
                    HStack(spacing: 6) {
                        Image(systemName: "info.circle.fill")
                        Text("ClickHouse has no foreign keys — showing tables only.")
                            .font(.system(size: 11))
                    }
                    .foregroundStyle(.secondary)
                    .padding(.horizontal, 10)
                    .padding(.vertical, 6)
                    .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 8))
                    .padding(12)
                }
            }
        }
        .background(Color(nsColor: .controlBackgroundColor))
        .task {
            guard let adapter = appState.activeAdapter else { return }
            await viewModel.load(adapter: adapter, schema: schema)
        }
    }

    private var diagramToolbar: some View {
        HStack(spacing: 4) {
            Button(action: { viewModel.zoomIn() }) {
                Image(systemName: "plus.magnifyingglass")
                    .frame(width: 28, height: 28)
            }
            .buttonStyle(.borderless)

            Text("\(Int(viewModel.zoom * 100))%")
                .font(.system(size: 10, weight: .medium, design: .monospaced))
                .foregroundStyle(.secondary)
                .frame(width: 38)

            Button(action: { viewModel.zoomOut() }) {
                Image(systemName: "minus.magnifyingglass")
                    .frame(width: 28, height: 28)
            }
            .buttonStyle(.borderless)

            Divider()
                .frame(height: 16)

            Button(action: { viewModel.fitToView() }) {
                Image(systemName: "arrow.up.left.and.arrow.down.right")
                    .frame(width: 28, height: 28)
            }
            .buttonStyle(.borderless)
            .help("Fit to view")

            Button(action: { viewModel.autoLayout() }) {
                Image(systemName: "rectangle.3.group")
                    .frame(width: 28, height: 28)
            }
            .buttonStyle(.borderless)
            .help("Auto layout")
        }
        .padding(.horizontal, 8)
        .padding(.vertical, 4)
        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 8))
    }
}

// MARK: - NSViewRepresentable wrapper

struct ERDiagramCanvasWrapper: NSViewRepresentable {
    @ObservedObject var viewModel: ERDiagramViewModel

    func makeNSView(context: Context) -> NSScrollView {
        let canvas = ERDiagramCanvas(viewModel: viewModel)
        viewModel.canvas = canvas

        let scrollView = NSScrollView()
        scrollView.documentView = canvas
        scrollView.hasVerticalScroller = true
        scrollView.hasHorizontalScroller = true
        scrollView.allowsMagnification = true
        scrollView.minMagnification = 0.1
        scrollView.maxMagnification = 3.0
        scrollView.magnification = CGFloat(viewModel.zoom)
        scrollView.backgroundColor = .clear
        scrollView.drawsBackground = false
        scrollView.scrollerStyle = .overlay
        scrollView.autohidesScrollers = true
        scrollView.contentView.postsBoundsChangedNotifications = true

        // Observe magnification changes from scroll view
        NotificationCenter.default.addObserver(
            forName: NSScrollView.didEndLiveMagnifyNotification,
            object: scrollView,
            queue: .main
        ) { _ in
            Task { @MainActor in
                viewModel.zoom = Float(scrollView.magnification)
            }
        }

        // Initial layout after a short delay so the scroll view has its frame
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) {
            if viewModel.needsInitialLayout {
                viewModel.autoLayout()
                viewModel.fitToView()
                viewModel.needsInitialLayout = false
            }
        }

        return scrollView
    }

    func updateNSView(_ scrollView: NSScrollView, context: Context) {
        if abs(scrollView.magnification - CGFloat(viewModel.zoom)) > 0.01 {
            scrollView.magnification = CGFloat(viewModel.zoom)
        }
        (scrollView.documentView as? ERDiagramCanvas)?.needsDisplay = true
    }
}
