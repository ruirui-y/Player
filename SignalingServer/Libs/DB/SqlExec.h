#ifndef SQL_EXEC_H
#define SQL_EXEC_H

#include <sqlite3.h>
#include <string>
#include <vector>
#include <map>
#include <variant>
#include <memory>

// SQLite 查询结果行
using SqlRow = std::map<std::string, std::string>;
using SqlResult = std::vector<SqlRow>;

// SQLite 执行器（每个 Worker 线程一个实例，无锁）
class SqlExec
{
public:
    SqlExec();
    ~SqlExec();

    bool Init(const std::string& db_path);

    // 执行非查询语句（INSERT/UPDATE/DELETE/CREATE）
    // 返回受影响的行数，失败返回 -1
    int Execute(const std::string& sql);

    // 执行查询语句（SELECT）
    // 返回查询结果，失败返回空 vector
    SqlResult Query(const std::string& sql);

    // 获取最后一次插入的 rowid
    int64_t LastInsertRowId() const;

private:
    sqlite3* db_{ nullptr };
};

#endif // SQL_EXEC_H
