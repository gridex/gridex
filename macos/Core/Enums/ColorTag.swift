// ColorTag.swift
// Gridex

import AppKit
import SwiftUI

enum ColorTag: String, Codable, Sendable, CaseIterable {
    case red
    case orange
    case green
    case blue
    case purple
    case gray

    var nsColor: NSColor {
        switch self {
        case .red: return NSColor(calibratedRed: 0.886, green: 0.294, blue: 0.290, alpha: 1.0)
        case .orange: return NSColor(calibratedRed: 0.937, green: 0.624, blue: 0.153, alpha: 1.0)
        case .green: return NSColor(calibratedRed: 0.388, green: 0.600, blue: 0.133, alpha: 1.0)
        case .blue: return NSColor(calibratedRed: 0.216, green: 0.541, blue: 0.867, alpha: 1.0)
        case .purple: return NSColor(calibratedRed: 0.325, green: 0.290, blue: 0.718, alpha: 1.0)
        case .gray: return NSColor.secondaryLabelColor
        }
    }

    var swiftUIColor: Color {
        Color(nsColor: nsColor)
    }

    var environmentHint: String {
        switch self {
        case .red: return "Production"
        case .orange: return "Staging"
        case .green: return "Development"
        case .blue: return "Local"
        case .purple: return "Custom"
        case .gray: return "Other"
        }
    }

    static func fromHex(_ hex: String) -> ColorTag? {
        let h = hex.lowercased().trimmingCharacters(in: CharacterSet(charactersIn: "#"))
        switch h {
        case "ff0000", "e74c4a", "dc3545", "ff4d4d": return .red
        case "ff9500", "ffa500", "f0a028", "ef9f27": return .orange
        case "28a745", "34c759", "4cd964", "63993d": return .green
        case "007aff", "378ae5", "0d6efd", "3a8bdd": return .blue
        case "5856d6", "534ab7", "6f42c1", "534bb8": return .purple
        default: return .gray
        }
    }
}
