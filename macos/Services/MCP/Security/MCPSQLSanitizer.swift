// MCPSQLSanitizer.swift
// Gridex
//
// Normalizes SQL strings for security checks — NOT for execution.
// Strips line comments, block comments, and single-quoted string literals so
// syntactic checks (e.g. "does this contain WHERE?") operate on code-only
// content rather than being fooled by payloads hidden inside comments or
// literals.

import Foundation

enum MCPSQLSanitizer {
    static func stripCommentsAndStrings(_ sql: String) -> String {
        var result = ""
        result.reserveCapacity(sql.count)
        var i = sql.startIndex
        let end = sql.endIndex

        while i < end {
            let c = sql[i]
            let next = sql.index(after: i)

            // -- line comment
            if c == "-", next < end, sql[next] == "-" {
                while i < end, sql[i] != "\n" {
                    i = sql.index(after: i)
                }
                continue
            }

            // /* block comment */
            if c == "/", next < end, sql[next] == "*" {
                i = sql.index(after: next)
                while i < end {
                    let j = sql.index(after: i)
                    if sql[i] == "*", j < end, sql[j] == "/" {
                        i = sql.index(after: j)
                        break
                    }
                    i = sql.index(after: i)
                }
                continue
            }

            // 'single-quoted literal'
            if c == "'" {
                i = sql.index(after: i)
                while i < end {
                    if sql[i] == "'" {
                        let j = sql.index(after: i)
                        if j < end, sql[j] == "'" {
                            // Escaped '' inside literal
                            i = sql.index(after: j)
                            continue
                        }
                        i = sql.index(after: i)
                        break
                    }
                    i = sql.index(after: i)
                }
                result.append(" ")
                continue
            }

            result.append(c)
            i = sql.index(after: i)
        }
        return result
    }
}
