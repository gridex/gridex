#pragma once
#include <string>
#include <vector>
#include <set>
#include <algorithm>
#include "TableRow.h"
#include "DatabaseType.h"

namespace DBModels
{
    enum class EditType { Insert, Update, Delete };

    struct PendingEdit
    {
        EditType type;
        int rowIndex = -1;          // -1 for inserts (new rows)
        std::wstring column;        // for Update only
        std::wstring oldValue;      // for Update only
        std::wstring newValue;      // for Update only
        TableRow primaryKey;        // WHERE clause for update/delete
        TableRow insertValues;      // full row for inserts
    };

    // Tracks pending INSERT/UPDATE/DELETE changes before commit
    class ChangeTracker
    {
    public:
        void trackInsert(const TableRow& values, int rowIndex = -1)
        {
            PendingEdit edit;
            edit.type = EditType::Insert;
            edit.rowIndex = rowIndex;
            edit.insertValues = values;
            edits_.push_back(edit);
        }

        void trackUpdate(int rowIndex, const std::wstring& column,
                         const std::wstring& oldValue, const std::wstring& newValue,
                         const TableRow& primaryKey)
        {
            // If reverting to original, skip
            if (oldValue == newValue) return;

            // If this row is a pending INSERT, update the insert values directly
            // (no separate UPDATE needed — just modify the INSERT data)
            for (auto& e : edits_)
            {
                if (e.type == EditType::Insert && e.rowIndex == rowIndex)
                {
                    if (!isNullCell(newValue) && !newValue.empty())
                        e.insertValues[column] = newValue;
                    else
                        e.insertValues.erase(column); // NULL = omit from INSERT
                    return;
                }
            }

            // Merge: overwrite existing UPDATE for same row+column
            for (auto& e : edits_)
            {
                if (e.type == EditType::Update && e.rowIndex == rowIndex && e.column == column)
                {
                    if (newValue == e.oldValue)
                    {
                        // Reverted to original — remove edit
                        edits_.erase(std::remove_if(edits_.begin(), edits_.end(),
                            [&](const PendingEdit& x) { return &x == &e; }), edits_.end());
                    }
                    else
                    {
                        e.newValue = newValue;
                    }
                    return;
                }
            }

            PendingEdit edit;
            edit.type = EditType::Update;
            edit.rowIndex = rowIndex;
            edit.column = column;
            edit.oldValue = oldValue;
            edit.newValue = newValue;
            edit.primaryKey = primaryKey;
            edits_.push_back(edit);
        }

        void trackDelete(int rowIndex, const TableRow& primaryKey)
        {
            // Remove any pending updates for this row
            edits_.erase(std::remove_if(edits_.begin(), edits_.end(),
                [rowIndex](const PendingEdit& e) {
                    return e.type == EditType::Update && e.rowIndex == rowIndex;
                }), edits_.end());

            PendingEdit edit;
            edit.type = EditType::Delete;
            edit.rowIndex = rowIndex;
            edit.primaryKey = primaryKey;
            edits_.push_back(edit);
        }

        void discardAll()
        {
            edits_.clear();
        }

        bool hasChanges() const { return !edits_.empty(); }
        int changeCount() const { return static_cast<int>(edits_.size()); }

        const std::vector<PendingEdit>& pendingEdits() const { return edits_; }

        std::set<int> deletedRowIndices() const
        {
            std::set<int> indices;
            for (auto& e : edits_)
                if (e.type == EditType::Delete && e.rowIndex >= 0)
                    indices.insert(e.rowIndex);
            return indices;
        }

        // Generate SQL statements for all pending changes
        std::vector<std::wstring> generateSQL(
            const std::wstring& table,
            const std::wstring& schema,
            DatabaseType dbType) const
        {
            std::vector<std::wstring> statements;

            for (auto& edit : edits_)
            {
                switch (edit.type)
                {
                case EditType::Insert:
                    statements.push_back(generateInsert(table, schema, edit, dbType));
                    break;
                case EditType::Update:
                    statements.push_back(generateUpdate(table, schema, edit, dbType));
                    break;
                case EditType::Delete:
                    statements.push_back(generateDelete(table, schema, edit, dbType));
                    break;
                }
            }
            return statements;
        }

    private:
        std::vector<PendingEdit> edits_;

        static std::wstring quoteId(const std::wstring& name, DatabaseType dbType)
        {
            if (dbType == DatabaseType::MySQL)
                return L"`" + name + L"`";
            return L"\"" + name + L"\"";  // PostgreSQL, SQLite
        }

        static std::wstring quoteLit(const std::wstring& value)
        {
            if (isNullCell(value)) return L"NULL";
            std::wstring escaped;
            escaped += L'\'';
            for (wchar_t c : value)
            {
                if (c == L'\'') escaped += L"''";
                else escaped += c;
            }
            escaped += L'\'';
            return escaped;
        }

        static std::wstring qualifiedTable(const std::wstring& table,
                                           const std::wstring& schema,
                                           DatabaseType dbType)
        {
            if (dbType == DatabaseType::SQLite || schema.empty())
                return quoteId(table, dbType);
            return quoteId(schema, dbType) + L"." + quoteId(table, dbType);
        }

        static std::wstring buildWhere(const TableRow& pk, DatabaseType dbType)
        {
            std::wstring where;
            bool first = true;
            for (auto& [col, val] : pk)
            {
                if (!first) where += L" AND ";
                where += quoteId(col, dbType);
                if (isNullCell(val))
                    where += L" IS NULL";
                else
                    where += L" = " + quoteLit(val);
                first = false;
            }
            return where;
        }

        static std::wstring generateInsert(const std::wstring& table,
                                           const std::wstring& schema,
                                           const PendingEdit& edit,
                                           DatabaseType dbType)
        {
            std::wstring sql = L"INSERT INTO " + qualifiedTable(table, schema, dbType) + L" (";
            std::wstring vals;
            bool first = true;
            for (auto& [col, val] : edit.insertValues)
            {
                // Blank cell (user left it empty in the new-row UI) →
                // omit the column so AUTO_INCREMENT / SERIAL / column
                // DEFAULT fire. Otherwise we emit '' which bombs on INT
                // columns and fights auto-increment. Explicit SQL NULL
                // still travels via the null sentinel (quoteLit → NULL).
                if (!isNullCell(val) && val.empty()) continue;
                if (!first) { sql += L", "; vals += L", "; }
                sql += quoteId(col, dbType);
                vals += quoteLit(val);
                first = false;
            }
            // All columns blank — use each dialect's all-defaults insert
            // form so we don't produce invalid "INSERT INTO t () VALUES()".
            // MySQL accepts "() VALUES ()"; PostgreSQL/SQLite/MSSQL want
            // "DEFAULT VALUES".
            if (first)
            {
                if (dbType == DatabaseType::MySQL)
                    return L"INSERT INTO " + qualifiedTable(table, schema, dbType) +
                           L" () VALUES ()";
                return L"INSERT INTO " + qualifiedTable(table, schema, dbType) +
                       L" DEFAULT VALUES";
            }
            sql += L") VALUES (" + vals + L")";
            return sql;
        }

        static std::wstring generateUpdate(const std::wstring& table,
                                           const std::wstring& schema,
                                           const PendingEdit& edit,
                                           DatabaseType dbType)
        {
            std::wstring sql = L"UPDATE " + qualifiedTable(table, schema, dbType) + L" SET ";
            sql += quoteId(edit.column, dbType) + L" = " + quoteLit(edit.newValue);
            sql += L" WHERE " + buildWhere(edit.primaryKey, dbType);
            return sql;
        }

        static std::wstring generateDelete(const std::wstring& table,
                                           const std::wstring& schema,
                                           const PendingEdit& edit,
                                           DatabaseType dbType)
        {
            std::wstring sql = L"DELETE FROM " + qualifiedTable(table, schema, dbType);
            sql += L" WHERE " + buildWhere(edit.primaryKey, dbType);
            return sql;
        }
    };
}
