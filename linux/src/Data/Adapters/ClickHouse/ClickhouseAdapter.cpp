#include "Data/Adapters/ClickHouse/ClickhouseAdapter.h"

#include <QByteArray>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QString>
#include <QUrl>
#include <QUrlQuery>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "Core/Errors/GridexError.h"
#include "Core/Models/Query/FilterExpression.h"

namespace gridex {

namespace {

std::string trim(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string toUpper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

bool startsWith(const std::string& s, std::string_view prefix) {
    return s.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), s.begin());
}

QueryType detectQueryType(const std::string& upper) {
    if (startsWith(upper, "SELECT") || startsWith(upper, "WITH") ||
        startsWith(upper, "EXPLAIN") || startsWith(upper, "SHOW") ||
        startsWith(upper, "DESCRIBE") || startsWith(upper, "DESC ")) return QueryType::Select;
    if (startsWith(upper, "INSERT")) return QueryType::Insert;
    if (startsWith(upper, "UPDATE") ||
        (startsWith(upper, "ALTER TABLE") && upper.find(" UPDATE ") != std::string::npos))
        return QueryType::Update;
    if (startsWith(upper, "DELETE") ||
        (startsWith(upper, "ALTER TABLE") && upper.find(" DELETE ") != std::string::npos))
        return QueryType::Delete;
    if (startsWith(upper, "CREATE") || startsWith(upper, "ALTER") ||
        startsWith(upper, "DROP")   || startsWith(upper, "TRUNCATE") ||
        startsWith(upper, "RENAME")) return QueryType::DDL;
    return QueryType::Other;
}

std::string parseUseDb(const std::string& sql) {
    // "USE dbname" / "USE `dbname`" / "USE \"dbname\""; trailing ;
    std::string after = sql.substr(3);  // drop "USE"
    after = trim(after);
    while (!after.empty() && (after.back() == ';' || after.back() == ' ')) after.pop_back();
    auto strip = [](std::string s, char ch) {
        if (s.size() >= 2 && s.front() == ch && s.back() == ch) return s.substr(1, s.size() - 2);
        return s;
    };
    after = strip(after, '`');
    after = strip(after, '"');
    after = strip(after, '\'');
    return after;
}

}  // namespace

ClickhouseAdapter::ClickhouseAdapter()
    : nam_(std::make_unique<QNetworkAccessManager>()) {}

ClickhouseAdapter::~ClickhouseAdapter() = default;

bool ClickhouseAdapter::isConnected() const noexcept {
    std::lock_guard lk(stateMu_);
    return connected_;
}

void ClickhouseAdapter::rebuildHttpDefaults() {
    if (!config_) return;
    scheme_   = config_->sslEnabled ? "https" : "http";
    host_     = config_->host.value_or("localhost");
    port_     = config_->port.value_or(config_->sslEnabled ? 8443 : 8123);
    username_ = config_->username.value_or("default");
}

void ClickhouseAdapter::connect(const ConnectionConfig& config,
                                const std::optional<std::string>& password) {
    {
        std::lock_guard lk(stateMu_);
        config_   = config;
        password_ = password;
        currentDb_ = (config.database && !config.database->empty()) ? config.database : std::nullopt;
        rebuildHttpDefaults();
    }

    // Smoke test — fail fast when credentials / host are wrong.
    try {
        (void)postSql("SELECT 1 FORMAT JSONCompact", /*readOnly=*/true);
    } catch (const std::exception& e) {
        std::lock_guard lk(stateMu_);
        connected_ = false;
        throw ConnectionError(std::string("ClickHouse connection failed: ") + e.what());
    }

    {
        std::lock_guard lk(stateMu_);
        connected_ = true;
    }

    // Cache version for `is_in_primary_key` capability check.
    try {
        auto r = executeRaw("SELECT version()");
        if (!r.rows.empty() && !r.rows.front().empty()) {
            serverVersionCache_ = r.rows.front().front().displayString();
        }
    } catch (...) {}
}

void ClickhouseAdapter::disconnect() {
    std::lock_guard lk(stateMu_);
    connected_ = false;
    config_.reset();
    password_.reset();
    currentDb_.reset();
    serverVersionCache_.clear();
}

bool ClickhouseAdapter::testConnection(const ConnectionConfig& config,
                                       const std::optional<std::string>& password) {
    ClickhouseAdapter probe;
    try {
        probe.connect(config, password);
        probe.disconnect();
        return true;
    } catch (...) {
        probe.disconnect();
        throw;
    }
}

ClickhouseAdapter::HttpResponse ClickhouseAdapter::postSql(
    const std::string& sql, bool readOnly,
    const std::optional<std::string>& databaseOverride) {

    QUrl url;
    {
        std::lock_guard lk(stateMu_);
        if (!config_) throw ConnectionError("ClickHouse: not connected");
        url.setScheme(QString::fromStdString(scheme_));
        url.setHost(QString::fromStdString(host_));
        url.setPort(port_);
        url.setPath("/");

        QUrlQuery q;
        std::optional<std::string> db = databaseOverride
            ? databaseOverride
            : (currentDb_ ? currentDb_ : config_->database);
        if (db && !db->empty()) q.addQueryItem("database", QString::fromStdString(*db));
        if (readOnly) q.addQueryItem("readonly", "2");
        url.setQuery(q);
    }

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "text/plain; charset=UTF-8");
    {
        std::lock_guard lk(stateMu_);
        if (!username_.empty()) {
            req.setRawHeader("X-ClickHouse-User", QByteArray::fromStdString(username_));
        }
        if (password_ && !password_->empty()) {
            req.setRawHeader("X-ClickHouse-Key", QByteArray::fromStdString(*password_));
        }
    }
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);

    QEventLoop loop;
    QNetworkReply* reply = nam_->post(req, QByteArray::fromStdString(sql));
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    HttpResponse out;
    out.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QByteArray body = reply->readAll();
    out.body = std::string(body.constData(), body.size());
    QByteArray summary = reply->rawHeader("X-ClickHouse-Summary");
    out.summaryJson = std::string(summary.constData(), summary.size());

    QNetworkReply::NetworkError netErr = reply->error();
    QString errString = reply->errorString();
    reply->deleteLater();

    if (netErr != QNetworkReply::NoError && out.status < 400) {
        throw ConnectionError("ClickHouse network error: " + errString.toStdString());
    }
    if (out.status >= 400) {
        std::string msg = trim(out.body);
        if (msg.empty()) msg = "HTTP " + std::to_string(out.status);
        throw QueryError("ClickHouse: " + msg);
    }
    return out;
}

QueryResult ClickhouseAdapter::execute(const std::string& query,
                                       const std::vector<QueryParameter>&) {
    return executeRaw(query);
}

QueryResult ClickhouseAdapter::executeRaw(const std::string& sql) {
    auto t0 = std::chrono::steady_clock::now();
    std::string trimmed = trim(sql);
    std::string upper = toUpper(trimmed);

    // ClickHouse HTTP is stateless — intercept USE so future queries pick up the new default DB.
    if (startsWith(upper, "USE ")) {
        std::string name = parseUseDb(trimmed);
        if (!name.empty()) {
            std::lock_guard lk(stateMu_);
            currentDb_ = name;
        }
        QueryResult r;
        return r;
    }

    bool isSelectShaped =
        startsWith(upper, "SELECT") || startsWith(upper, "WITH") ||
        startsWith(upper, "EXPLAIN") || startsWith(upper, "SHOW") ||
        startsWith(upper, "DESCRIBE") || startsWith(upper, "DESC ");

    QueryType qType = detectQueryType(upper);

    if (isSelectShaped) {
        std::string wire = trimmed + " FORMAT JSONCompact";
        auto resp = postSql(wire, /*readOnly=*/false);
        double elapsedMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        return parseJsonCompact(resp.body, elapsedMs);
    }

    auto resp = postSql(trimmed);
    QueryResult out;
    // Pull written_rows from the summary header for INSERT/ALTER feedback.
    int writtenRows = 0;
    if (!resp.summaryJson.empty()) {
        try {
            auto j = nlohmann::json::parse(resp.summaryJson);
            if (j.contains("written_rows")) {
                std::string s = j["written_rows"].get<std::string>();
                writtenRows = std::stoi(s);
            }
        } catch (...) {}
    }
    out.rowsAffected = writtenRows;
    (void)qType;
    return out;
}

QueryResult ClickhouseAdapter::parseJsonCompact(const std::string& body, double elapsedMs) {
    QueryResult out;
    if (body.empty()) return out;

    nlohmann::json root;
    try {
        root = nlohmann::json::parse(body);
    } catch (const std::exception&) {
        std::string snippet = body.substr(0, 400);
        throw QueryError("ClickHouse: unexpected response: " + snippet);
    }
    if (!root.is_object() || !root.contains("meta") || !root.contains("data")) return out;

    const auto& meta = root["meta"];
    const auto& data = root["data"];
    if (!meta.is_array() || !data.is_array()) return out;

    out.columns.reserve(meta.size());
    std::vector<std::string> colTypes;
    colTypes.reserve(meta.size());
    for (const auto& m : meta) {
        ColumnHeader h;
        h.name     = m.value("name", "");
        h.dataType = m.value("type", "String");
        h.isNullable = h.dataType.rfind("Nullable", 0) == 0;
        colTypes.push_back(h.dataType);
        out.columns.push_back(std::move(h));
    }

    auto unwrap = [](std::string t) {
        if (t.rfind("Nullable(", 0) == 0 && !t.empty() && t.back() == ')') {
            return t.substr(9, t.size() - 10);
        }
        return t;
    };

    for (const auto& jr : data) {
        if (!jr.is_array()) continue;
        std::vector<RowValue> row;
        row.reserve(jr.size());
        for (std::size_t i = 0; i < jr.size(); ++i) {
            const auto& v = jr[i];
            const std::string& chType = i < colTypes.size() ? colTypes[i] : std::string("String");
            const std::string base = unwrap(chType);

            if (v.is_null()) { row.push_back(RowValue::makeNull()); continue; }

            // CH JSONCompact returns numbers as JSON strings to preserve precision.
            if (base.rfind("UInt", 0) == 0 || base.rfind("Int", 0) == 0) {
                try {
                    if (v.is_string()) row.push_back(RowValue::makeInteger(std::stoll(v.get<std::string>())));
                    else if (v.is_number_integer()) row.push_back(RowValue::makeInteger(v.get<std::int64_t>()));
                    else row.push_back(RowValue::makeNull());
                } catch (...) {
                    row.push_back(RowValue::makeString(v.is_string() ? v.get<std::string>() : v.dump()));
                }
                continue;
            }
            if (base.rfind("Float", 0) == 0) {
                try {
                    if (v.is_string()) row.push_back(RowValue::makeDouble(std::stod(v.get<std::string>())));
                    else if (v.is_number()) row.push_back(RowValue::makeDouble(v.get<double>()));
                    else row.push_back(RowValue::makeNull());
                } catch (...) { row.push_back(RowValue::makeNull()); }
                continue;
            }
            if (base == "Bool" || base == "Boolean") {
                if (v.is_boolean())     row.push_back(RowValue::makeBoolean(v.get<bool>()));
                else if (v.is_number()) row.push_back(RowValue::makeBoolean(v.get<int>() != 0));
                else if (v.is_string()) {
                    auto s = v.get<std::string>();
                    row.push_back(RowValue::makeBoolean(s == "true" || s == "1"));
                } else row.push_back(RowValue::makeNull());
                continue;
            }
            if (base == "UUID") {
                row.push_back(RowValue::makeUuid(v.is_string() ? v.get<std::string>() : v.dump()));
                continue;
            }
            if (base.rfind("Map(", 0) == 0 || base.rfind("Tuple(", 0) == 0
                || base == "JSON" || base == "Object('json')"
                || base.rfind("Array(", 0) == 0) {
                row.push_back(RowValue::makeJson(v.dump()));
                continue;
            }
            // String / FixedString / Date* / Decimal* / Enum / fallback
            if (v.is_string()) row.push_back(RowValue::makeString(v.get<std::string>()));
            else if (v.is_number_integer()) row.push_back(RowValue::makeInteger(v.get<std::int64_t>()));
            else if (v.is_number()) row.push_back(RowValue::makeDouble(v.get<double>()));
            else if (v.is_boolean()) row.push_back(RowValue::makeBoolean(v.get<bool>()));
            else row.push_back(RowValue::makeJson(v.dump()));
        }
        out.rows.push_back(std::move(row));
    }
    (void)elapsedMs;
    return out;
}

// ---------- Schema inspection ----------

std::vector<std::string> ClickhouseAdapter::listDatabases() {
    auto r = executeRaw("SELECT name FROM system.databases ORDER BY name");
    std::vector<std::string> out;
    out.reserve(r.rows.size());
    for (const auto& row : r.rows) {
        if (!row.empty() && row.front().isString()) out.push_back(row.front().asString());
    }
    return out;
}

std::vector<std::string> ClickhouseAdapter::listSchemas(const std::optional<std::string>&) {
    // ClickHouse is flat — databases ARE the schema boundary. Mirror MySQL's
    // behaviour: return [currentDatabase] so the sidebar shows a single
    // expandable node containing the tables of the active database.
    if (auto cur = currentDatabase()) return {*cur};
    return {};
}

std::vector<TableInfo> ClickhouseAdapter::listTables(const std::optional<std::string>& schema) {
    std::string db = resolveDb(schema);
    std::string sql =
        "SELECT name, total_rows FROM system.tables WHERE database = '" + escapeLiteral(db)
        + "' AND engine NOT LIKE '%View' ORDER BY name";
    auto r = executeRaw(sql);
    std::vector<TableInfo> out;
    out.reserve(r.rows.size());
    for (const auto& row : r.rows) {
        if (row.empty() || !row.front().isString()) continue;
        TableInfo t;
        t.name = row.front().asString();
        t.schema = db;
        t.type = TableKind::Table;
        if (row.size() > 1 && row[1].isInteger()) t.estimatedRowCount = static_cast<int>(row[1].asInteger());
        out.push_back(std::move(t));
    }
    return out;
}

std::vector<ViewInfo> ClickhouseAdapter::listViews(const std::optional<std::string>& schema) {
    std::string db = resolveDb(schema);
    std::string sql =
        "SELECT name, as_select, engine FROM system.tables WHERE database = '" + escapeLiteral(db)
        + "' AND engine LIKE '%View' ORDER BY name";
    auto r = executeRaw(sql);
    std::vector<ViewInfo> out;
    out.reserve(r.rows.size());
    for (const auto& row : r.rows) {
        if (row.empty() || !row.front().isString()) continue;
        ViewInfo v;
        v.name = row.front().asString();
        v.schema = db;
        if (row.size() > 1 && row[1].isString()) v.definition = row[1].asString();
        if (row.size() > 2 && row[2].isString()) v.isMaterialized = (row[2].asString() == "MaterializedView");
        out.push_back(std::move(v));
    }
    return out;
}

bool ClickhouseAdapter::serverSupportsIsInPrimaryKey() const {
    if (serverVersionCache_.empty()) return true;  // optimistic
    int major = 0, minor = 0;
    std::sscanf(serverVersionCache_.c_str(), "%d.%d", &major, &minor);
    if (major > 21) return true;
    if (major == 21 && minor >= 3) return true;
    return false;
}

TableDescription ClickhouseAdapter::describeTable(const std::string& name,
                                                  const std::optional<std::string>& schema) {
    std::string db = resolveDb(schema);
    bool hasPK = serverSupportsIsInPrimaryKey();

    std::string colSelect = hasPK
        ? "name, type, default_kind, default_expression, comment, is_in_primary_key, position"
        : "name, type, default_kind, default_expression, comment, 0 AS is_in_primary_key, position";
    auto r = executeRaw(
        "SELECT " + colSelect + " FROM system.columns WHERE database = '" + escapeLiteral(db)
        + "' AND table = '" + escapeLiteral(name) + "' ORDER BY position");

    TableDescription desc;
    desc.name = name;
    desc.schema = db;
    desc.columns.reserve(r.rows.size());
    int ord = 1;
    for (const auto& row : r.rows) {
        if (row.size() < 7) continue;
        ColumnInfo c;
        c.name             = row[0].isString()  ? row[0].asString()  : "";
        c.dataType         = row[1].isString()  ? row[1].asString()  : "String";
        c.isNullable       = c.dataType.rfind("Nullable", 0) == 0;
        std::string defKind = row[2].isString() ? row[2].asString() : "";
        std::string defExpr = row[3].isString() ? row[3].asString() : "";
        if (!defExpr.empty()) {
            c.defaultValue = defKind.empty() ? defExpr : (defKind + " " + defExpr);
        }
        if (row[4].isString() && !row[4].asString().empty()) c.comment = row[4].asString();
        c.isPrimaryKey = row[5].isInteger() && row[5].asInteger() == 1;
        c.ordinalPosition = ord++;
        desc.columns.push_back(std::move(c));
    }

    desc.indexes = listIndexes(name, db);

    try {
        auto meta = executeRaw(
            "SELECT total_rows, comment FROM system.tables WHERE database = '"
            + escapeLiteral(db) + "' AND name = '" + escapeLiteral(name) + "'");
        if (!meta.rows.empty()) {
            const auto& row = meta.rows.front();
            if (row.size() > 0 && row[0].isInteger())
                desc.estimatedRowCount = static_cast<int>(row[0].asInteger());
            if (row.size() > 1 && row[1].isString() && !row[1].asString().empty())
                desc.comment = row[1].asString();
        }
    } catch (...) {}

    return desc;
}

std::vector<IndexInfo> ClickhouseAdapter::listIndexes(const std::string& table,
                                                      const std::optional<std::string>& schema) {
    std::string db = resolveDb(schema);
    try {
        auto r = executeRaw(
            "SELECT name, expr, type FROM system.data_skipping_indices WHERE database = '"
            + escapeLiteral(db) + "' AND table = '" + escapeLiteral(table) + "' ORDER BY name");
        std::vector<IndexInfo> out;
        for (const auto& row : r.rows) {
            if (row.empty() || !row.front().isString()) continue;
            IndexInfo i;
            i.name = row.front().asString();
            if (row.size() > 1 && row[1].isString()) i.columns.push_back(row[1].asString());
            if (row.size() > 2 && row[2].isString()) i.type = row[2].asString();
            i.tableName = table;
            i.isUnique = false;
            out.push_back(std::move(i));
        }
        return out;
    } catch (...) {
        return {};
    }
}

std::vector<ForeignKeyInfo> ClickhouseAdapter::listForeignKeys(const std::string&,
                                                               const std::optional<std::string>&) {
    return {};  // ClickHouse has no foreign keys.
}

std::vector<std::string> ClickhouseAdapter::listFunctions(const std::optional<std::string>&) {
    try {
        auto r = executeRaw(
            "SELECT name FROM system.functions WHERE is_aggregate = 0 "
            "AND origin = 'SQLUserDefined' ORDER BY name");
        std::vector<std::string> out;
        for (const auto& row : r.rows) {
            if (!row.empty() && row.front().isString()) out.push_back(row.front().asString());
        }
        return out;
    } catch (...) {
        return {};
    }
}

std::string ClickhouseAdapter::getFunctionSource(const std::string& name,
                                                 const std::optional<std::string>&) {
    auto r = executeRaw(
        "SELECT create_query FROM system.functions WHERE name = '" + escapeLiteral(name) + "'");
    if (!r.rows.empty() && !r.rows.front().empty() && r.rows.front().front().isString()) {
        return r.rows.front().front().asString();
    }
    return {};
}

// ---------- DML ----------

QueryResult ClickhouseAdapter::insertRow(const std::string& table,
                                         const std::optional<std::string>& schema,
                                         const std::unordered_map<std::string, RowValue>& values) {
    std::string db = resolveDb(schema);
    std::string qualified = qualifiedName(db, table);
    std::vector<std::string> keys;
    keys.reserve(values.size());
    for (const auto& [k, _] : values) keys.push_back(k);
    std::sort(keys.begin(), keys.end());

    std::string cols, vals;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (i > 0) { cols += ", "; vals += ", "; }
        cols += quoteIdentifier(SQLDialect::ClickHouse, keys[i]);
        vals += inlineValue(values.at(keys[i]));
    }
    return executeRaw("INSERT INTO " + qualified + " (" + cols + ") VALUES (" + vals + ")");
}

void ClickhouseAdapter::ensureMutable(const std::string& db, const std::string& table,
                                      const std::string& op) {
    try {
        auto r = executeRaw(
            "SELECT engine FROM system.tables WHERE database = '" + escapeLiteral(db)
            + "' AND name = '" + escapeLiteral(table) + "'");
        if (r.rows.empty() || r.rows.front().empty() || !r.rows.front().front().isString()) return;
        std::string engine = r.rows.front().front().asString();
        if (engine.find("MergeTree") != std::string::npos) return;
        throw QueryError(
            "ClickHouse: cannot " + op + " rows in `" + db + "`.`" + table
            + "` — engine `" + engine + "` does not support row mutations. "
            + "Only MergeTree-family engines accept ALTER TABLE ... " + op + ".");
    } catch (const QueryError&) {
        throw;
    } catch (...) {
        // Failed to inspect engine — let the server decide.
    }
}

QueryResult ClickhouseAdapter::updateRow(const std::string& table,
                                         const std::optional<std::string>& schema,
                                         const std::unordered_map<std::string, RowValue>& set,
                                         const std::unordered_map<std::string, RowValue>& where) {
    std::string db = resolveDb(schema);
    ensureMutable(db, table, "UPDATE");
    std::string qualified = qualifiedName(db, table);
    std::string setClause, whereClause;
    bool first = true;
    for (const auto& [k, v] : set) {
        if (!first) setClause += ", ";
        first = false;
        setClause += quoteIdentifier(SQLDialect::ClickHouse, k) + " = " + inlineValue(v);
    }
    first = true;
    for (const auto& [k, v] : where) {
        if (!first) whereClause += " AND ";
        first = false;
        whereClause += quoteIdentifier(SQLDialect::ClickHouse, k) + " = " + inlineValue(v);
    }
    return executeRaw("ALTER TABLE " + qualified + " UPDATE " + setClause + " WHERE " + whereClause);
}

QueryResult ClickhouseAdapter::deleteRow(const std::string& table,
                                         const std::optional<std::string>& schema,
                                         const std::unordered_map<std::string, RowValue>& where) {
    std::string db = resolveDb(schema);
    ensureMutable(db, table, "DELETE");
    std::string qualified = qualifiedName(db, table);
    std::string whereClause;
    bool first = true;
    for (const auto& [k, v] : where) {
        if (!first) whereClause += " AND ";
        first = false;
        whereClause += quoteIdentifier(SQLDialect::ClickHouse, k) + " = " + inlineValue(v);
    }
    return executeRaw("ALTER TABLE " + qualified + " DELETE WHERE " + whereClause);
}

// ---------- Pagination ----------

QueryResult ClickhouseAdapter::fetchRows(
    const std::string& table,
    const std::optional<std::string>& schema,
    const std::optional<std::vector<std::string>>& columns,
    const std::optional<FilterExpression>& /*where*/,
    const std::optional<std::vector<QuerySortDescriptor>>& /*orderBy*/,
    int limit, int offset) {
    std::string db = resolveDb(schema);
    std::string qualified = qualifiedName(db, table);
    std::string colList = "*";
    if (columns && !columns->empty()) {
        colList.clear();
        for (std::size_t i = 0; i < columns->size(); ++i) {
            if (i > 0) colList += ", ";
            colList += quoteIdentifier(SQLDialect::ClickHouse, (*columns)[i]);
        }
    }
    std::string sql = "SELECT " + colList + " FROM " + qualified
                    + " LIMIT " + std::to_string(limit) + " OFFSET " + std::to_string(offset);
    return executeRaw(sql);
}

// ---------- Server info ----------

std::string ClickhouseAdapter::serverVersion() {
    if (!serverVersionCache_.empty()) return serverVersionCache_;
    auto r = executeRaw("SELECT version()");
    if (!r.rows.empty() && !r.rows.front().empty() && r.rows.front().front().isString()) {
        serverVersionCache_ = r.rows.front().front().asString();
    }
    return serverVersionCache_.empty() ? std::string("ClickHouse") : serverVersionCache_;
}

std::optional<std::string> ClickhouseAdapter::currentDatabase() {
    {
        std::lock_guard lk(stateMu_);
        if (currentDb_ && !currentDb_->empty()) return currentDb_;
    }
    auto r = executeRaw("SELECT currentDatabase()");
    if (!r.rows.empty() && !r.rows.front().empty() && r.rows.front().front().isString()) {
        std::string s = r.rows.front().front().asString();
        if (!s.empty()) {
            std::lock_guard lk(stateMu_);
            currentDb_ = s;
            return s;
        }
    }
    return std::nullopt;
}

// ---------- Helpers ----------

std::string ClickhouseAdapter::resolveDb(const std::optional<std::string>& schema) const {
    if (schema && !schema->empty()) return *schema;
    std::lock_guard lk(stateMu_);
    if (currentDb_ && !currentDb_->empty()) return *currentDb_;
    return "default";
}

std::string ClickhouseAdapter::qualifiedName(const std::string& db, const std::string& table) const {
    return quoteIdentifier(SQLDialect::ClickHouse, db) + "." + quoteIdentifier(SQLDialect::ClickHouse, table);
}

std::string ClickhouseAdapter::escapeLiteral(const std::string& s) const {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '\'') out += "\\'";
        else out.push_back(c);
    }
    return out;
}

std::string ClickhouseAdapter::inlineValue(const RowValue& v) const {
    if (v.isNull())    return "NULL";
    if (v.isString())  return "'" + escapeLiteral(v.asString()) + "'";
    if (v.isInteger()) return std::to_string(v.asInteger());
    if (v.isDouble())  return std::to_string(v.asDouble());
    if (v.isBoolean()) return v.asBoolean() ? "1" : "0";
    if (v.isJson())    return "'" + escapeLiteral(v.asJson()) + "'";
    if (v.isUuid())    return "'" + escapeLiteral(v.asUuid()) + "'";
    return "NULL";
}

}  // namespace gridex
