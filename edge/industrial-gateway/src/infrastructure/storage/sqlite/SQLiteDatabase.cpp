
#include "SQLiteDatabase.hpp"

#include <sqlite3.h>

#include <limits>
#include <memory>
#include <string>
#include <utility>


namespace industrial_gateway::infrastructure::storage::sqlite
{

namespace
{

// ==================================================
// SQLite Statement 智能指针
// ==================================================

using SQLiteStatement = std::unique_ptr<
    sqlite3_stmt,
    decltype(&sqlite3_finalize)
>;


// ==================================================
// 获取 SQLite 错误信息
// ==================================================

std::string getSQLiteError(
    sqlite3* database,
    int result_code
)
{
    const char* message = nullptr;

    if (database != nullptr)
    {
        message = sqlite3_errmsg(database);
    }
    else
    {
        message = sqlite3_errstr(result_code);
    }

    return message != nullptr
        ? message
        : "未知 SQLite 错误";
}


// ==================================================
// 执行 SQLite SQL
// ==================================================

bool executeSQLiteSql(
    sqlite3* database,
    const std::string& sql,
    const std::string& operation,
    std::string& error_message
)
{
    char* sqlite_error = nullptr;

    const int result_code = sqlite3_exec(
        database,
        sql.c_str(),
        nullptr,
        nullptr,
        &sqlite_error
    );

    if (result_code != SQLITE_OK)
    {
        const std::string detail =
            sqlite_error != nullptr
                ? sqlite_error
                : getSQLiteError(database, result_code);

        error_message =
            operation + "失败: " + detail;

        sqlite3_free(sqlite_error);

        return false;
    }

    sqlite3_free(sqlite_error);

    return true;
}


// ==================================================
// 验证 PRAGMA 文本结果
// ==================================================

bool verifyPragmaText(
    sqlite3* database,
    const std::string& sql,
    const std::string& expected_value,
    const std::string& operation,
    std::string& error_message
)
{
    sqlite3_stmt* raw_statement = nullptr;

    const int prepare_code = sqlite3_prepare_v2(
        database,
        sql.c_str(),
        -1,
        &raw_statement,
        nullptr
    );

    SQLiteStatement statement(
        raw_statement,
        sqlite3_finalize
    );

    if (prepare_code != SQLITE_OK)
    {
        error_message =
            operation + "失败: " +
            getSQLiteError(database, prepare_code);

        return false;
    }

    if (statement == nullptr)
    {
        error_message = operation + "失败: PRAGMA 未返回结果";
        return false;
    }

    const int step_code = sqlite3_step(statement.get());

    if (step_code != SQLITE_ROW)
    {
        error_message =
            operation + "失败: " +
            getSQLiteError(database, step_code);

        return false;
    }

    const auto* value = sqlite3_column_text(
        statement.get(),
        0
    );

    if (value == nullptr)
    {
        error_message = operation + "失败: 无法读取 PRAGMA 返回值";
        return false;
    }

    const std::string actual_value(
        reinterpret_cast<const char*>(value)
    );

    if (sqlite3_stricmp(
            actual_value.c_str(),
            expected_value.c_str()) != 0)
    {
        error_message =
            operation + "失败: 预期为 " + expected_value +
            "，实际为 " + actual_value;

        return false;
    }

    return true;
}


// ==================================================
// 预编译 SQL
// ==================================================

bool prepareStatement(
    sqlite3* database,
    const std::string& sql,
    SQLiteStatement& statement,
    std::string& error_message
)
{
    if (sql.empty())
    {
        error_message = "SQL 语句不能为空";
        return false;
    }

    sqlite3_stmt* raw_statement = nullptr;

    const char* tail = nullptr;

    const int result_code = sqlite3_prepare_v2(
        database,
        sql.c_str(),
        -1,
        &raw_statement,
        &tail
    );

    // 从拿到原始 Statement 指针开始就交给 RAII 管理，
    // 确保后续任何失败路径都能释放资源。
    SQLiteStatement prepared_statement(
        raw_statement,
        sqlite3_finalize
    );

    if (result_code != SQLITE_OK)
    {
        error_message =
            "SQL 预编译失败: " +
            getSQLiteError(database, result_code);

        return false;
    }

    if (prepared_statement == nullptr)
    {
        error_message = "SQL 语句无效或为空";
        return false;
    }

    // 检查是否包含多条 SQL 语句
    if (tail != nullptr && *tail != '\0')
    {
        sqlite3_stmt* raw_trailing_statement = nullptr;

        const int trailing_code = sqlite3_prepare_v2(
            database,
            tail,
            -1,
            &raw_trailing_statement,
            nullptr
        );

        SQLiteStatement trailing_statement(
            raw_trailing_statement,
            sqlite3_finalize
        );

        if (trailing_code != SQLITE_OK)
        {
            error_message =
                "SQL 尾部内容解析失败: " +
                getSQLiteError(database, trailing_code);

            return false;
        }

        if (trailing_statement != nullptr)
        {
            error_message =
                "不允许在参数化 SQL 中执行多条 SQL 语句";

            return false;
        }
    }

    statement = std::move(prepared_statement);

    return true;
}


// ==================================================
// 绑定单个 SQL 参数
// ==================================================

int bindValue(
    sqlite3_stmt* statement,
    int index,
    const SQLiteValue& value
)
{
    // NULL
    if (std::holds_alternative<std::monostate>(value))
    {
        return sqlite3_bind_null(statement, index);
    }

    // int64
    if (const auto* integer =
        std::get_if<std::int64_t>(&value))
    {
        return sqlite3_bind_int64(
            statement,
            index,
            *integer
        );
    }

    // double
    if (const auto* floating =
        std::get_if<double>(&value))
    {
        return sqlite3_bind_double(
            statement,
            index,
            *floating
        );
    }

    // string
    if (const auto* text =
        std::get_if<std::string>(&value))
    {
        return sqlite3_bind_text64(
            statement,
            index,
            text->data(),
            static_cast<sqlite3_uint64>(text->size()),
            SQLITE_TRANSIENT,
            SQLITE_UTF8
        );
    }

    // blob
    if (const auto* blob =
        std::get_if<SQLiteBlob>(&value))
    {
        // 空 Blob 也必须保持为 Blob，而不是 NULL
        static const std::uint8_t empty_blob = 0;

        const void* data = blob->empty()
            ? static_cast<const void*>(&empty_blob)
            : static_cast<const void*>(blob->data());

        return sqlite3_bind_blob64(
            statement,
            index,
            data,
            static_cast<sqlite3_uint64>(blob->size()),
            SQLITE_TRANSIENT
        );
    }

    return SQLITE_MISUSE;
}


// ==================================================
// 绑定全部 SQL 参数
// ==================================================

bool bindParameters(
    sqlite3_stmt* statement,
    const std::vector<SQLiteValue>& params,
    sqlite3* database,
    std::string& error_message
)
{
    const int expected_count =
        sqlite3_bind_parameter_count(statement);

    if (params.size() !=
        static_cast<std::size_t>(expected_count))
    {
        error_message =
            "SQL 参数数量不匹配，预期: " +
            std::to_string(expected_count) +
            "，实际: " +
            std::to_string(params.size());

        return false;
    }

    for (std::size_t i = 0; i < params.size(); ++i)
    {
        const int result_code = bindValue(
            statement,
            static_cast<int>(i + 1),
            params[i]
        );

        if (result_code != SQLITE_OK)
        {
            error_message =
                "SQL 参数绑定失败: " +
                getSQLiteError(database, result_code);

            return false;
        }
    }

    return true;
}


// ==================================================
// 执行预编译 SQL，直到执行完成
// ==================================================

bool stepStatement(
    sqlite3_stmt* statement,
    sqlite3* database,
    std::string& error_message
)
{
    int result_code = sqlite3_step(statement);

    // 忽略返回数据的语句，直到执行结束
    while (result_code == SQLITE_ROW)
    {
        result_code = sqlite3_step(statement);
    }

    if (result_code != SQLITE_DONE)
    {
        error_message =
            "SQL 执行失败: " +
            getSQLiteError(database, result_code);

        return false;
    }

    return true;
}


// ==================================================
// 读取 SQLite 查询列
// ==================================================

bool readColumn(
    sqlite3_stmt* statement,
    int column_index,
    SQLiteValue& value,
    std::string& error_message
)
{
    const int column_type = sqlite3_column_type(
        statement,
        column_index
    );

    switch (column_type)
    {
        // NULL
        case SQLITE_NULL:
        {
            value = std::monostate{};
            return true;
        }

        // INTEGER
        case SQLITE_INTEGER:
        {
            value = static_cast<std::int64_t>(
                sqlite3_column_int64(
                    statement,
                    column_index
                )
            );

            return true;
        }

        // FLOAT
        case SQLITE_FLOAT:
        {
            value = sqlite3_column_double(
                statement,
                column_index
            );

            return true;
        }

        // TEXT
        case SQLITE_TEXT:
        {
            const auto* data = sqlite3_column_text(
                statement,
                column_index
            );

            const int bytes = sqlite3_column_bytes(
                statement,
                column_index
            );

            if (bytes > 0 && data == nullptr)
            {
                error_message = "读取 SQLite TEXT 数据失败";
                return false;
            }

            if (bytes == 0)
            {
                value = std::string{};
            }
            else
            {
                value = std::string(
                    reinterpret_cast<const char*>(data),
                    static_cast<std::size_t>(bytes)
                );
            }

            return true;
        }

        // BLOB
        case SQLITE_BLOB:
        {
            const void* data = sqlite3_column_blob(
                statement,
                column_index
            );

            const int bytes = sqlite3_column_bytes(
                statement,
                column_index
            );

            if (bytes > 0 && data == nullptr)
            {
                error_message = "读取 SQLite BLOB 数据失败";
                return false;
            }

            SQLiteBlob blob;

            if (bytes > 0)
            {
                const auto* begin =
                    static_cast<const std::uint8_t*>(data);

                blob.assign(
                    begin,
                    begin + bytes
                );
            }

            value = std::move(blob);

            return true;
        }

        default:
        {
            error_message =
                "遇到不支持的 SQLite 数据类型";

            return false;
        }
    }
}

} // namespace


// ==================================================
// SQLiteDatabase PImpl
// ==================================================

class SQLiteDatabase::Impl
{
public:

    sqlite3* database = nullptr;

    ~Impl()
    {
        if (database != nullptr)
        {
            sqlite3_close_v2(database);
            database = nullptr;
        }
    }
};


// ==================================================
// 构造与析构
// ==================================================

SQLiteDatabase::SQLiteDatabase()
    : impl_(std::make_unique<Impl>())
{
}


SQLiteDatabase::~SQLiteDatabase() = default;


// ==================================================
// 数据库连接管理
// ==================================================

bool SQLiteDatabase::open(
    const std::string& database_path,
    int busy_timeout_ms,
    std::string& error_message
)
{
    error_message.clear();

    // 不允许重复打开
    if (isOpen())
    {
        error_message = "SQLite 数据库已经打开";
        return false;
    }

    // 检查数据库路径
    if (database_path.empty())
    {
        error_message = "SQLite 数据库路径不能为空";
        return false;
    }

    // 检查忙等待时间
    if (busy_timeout_ms < 0)
    {
        error_message = "SQLite busy_timeout_ms 不能为负数";
        return false;
    }

    sqlite3* database = nullptr;

    // 打开数据库
    const int result_code = sqlite3_open_v2(
        database_path.c_str(),
        &database,
        SQLITE_OPEN_READWRITE |
        SQLITE_OPEN_CREATE |
        SQLITE_OPEN_FULLMUTEX,
        nullptr
    );

    if (result_code != SQLITE_OK)
    {
        error_message =
            "打开 SQLite 数据库失败: " +
            getSQLiteError(database, result_code);

        if (database != nullptr)
        {
            sqlite3_close_v2(database);
        }

        return false;
    }

    // 开启扩展错误码
    int code = sqlite3_extended_result_codes(
        database,
        1
    );

    if (code != SQLITE_OK)
    {
        error_message =
            "设置 SQLite 扩展错误码失败: " +
            getSQLiteError(database, code);

        sqlite3_close_v2(database);
        return false;
    }

    // 设置忙等待超时时间
    code = sqlite3_busy_timeout(
        database,
        busy_timeout_ms
    );

    if (code != SQLITE_OK)
    {
        error_message =
            "设置 SQLite busy_timeout 失败: " +
            getSQLiteError(database, code);

        sqlite3_close_v2(database);
        return false;
    }

    // ==================================================
    // 配置 WAL 模式
    // ==================================================

    if (
        !executeSQLiteSql(
            database,
            "PRAGMA journal_mode=WAL;",
            "设置 SQLite WAL 模式",
            error_message
        )
    )
    {
        sqlite3_close_v2(database);
        return false;
    }

    // sqlite3_exec 不会把 PRAGMA 返回值交给调用者，
    // 因此需要再次读取并确认实际模式确实为 WAL。
    if (
        !verifyPragmaText(
            database,
            "PRAGMA journal_mode;",
            "wal",
            "验证 SQLite WAL 模式",
            error_message
        )
    )
    {
        sqlite3_close_v2(database);
        return false;
    }


    // ==================================================
    // 配置同步级别
    // ==================================================

    if (
        !executeSQLiteSql(
            database,
            "PRAGMA synchronous=FULL;",
            "设置 SQLite synchronous",
            error_message
        )
    )
    {
        sqlite3_close_v2(database);
        return false;
    }


    // ==================================================
    // 开启外键约束
    // ==================================================

    if (
        !executeSQLiteSql(
            database,
            "PRAGMA foreign_keys=ON;",
            "开启 SQLite foreign_keys",
            error_message
        )
    )
    {
        sqlite3_close_v2(database);
        return false;
    }

    // 所有初始化成功后，保存数据库连接
    impl_->database = database;

    return true;
}


// ==================================================
// 关闭数据库
// ==================================================

void SQLiteDatabase::close() noexcept
{
    if (!isOpen())
    {
        return;
    }

    const int result_code = sqlite3_close_v2(
        impl_->database
    );

    if (result_code == SQLITE_OK)
    {
        impl_->database = nullptr;
    }
}


// ==================================================
// 检查数据库是否打开
// ==================================================

bool SQLiteDatabase::isOpen() const noexcept
{
    return impl_ != nullptr &&
           impl_->database != nullptr;
}


// ==================================================
// 执行原始 SQL
// ==================================================

bool SQLiteDatabase::executeRaw(
    const std::string& sql,
    std::string& error_message
)
{
    error_message.clear();

    if (!isOpen())
    {
        error_message = "SQLite 数据库尚未打开";
        return false;
    }

    if (sql.empty())
    {
        error_message = "SQL 语句不能为空";
        return false;
    }

    return executeSQLiteSql(
        impl_->database,
        sql,
        "执行 SQL",
        error_message
    );
}


// ==================================================
// 执行带参数绑定的 SQL
// ==================================================

bool SQLiteDatabase::execute(
    const std::string& sql,
    const std::vector<SQLiteValue>& params,
    SQLiteExecuteResult& result,
    std::string& error_message
)
{
    result = {};
    error_message.clear();

    if (!isOpen())
    {
        error_message = "SQLite 数据库尚未打开";
        return false;
    }

    SQLiteStatement statement(
        nullptr,
        sqlite3_finalize
    );

    // 预编译 SQL
    if (
        !prepareStatement(
            impl_->database,
            sql,
            statement,
            error_message
        )
    )
    {
        return false;
    }

    // 绑定参数
    if (
        !bindParameters(
            statement.get(),
            params,
            impl_->database,
            error_message
        )
    )
    {
        return false;
    }

    // 执行 SQL
    if (
        !stepStatement(
            statement.get(),
            impl_->database,
            error_message
        )
    )
    {
        return false;
    }

    SQLiteExecuteResult candidate;

    candidate.last_insert_row_id =
        sqlite3_last_insert_rowid(impl_->database);

    candidate.affected_rows =
        sqlite3_changes(impl_->database);

    result = candidate;

    return true;
}


// ==================================================
// 批量执行 SQL
// ==================================================

bool SQLiteDatabase::executeBatch(
    const std::string& sql,
    const std::vector<std::vector<SQLiteValue>>& parameter_sets,
    int& affected_rows,
    std::string& error_message
)
{
    error_message.clear();

    affected_rows = 0;

    if (!isOpen())
    {
        error_message = "SQLite 数据库尚未打开";
        return false;
    }

    // 空批次是成功的空操作
    if (parameter_sets.empty())
    {
        return true;
    }

    SQLiteStatement statement(
        nullptr,
        sqlite3_finalize
    );

    // 只预编译一次
    if (
        !prepareStatement(
            impl_->database,
            sql,
            statement,
            error_message
        )
    )
    {
        return false;
    }

    int total_affected_rows = 0;

    for (const auto& params : parameter_sets)
    {
        // 绑定本组参数
        if (
            !bindParameters(
                statement.get(),
                params,
                impl_->database,
                error_message
            )
        )
        {
            // 返回已经成功执行的部分数量。
            // 如果调用者开启了事务，可以据此决定是否回滚。
            affected_rows = total_affected_rows;
            return false;
        }

        // 执行本组 SQL
        if (
            !stepStatement(
                statement.get(),
                impl_->database,
                error_message
            )
        )
        {
            affected_rows = total_affected_rows;
            return false;
        }

        // 累计影响行数，并检查 int 溢出。
        const int current_affected_rows =
            sqlite3_changes(impl_->database);

        if (current_affected_rows >
            std::numeric_limits<int>::max() - total_affected_rows)
        {
            error_message = "批量 SQL 影响行数超出 int 可表示范围";
            affected_rows = total_affected_rows;
            return false;
        }

        total_affected_rows += current_affected_rows;
        affected_rows = total_affected_rows;

        // 重置预编译语句
        int code = sqlite3_reset(statement.get());

        if (code != SQLITE_OK)
        {
            error_message =
                "重置 SQLite Statement 失败: " +
                getSQLiteError(impl_->database, code);

            return false;
        }

        // 清除上一组绑定参数
        code = sqlite3_clear_bindings(statement.get());

        if (code != SQLITE_OK)
        {
            error_message =
                "清除 SQLite 参数绑定失败: " +
                getSQLiteError(impl_->database, code);

            return false;
        }
    }

    return true;
}


// ==================================================
// 数据查询
// ==================================================

bool SQLiteDatabase::query(
    const std::string& sql,
    const std::vector<SQLiteValue>& params,
    SQLiteRows& rows,
    std::string& error_message
)
{
    rows.clear();
    error_message.clear();

    if (!isOpen())
    {
        error_message = "SQLite 数据库尚未打开";
        return false;
    }

    SQLiteStatement statement(
        nullptr,
        sqlite3_finalize
    );

    // 预编译 SQL
    if (
        !prepareStatement(
            impl_->database,
            sql,
            statement,
            error_message
        )
    )
    {
        return false;
    }

    // 绑定参数
    if (
        !bindParameters(
            statement.get(),
            params,
            impl_->database,
            error_message
        )
    )
    {
        return false;
    }

    SQLiteRows candidate_rows;

    // 执行查询
    while (true)
    {
        const int result_code = sqlite3_step(
            statement.get()
        );

        if (result_code == SQLITE_DONE)
        {
            break;
        }

        if (result_code != SQLITE_ROW)
        {
            error_message =
                "SQLite 查询失败: " +
                getSQLiteError(
                    impl_->database,
                    result_code
                );

            return false;
        }

        const int column_count =
            sqlite3_column_count(statement.get());

        SQLiteRow row;

        row.reserve(
            static_cast<std::size_t>(column_count)
        );

        // 读取当前行所有列
        for (int i = 0; i < column_count; ++i)
        {
            SQLiteValue value;

            if (
                !readColumn(
                    statement.get(),
                    i,
                    value,
                    error_message
                )
            )
            {
                return false;
            }

            row.push_back(std::move(value));
        }

        candidate_rows.push_back(std::move(row));
    }

    // 查询成功后再提交结果
    rows = std::move(candidate_rows);

    return true;
}


// ==================================================
// 事务管理
// ==================================================

bool SQLiteDatabase::beginTransaction(
    std::string& error_message
)
{
    return executeRaw(
        "BEGIN IMMEDIATE;",
        error_message
    );
}


bool SQLiteDatabase::commit(
    std::string& error_message
)
{
    return executeRaw(
        "COMMIT;",
        error_message
    );
}


bool SQLiteDatabase::rollback(
    std::string& error_message
)
{
    return executeRaw(
        "ROLLBACK;",
        error_message
    );
}

} // namespace industrial_gateway::infrastructure::storage::sqlite
