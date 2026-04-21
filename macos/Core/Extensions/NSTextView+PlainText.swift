// NSTextView+PlainText.swift
// Gridex
//
// Helpers for code-style text input.

import AppKit

extension NSTextView {
    /// Turn off every auto-substitution macOS applies by default. Essential for
    /// code/JSON/SQL editors — smart quotes silently corrupt strings, and
    /// spell/grammar underlines are pure noise on syntactic text.
    func disableAutoSubstitutions() {
        isAutomaticQuoteSubstitutionEnabled = false
        isAutomaticDashSubstitutionEnabled = false
        isAutomaticTextReplacementEnabled = false
        isAutomaticSpellingCorrectionEnabled = false
        isContinuousSpellCheckingEnabled = false
        isGrammarCheckingEnabled = false
        isAutomaticDataDetectionEnabled = false
        isAutomaticLinkDetectionEnabled = false
    }
}
