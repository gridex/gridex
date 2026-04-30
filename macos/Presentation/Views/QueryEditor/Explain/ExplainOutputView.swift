// ExplainOutputView.swift
// Gridex
//
// Three view-modes over a single EXPLAIN output string:
//   • Text — server output verbatim, monospace, optional word wrap
//   • JSON — JSONSerialization-prettified + lightweight syntax highlight
//   • Tree — recursive DisclosureGroup walk of the JSON plan
//
// Read-only browser only. No heat coloring, no slow-node detection, no
// recommendations. The richer "Analysis tab with timing heatmap +
// recommendations" view lives in the Enterprise Edition's
// EEExplainVisualizerView; this OSS surface intentionally stops short.

import SwiftUI

enum ExplainViewMode: String, CaseIterable, Identifiable {
    case text, json, tree

    var id: String { rawValue }
    var displayName: String {
        switch self {
        case .text: return "Text"
        case .json: return "JSON"
        case .tree: return "Tree"
        }
    }
    var systemImage: String {
        switch self {
        case .text: return "text.alignleft"
        case .json: return "curlybraces"
        case .tree: return "list.bullet.indent"
        }
    }
}

struct ExplainOutputView: View {

    /// Raw output as returned by the server. For PG `FORMAT TEXT` this is the
    /// concatenated `QUERY PLAN` rows; for `FORMAT JSON` it's the entire JSON
    /// document the server emits in the first row's first cell.
    let raw: String

    /// What format the user asked for. Used to pick the initial view mode and
    /// to gate the Tree view (only meaningful for JSON/YAML output).
    let format: ExplainOptions.Format

    /// Whether the EXPLAIN was run with ANALYZE on. Drives the inline hint
    /// banner below — without ANALYZE the user only sees plan estimates, not
    /// actual times / row counts / buffer stats, which is rarely enough to
    /// debug a real performance issue.
    let analyzed: Bool

    @State private var mode: ExplainViewMode
    @State private var wordWrap: Bool = true

    init(raw: String, format: ExplainOptions.Format, analyzed: Bool) {
        self.raw = raw
        self.format = format
        self.analyzed = analyzed
        // Auto-pick: if the user asked for JSON, default to Tree (most useful).
        // Otherwise default to Text.
        let initial: ExplainViewMode = (format == .json) ? .tree : .text
        _mode = State(initialValue: initial)
    }

    /// View modes available for the current output. Non-JSON plans can't be
    /// browsed as a tree — hide the Tree segment in that case so the picker
    /// doesn't show a non-functional option.
    private var availableModes: [ExplainViewMode] {
        format == .json ? ExplainViewMode.allCases : [.text]
    }

    var body: some View {
        VStack(spacing: 0) {
            header
            Divider()
            if !analyzed {
                analyzeHint
                Divider()
            }
            content
        }
    }

    /// Inline tip surfaced when ANALYZE is off. Most user-facing perf debug
    /// requires actual times / row counts / cache stats — this nudges the
    /// user toward the right toggles without forcing them.
    private var analyzeHint: some View {
        HStack(alignment: .firstTextBaseline, spacing: 6) {
            Image(systemName: "lightbulb")
                .font(.system(size: 11))
                .foregroundStyle(.yellow)
            Text("Tip: enable **Analyze** + **Buffers** in the options menu to see actual times and cache stats. The current output is plan estimates only.")
                .font(.system(size: 11))
                .foregroundStyle(.secondary)
            Spacer()
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 6)
        .background(Color(nsColor: .controlBackgroundColor).opacity(0.5))
    }

    // MARK: - Header

    private var header: some View {
        HStack(spacing: 8) {
            // View-mode picker. Single-segment for non-JSON output (no choice
            // to make), full picker when JSON.
            if availableModes.count > 1 {
                Picker("", selection: $mode) {
                    ForEach(availableModes) { m in
                        Label(m.displayName, systemImage: m.systemImage).tag(m)
                    }
                }
                .pickerStyle(.segmented)
                .labelsHidden()
                .fixedSize()
            } else {
                Label("Text", systemImage: ExplainViewMode.text.systemImage)
                    .font(.system(size: 11))
                    .foregroundStyle(.secondary)
            }

            Spacer()

            if mode == .text || mode == .json {
                Toggle(isOn: $wordWrap) {
                    Label("Wrap", systemImage: "arrow.turn.down.left")
                        .labelStyle(.iconOnly)
                }
                .toggleStyle(.button)
                .controlSize(.small)
                .help("Word wrap")
            }

            Button {
                copyToPasteboard()
            } label: {
                Label("Copy", systemImage: "doc.on.doc")
                    .labelStyle(.iconOnly)
            }
            .buttonStyle(.borderless)
            .controlSize(.small)
            .help("Copy raw output")
        }
        .padding(.horizontal, 8)
        .padding(.vertical, 4)
        .background(Color(nsColor: .windowBackgroundColor))
    }

    // MARK: - Body

    @ViewBuilder
    private var content: some View {
        switch mode {
        case .text: textView
        case .json: jsonView
        case .tree: treeView
        }
    }

    // MARK: - Text mode

    private var textView: some View {
        ScrollView([.vertical, wordWrap ? [] : .horizontal]) {
            Text(raw)
                .font(.system(size: 12, design: .monospaced))
                .textSelection(.enabled)
                .frame(maxWidth: wordWrap ? .infinity : nil, alignment: .leading)
                .padding(8)
        }
    }

    // MARK: - JSON mode

    private var jsonView: some View {
        let pretty = ExplainJSONPrettyPrinter.prettyPrint(raw) ?? raw
        return ScrollView([.vertical, wordWrap ? [] : .horizontal]) {
            ExplainJSONHighlightedText(text: pretty)
                .font(.system(size: 12, design: .monospaced))
                .textSelection(.enabled)
                .frame(maxWidth: wordWrap ? .infinity : nil, alignment: .leading)
                .padding(8)
        }
    }

    // MARK: - Tree mode

    @ViewBuilder
    private var treeView: some View {
        if let nodes = ExplainPlanReader.parse(jsonString: raw), !nodes.isEmpty {
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 0) {
                    ForEach(nodes) { node in
                        ExplainPlanNodeRow(node: node, depth: 0)
                    }
                }
                .padding(8)
            }
        } else {
            Text("Tree view requires `FORMAT JSON` output.\nSwitch the EXPLAIN format to JSON and re-run.")
                .font(.system(size: 12))
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
    }

    // MARK: - Actions

    private func copyToPasteboard() {
        let pb = NSPasteboard.general
        pb.clearContents()
        pb.setString(raw, forType: .string)
    }
}

// MARK: - JSON pretty printer

enum ExplainJSONPrettyPrinter {
    /// Re-encode the JSON with `.prettyPrinted + .sortedKeys` for stable output.
    /// Returns nil when `raw` isn't valid JSON — caller falls back to the raw
    /// string so the user sees something instead of a blank panel.
    static func prettyPrint(_ raw: String) -> String? {
        guard let data = raw.data(using: .utf8),
              let obj = try? JSONSerialization.jsonObject(with: data, options: [.fragmentsAllowed]),
              let pretty = try? JSONSerialization.data(
                  withJSONObject: obj,
                  options: [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes]
              ),
              let string = String(data: pretty, encoding: .utf8)
        else { return nil }
        return string
    }
}

// MARK: - Lightweight JSON syntax highlight

/// Tokenises JSON text and emits each token as a coloured `Text` segment.
/// Single-pass O(n), no parser — just lexical: strings (red-ish), numbers
/// (green-ish), keywords (purple), keys (blue, when followed by `:`).
struct ExplainJSONHighlightedText: View {
    let text: String

    var body: some View {
        // SwiftUI can concat Text segments via `+`. Build incrementally.
        var result = Text("")
        for token in tokenise(text) {
            result = result + Text(token.lexeme)
                .foregroundColor(token.kind.color)
        }
        return result.lineLimit(nil)
    }

    private struct Token {
        let lexeme: String
        let kind: Kind
        enum Kind {
            case key, string, number, keyword, punctuation, whitespace, other
            var color: Color {
                switch self {
                case .key:         return Color.blue
                case .string:      return Color(nsColor: .systemRed)
                case .number:      return Color(nsColor: .systemGreen)
                case .keyword:     return Color(nsColor: .systemPurple)
                case .punctuation: return Color.secondary
                case .whitespace:  return Color.primary
                case .other:       return Color.primary
                }
            }
        }
    }

    /// Naïve lexer: enough to colour the canonical Postgres EXPLAIN JSON output.
    /// Doesn't try to be a full RFC 8259 parser — fragments + valid JSON both work.
    private func tokenise(_ s: String) -> [Token] {
        var tokens: [Token] = []
        var i = s.startIndex

        while i < s.endIndex {
            let c = s[i]
            if c.isWhitespace || c.isNewline {
                let start = i
                while i < s.endIndex && (s[i].isWhitespace || s[i].isNewline) { i = s.index(after: i) }
                tokens.append(Token(lexeme: String(s[start..<i]), kind: .whitespace))
            } else if c == "\"" {
                // Read a string literal. Look ahead to decide if it's a key
                // (followed by `:` after optional whitespace).
                let start = i
                i = s.index(after: i)
                while i < s.endIndex && s[i] != "\"" {
                    if s[i] == "\\" && s.index(after: i) < s.endIndex {
                        i = s.index(after: i) // skip escaped char
                    }
                    i = s.index(after: i)
                }
                if i < s.endIndex { i = s.index(after: i) }
                let literal = String(s[start..<i])

                // Peek for `:` to detect "key" role.
                var j = i
                while j < s.endIndex && s[j].isWhitespace { j = s.index(after: j) }
                let isKey = j < s.endIndex && s[j] == ":"
                tokens.append(Token(lexeme: literal, kind: isKey ? .key : .string))
            } else if c.isNumber || c == "-" {
                let start = i
                if c == "-" { i = s.index(after: i) }
                while i < s.endIndex && (s[i].isNumber || s[i] == "." || s[i] == "e" || s[i] == "E" || s[i] == "+" || s[i] == "-") {
                    i = s.index(after: i)
                }
                tokens.append(Token(lexeme: String(s[start..<i]), kind: .number))
            } else if c.isLetter {
                let start = i
                while i < s.endIndex && s[i].isLetter { i = s.index(after: i) }
                let word = String(s[start..<i])
                let kind: Token.Kind = (word == "true" || word == "false" || word == "null") ? .keyword : .other
                tokens.append(Token(lexeme: word, kind: kind))
            } else if "{}[],:".contains(c) {
                tokens.append(Token(lexeme: String(c), kind: .punctuation))
                i = s.index(after: i)
            } else {
                tokens.append(Token(lexeme: String(c), kind: .other))
                i = s.index(after: i)
            }
        }
        return tokens
    }
}

// MARK: - Tree mode plumbing

/// Lightweight read-only walker over the Postgres `EXPLAIN (FORMAT JSON)`
/// output shape. NOT a plan analysis engine — that's an Enterprise Edition
/// feature. This keeps just enough structure to render a DisclosureGroup
/// tree of node names + cost summary lines.
struct ExplainPlanNode: Identifiable {
    let id = UUID()
    let nodeType: String
    let summary: String          // "(cost=… rows=… loops=…)"
    let attributes: [(String, String)]  // remaining flat key/value pairs for an inspector row
    let children: [ExplainPlanNode]
}

enum ExplainPlanReader {
    /// Parse `EXPLAIN (FORMAT JSON)` text into a flat array of root nodes
    /// (PG returns one root per statement). Returns nil when the input
    /// isn't a JSON array or doesn't have the expected `Plan` shape.
    static func parse(jsonString: String) -> [ExplainPlanNode]? {
        guard let data = jsonString.data(using: .utf8),
              let arr = try? JSONSerialization.jsonObject(with: data) as? [[String: Any]]
        else { return nil }

        let nodes = arr.compactMap { obj -> ExplainPlanNode? in
            guard let plan = obj["Plan"] as? [String: Any] else { return nil }
            return makeNode(plan)
        }
        return nodes.isEmpty ? nil : nodes
    }

    private static func makeNode(_ plan: [String: Any]) -> ExplainPlanNode {
        let nodeType = plan["Node Type"] as? String ?? "(unknown)"

        // Summary line — the same shape PG emits in FORMAT TEXT.
        var summaryParts: [String] = []
        if let startup = plan["Startup Cost"] as? Double, let total = plan["Total Cost"] as? Double {
            summaryParts.append(String(format: "cost=%.2f..%.2f", startup, total))
        } else if let total = plan["Total Cost"] as? Double {
            summaryParts.append(String(format: "cost=%.2f", total))
        }
        if let rows = plan["Plan Rows"] as? Int { summaryParts.append("rows=\(rows)") }
        if let width = plan["Plan Width"] as? Int { summaryParts.append("width=\(width)") }

        // Pull every other primitive key into the attributes list so the user
        // can browse them under a node row without us interpreting anything.
        var attributes: [(String, String)] = []
        let exclude: Set<String> = [
            "Plans", "Node Type",
            "Startup Cost", "Total Cost", "Plan Rows", "Plan Width",
        ]
        for key in plan.keys.sorted() where !exclude.contains(key) {
            let value = plan[key]
            if let str = value as? String {
                attributes.append((key, str))
            } else if let num = value as? NSNumber {
                // JSONSerialization wraps both numbers and booleans in NSNumber.
                // CFBoolean type check is the reliable way to tell them apart;
                // a plain `as? Bool` would mis-classify 0/1 as false/true.
                if CFGetTypeID(num) == CFBooleanGetTypeID() {
                    attributes.append((key, num.boolValue ? "true" : "false"))
                } else {
                    attributes.append((key, num.stringValue))
                }
            }
            // Arrays/objects are skipped — we don't try to render them inline.
        }

        let childPlans = plan["Plans"] as? [[String: Any]] ?? []
        let children = childPlans.map(makeNode)

        return ExplainPlanNode(
            nodeType: nodeType,
            summary: summaryParts.joined(separator: " "),
            attributes: attributes,
            children: children
        )
    }
}

// MARK: - Tree row

/// One DisclosureGroup per node, recursively. Read-only — clicking a node
/// just expands/collapses; clicking an attribute does nothing. No timing
/// heat colors and no recommendation cards (those belong in EE).
struct ExplainPlanNodeRow: View {
    let node: ExplainPlanNode
    let depth: Int

    @State private var expanded: Bool = true

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            DisclosureGroup(isExpanded: $expanded) {
                VStack(alignment: .leading, spacing: 2) {
                    if !node.attributes.isEmpty {
                        ForEach(node.attributes, id: \.0) { (k, v) in
                            HStack(alignment: .firstTextBaseline, spacing: 6) {
                                Text(k)
                                    .font(.system(size: 11, design: .monospaced))
                                    .foregroundStyle(.secondary)
                                Text(v)
                                    .font(.system(size: 11, design: .monospaced))
                                    .textSelection(.enabled)
                            }
                            .padding(.leading, 16)
                        }
                    }
                    ForEach(node.children) { child in
                        ExplainPlanNodeRow(node: child, depth: depth + 1)
                    }
                }
            } label: {
                HStack(spacing: 6) {
                    Image(systemName: "chevron.right.circle.fill")
                        .foregroundStyle(.tint)
                        .font(.system(size: 11))
                    Text(node.nodeType)
                        .font(.system(size: 12, weight: .semibold, design: .monospaced))
                    if !node.summary.isEmpty {
                        Text(node.summary)
                            .font(.system(size: 11, design: .monospaced))
                            .foregroundStyle(.secondary)
                    }
                }
            }
        }
    }
}
