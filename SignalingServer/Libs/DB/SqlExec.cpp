#include "SqlExec.h"
#include "Global/LogManager.h"

SqlExec::SqlExec()
{
}

SqlExec::~SqlExec()
{
    if (db_)
    {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool SqlExec::Init(const std::string& db_path)
{
    int rc = sqlite3_open(db_path.c_str(), &db_);
    if (rc != SQLITE_OK)
    {
        LOG_ERROR("db", "[SqlExec] 打开数据库失败: {}", sqlite3_errmsg(db_));
        return false;
    }

    // 启用 WAL 模式（并发读性能更好）
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA busy_timeout=5000;",  nullptr, nullptr, nullptr);

    // 建表
    const char* create_devices = R"(
        CREATE TABLE IF NOT EXISTS devices (
            id          TEXT PRIMARY KEY,
            name        TEXT NOT NULL,
            public_ip   TEXT DEFAULT '',
            local_ip    TEXT DEFAULT '',
            status      INTEGER DEFAULT 1,
            last_seen   INTEGER DEFAULT 0
        );
    )";
    Execute(create_devices);

    LOG_INFO("db", "[SqlExec] SQLite 初始化完成: {}", db_path);
    return true;
}

int SqlExec::Execute(const std::string& sql)
{
    if (!db_) return -1;

    char* err_msg = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK)
    {
        LOG_ERROR("db", "[SqlExec] Execute 失败: {} | SQL: {}", err_msg, sql);
        sqlite3_free(err_msg);
        return -1;
    }

    return sqlite3_changes(db_);
}

SqlResult SqlExec::Query(const std::string& sql)
{
    SqlResult result;
    if (!db_) return result;

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK)
    {
        LOG_ERROR("db", "[SqlExec] Query prepare 失败: {} | SQL: {}",
                  sqlite3_errmsg(db_), sql);
        return result;
    }

    int col_count = sqlite3_column_count(stmt);
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        SqlRow row;
        for (int i = 0; i < col_count; ++i)
        {
            const char* col_name = sqlite3_column_name(stmt, i);
            const char* col_text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, i));
            row[col_name] = col_text ? col_text : "";
        }
        result.push_back(std::move(row));
    }

    if (rc != SQLITE_DONE && rc != SQLITE_ROW)
    {
        LOG_ERROR("db", "[SqlExec] Query step 失败: {}", sqlite3_errmsg(db_));
    }

    sqlite3_finalize(stmt);
    return result;
}

int64_t SqlExec::LastInsertRowId() const
{
    return db_ ? sqlite3_last_insert_rowid(db_) : -1;
}
