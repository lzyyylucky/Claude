// SQL Server ODBC implementation built into cct-storage.dll — see CMakeLists.txt
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "storage_iface.hpp"
#include "storage_helpers.hpp"
#include "token_billing_common.hpp"
#include "../web/crypto.hpp"

#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <sql.h>
#include <sqlext.h>

#pragma comment(lib, "odbc32.lib")

namespace {

std::wstring u82w(const std::string& u8) {
  if (u8.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), static_cast<int>(u8.size()), nullptr, 0);
  if (n <= 0) return {};
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), static_cast<int>(u8.size()), w.data(), n);
  return w;
}

std::string w2u8(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  if (n <= 0) return {};
  std::string u8(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), u8.data(), n, nullptr, nullptr);
  return u8;
}

bool rok(SQLRETURN rc) { return rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO; }

std::wstring sq_esc_u8(const std::string& u8) {
  std::wstring w = u82w(u8);
  std::wstring o;
  o.reserve(w.size() + 8);
  for (wchar_t c : w) {
    if (c == L'\'') o += L'\'';
    o += c;
  }
  return o;
}

bool exec_w(SQLHDBC dbc, const std::wstring& sql, std::string& err) {
  SQLHSTMT st = SQL_NULL_HSTMT;
  if (!rok(SQLAllocHandle(SQL_HANDLE_STMT, dbc, &st))) {
    err = "SQLAllocHandle STMT";
    return false;
  }
  SQLRETURN rc = SQLExecDirectW(st, const_cast<SQLWCHAR*>(reinterpret_cast<const SQLWCHAR*>(sql.c_str())), SQL_NTS);
  if (!rok(rc)) {
    SQLWCHAR msg[SQL_MAX_MESSAGE_LENGTH]{};
    SQLSMALLINT ml = 0;
    SQLGetDiagRecW(SQL_HANDLE_STMT, st, 1, nullptr, nullptr, msg, SQL_MAX_MESSAGE_LENGTH - 1, &ml);
    err = w2u8(msg);
    SQLFreeHandle(SQL_HANDLE_STMT, st);
    return false;
  }
  SQLFreeHandle(SQL_HANDLE_STMT, st);
  return true;
}

struct OdbcConn {
  SQLHENV env = SQL_NULL_HENV;
  SQLHDBC dbc = SQL_NULL_HDBC;
};

bool conn_open_u8(const std::string& conn_utf8, OdbcConn& out, std::string& err) {
  std::wstring wconn = u82w(conn_utf8);
  if (!rok(SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &out.env))) {
    err = "SQLAllocHandle ENV";
    return false;
  }
  SQLSetEnvAttr(out.env, SQL_ATTR_ODBC_VERSION, reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0);
  if (!rok(SQLAllocHandle(SQL_HANDLE_DBC, out.env, &out.dbc))) {
    err = "SQLAllocHandle DBC";
    SQLFreeHandle(SQL_HANDLE_ENV, out.env);
    out.env = SQL_NULL_HENV;
    return false;
  }
  SQLWCHAR ob[4096]{};
  SQLSMALLINT oc = 0;
  SQLRETURN rc =
      SQLDriverConnectW(out.dbc, nullptr, reinterpret_cast<SQLWCHAR*>(const_cast<wchar_t*>(wconn.c_str())), SQL_NTS,
                        ob, static_cast<SQLSMALLINT>(sizeof(ob) / sizeof(ob[0])), &oc, SQL_DRIVER_NOPROMPT);
  if (!rok(rc)) {
    SQLWCHAR msg[SQL_MAX_MESSAGE_LENGTH]{};
    SQLSMALLINT ml = 0;
    SQLGetDiagRecW(SQL_HANDLE_DBC, out.dbc, 1, nullptr, nullptr, msg, SQL_MAX_MESSAGE_LENGTH - 1, &ml);
    err = w2u8(msg);
    SQLFreeHandle(SQL_HANDLE_DBC, out.dbc);
    SQLFreeHandle(SQL_HANDLE_ENV, out.env);
    out.dbc = SQL_NULL_HDBC;
    out.env = SQL_NULL_HENV;
    return false;
  }
  return true;
}

void conn_close(OdbcConn& c) {
  if (c.dbc) {
    SQLDisconnect(c.dbc);
    SQLFreeHandle(SQL_HANDLE_DBC, c.dbc);
    c.dbc = SQL_NULL_HDBC;
  }
  if (c.env) {
    SQLFreeHandle(SQL_HANDLE_ENV, c.env);
    c.env = SQL_NULL_HENV;
  }
}

/** 未跑过增量脚本时自动建表，避免 /api/me 因查不到 UserBilling 返回 500 导致前端登录死循环 */
bool ddl_ensure_user_billing(SQLHDBC dbc, std::string& err) {
  const std::wstring sql =
      L"IF NOT EXISTS (SELECT 1 FROM sys.tables t INNER JOIN sys.schemas s ON t.schema_id = s.schema_id "
      L"WHERE s.name = N'dbo' AND t.name = N'UserBilling') "
      L"BEGIN "
      L"CREATE TABLE dbo.UserBilling ("
      L"UserId BIGINT NOT NULL CONSTRAINT PK_UserBilling PRIMARY KEY,"
      L"Tier NVARCHAR(16) NOT NULL CONSTRAINT DF_UserBilling_Tier DEFAULT (N'free'),"
      L"TokenQuota BIGINT NOT NULL CONSTRAINT DF_UserBilling_Quota DEFAULT (50000),"
      L"TokensConsumed BIGINT NOT NULL CONSTRAINT DF_UserBilling_Consumed DEFAULT (0),"
      L"PeriodYm INT NOT NULL CONSTRAINT DF_UserBilling_Period DEFAULT (0),"
      L"UpdatedAt DATETIME2 NOT NULL CONSTRAINT DF_UserBilling_Upd DEFAULT (SYSUTCDATETIME()),"
      L"CONSTRAINT FK_UserBilling_User FOREIGN KEY (UserId) REFERENCES dbo.Users (Id) ON DELETE CASCADE"
      L"); END";
  return exec_w(dbc, sql, err);
}

}  // namespace

namespace cct::storage::sql {

class SqlUserPersistence final : public IUserPersistence {
 public:
  explicit SqlUserPersistence(SQLHDBC dbc) : dbc_(dbc) {}

  bool register_user(const std::string& username, const std::string& password, std::uint64_t& out_id,
                     std::string& error) override {
    std::vector<unsigned char> salt;
    if (!cct::web::crypto::random_bytes(salt, 16)) {
      error = "随机数失败";
      return false;
    }
    std::vector<unsigned char> hash;
    if (!cct::web::crypto::pbkdf2_sha256(password, salt, hash)) {
      error = "哈希失败";
      return false;
    }
    std::string salt_hex = cct::web::crypto::bytes_to_hex(salt.data(), salt.size());
    std::string hash_hex = cct::web::crypto::bytes_to_hex(hash.data(), hash.size());
    /** OUTPUT INSERTED.Id：避免 INSERT;SELECT SCOPE_IDENTITY() 多结果集时 ODBC 首次 Fetch 落到行计数导致 Id=0 */
    std::wstring sql =
        L"INSERT INTO dbo.Users (Username, DisplayName, SaltHex, HashHex) OUTPUT INSERTED.Id VALUES (N'" +
        sq_esc_u8(username) + L"', N'" + sq_esc_u8(username) + L"', N'" + sq_esc_u8(salt_hex) + L"', N'" +
        sq_esc_u8(hash_hex) + L"')";
    SQLHSTMT st = SQL_NULL_HSTMT;
    if (!rok(SQLAllocHandle(SQL_HANDLE_STMT, dbc_, &st))) {
      error = "SQL_alloc";
      return false;
    }
    if (!rok(SQLExecDirectW(st, reinterpret_cast<SQLWCHAR*>(const_cast<wchar_t*>(sql.c_str())), SQL_NTS))) {
      SQLFreeHandle(SQL_HANDLE_STMT, st);
      error = "用户名已存在或其它插入失败";
      return false;
    }
    SQLBIGINT idv = 0;
    SQLLEN ind = 0;
    SQLRETURN fr_id = SQLFetch(st);
    if (fr_id == SQL_SUCCESS || fr_id == SQL_SUCCESS_WITH_INFO)
      SQLGetData(st, 1, SQL_C_SBIGINT, &idv, sizeof(idv), &ind);
    SQLFreeHandle(SQL_HANDLE_STMT, st);
    out_id = static_cast<std::uint64_t>(idv);
    if (out_id == 0) {
      error = "插入失败";
      return false;
    }
    std::wstring pref =
        L"IF NOT EXISTS (SELECT 1 FROM dbo.UserPreferences WHERE UserId=" + std::to_wstring(out_id) +
        L") INSERT INTO dbo.UserPreferences (UserId, Theme) VALUES (" + std::to_wstring(out_id) + L", N'dark');";
    std::string er;
    exec_w(dbc_, pref, er);
    return true;
  }

  bool verify_login(const std::string& username, const std::string& password, std::uint64_t& out_id,
                    std::string& error) override {
    std::wstring sql =
        L"SELECT Id, SaltHex, HashHex FROM dbo.Users WHERE Username=N'" + sq_esc_u8(username) + L"'";
    SQLHSTMT st = SQL_NULL_HSTMT;
    if (!rok(SQLAllocHandle(SQL_HANDLE_STMT, dbc_, &st))) {
      error = "SQL_alloc";
      return false;
    }
    if (!rok(SQLExecDirectW(st, reinterpret_cast<SQLWCHAR*>(const_cast<wchar_t*>(sql.c_str())), SQL_NTS))) {
      SQLFreeHandle(SQL_HANDLE_STMT, st);
      error = "用户名或密码错误";
      return false;
    }
    SQLRETURN fr = SQLFetch(st);
    if (fr != SQL_SUCCESS && fr != SQL_SUCCESS_WITH_INFO) {
      SQLFreeHandle(SQL_HANDLE_STMT, st);
      error = "用户名或密码错误";
      return false;
    }
    SQLBIGINT idv = 0;
    SQLLEN id_ind = 0;
    SQLGetData(st, 1, SQL_C_SBIGINT, &idv, sizeof(idv), &id_ind);
    SQLWCHAR sb[512]{}, hb[512]{};
    SQLLEN sl = 0, hl = 0;
    SQLGetData(st, 2, SQL_C_WCHAR, sb, sizeof(sb), &sl);
    SQLGetData(st, 3, SQL_C_WCHAR, hb, sizeof(hb), &hl);
    SQLFreeHandle(SQL_HANDLE_STMT, st);
    std::string salt_hex = w2u8(sb);
    std::string expect_hex = w2u8(hb);
    std::vector<unsigned char> salt, expect;
    if (!cct::web::crypto::hex_to_bytes(salt_hex, salt) || !cct::web::crypto::hex_to_bytes(expect_hex, expect)) {
      error = "用户数据损坏";
      return false;
    }
    std::vector<unsigned char> got;
    if (!cct::web::crypto::pbkdf2_sha256(password, salt, got)) {
      error = "哈希失败";
      return false;
    }
    std::string got_hex = cct::web::crypto::bytes_to_hex(got.data(), got.size());
    if (!cct::web::crypto::timing_equal_hex(got_hex, expect_hex)) {
      error = "用户名或密码错误";
      return false;
    }
    out_id = static_cast<std::uint64_t>(idv);
    return true;
  }

  bool get_user_by_id(std::uint64_t id, UserRowLite& out, std::string& error) override {
    std::wstring sql =
        L"SELECT Username, DisplayName, SaltHex, HashHex FROM dbo.Users WHERE Id=" + std::to_wstring(id);
    SQLHSTMT st = SQL_NULL_HSTMT;
    if (!rok(SQLAllocHandle(SQL_HANDLE_STMT, dbc_, &st))) {
      error = "SQL_alloc";
      return false;
    }
    if (!rok(SQLExecDirectW(st, reinterpret_cast<SQLWCHAR*>(const_cast<wchar_t*>(sql.c_str())), SQL_NTS))) {
      SQLFreeHandle(SQL_HANDLE_STMT, st);
      error = "用户不存在";
      return false;
    }
    SQLRETURN fr_u = SQLFetch(st);
    if (fr_u != SQL_SUCCESS && fr_u != SQL_SUCCESS_WITH_INFO) {
      SQLFreeHandle(SQL_HANDLE_STMT, st);
      error = "用户不存在";
      return false;
    }
    SQLWCHAR ub[64]{}, db[256]{}, sb[512]{}, hb[512]{};
    SQLLEN il = 0;
    SQLGetData(st, 1, SQL_C_WCHAR, ub, sizeof(ub), &il);
    SQLGetData(st, 2, SQL_C_WCHAR, db, sizeof(db), &il);
    SQLGetData(st, 3, SQL_C_WCHAR, sb, sizeof(sb), &il);
    SQLGetData(st, 4, SQL_C_WCHAR, hb, sizeof(hb), &il);
    SQLFreeHandle(SQL_HANDLE_STMT, st);
    out.id = id;
    out.username = w2u8(ub);
    out.display_name = w2u8(db);
    out.salt_hex = w2u8(sb);
    out.hash_hex = w2u8(hb);
    return true;
  }

  bool update_display_name(std::uint64_t id, const std::string& display_name, std::string& error) override {
    std::string trimmed = display_name;
    while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t')) trimmed.erase(trimmed.begin());
    while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t')) trimmed.pop_back();
    if (trimmed.empty()) {
      error = "显示名称不能为空";
      return false;
    }
    if (trimmed.size() > 64) {
      error = "显示名称过长（最多 64 字符）";
      return false;
    }
    std::wstring sql =
        L"UPDATE dbo.Users SET DisplayName=N'" + sq_esc_u8(trimmed) + L"' WHERE Id=" + std::to_wstring(id);
    return exec_w(dbc_, sql, error);
  }

  bool get_ui_theme(std::uint64_t id, std::string& theme_out, std::string& error) override {
    (void)error;
    theme_out = "dark";
    SQLHSTMT st = SQL_NULL_HSTMT;
    if (!rok(SQLAllocHandle(SQL_HANDLE_STMT, dbc_, &st))) return true;
    std::wstring sql = L"SELECT Theme FROM dbo.UserPreferences WHERE UserId=" + std::to_wstring(id);
    SQLRETURN er = SQLExecDirectW(st, reinterpret_cast<SQLWCHAR*>(const_cast<wchar_t*>(sql.c_str())), SQL_NTS);
    if (!rok(er)) {
      SQLFreeHandle(SQL_HANDLE_STMT, st);
      return true;
    }
    SQLRETURN fr = SQLFetch(st);
    if (fr != SQL_SUCCESS && fr != SQL_SUCCESS_WITH_INFO) {
      SQLFreeHandle(SQL_HANDLE_STMT, st);
      return true;
    }
    SQLWCHAR tb[32]{};
    SQLLEN il = 0;
    SQLGetData(st, 1, SQL_C_WCHAR, tb, sizeof(tb), &il);
    SQLFreeHandle(SQL_HANDLE_STMT, st);
    std::string t = w2u8(tb);
    for (char& c : t) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    if (t == "dark" || t == "light" || t == "system") theme_out = std::move(t);
    return true;
  }

  bool set_ui_theme(std::uint64_t id, const std::string& theme, std::string& error) override {
    std::string norm = theme;
    for (char& c : norm) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    if (norm != "dark" && norm != "light" && norm != "system") {
      error = "theme 须为 dark、light 或 system";
      return false;
    }
    std::wstring sel =
        L"MERGE dbo.UserPreferences AS T USING (SELECT " + std::to_wstring(id) +
        L" AS UserId) AS S ON (T.UserId = S.UserId) WHEN MATCHED THEN UPDATE SET T.Theme=N'" + sq_esc_u8(norm) +
        L"', T.UpdatedAt=SYSUTCDATETIME() WHEN NOT MATCHED THEN INSERT (UserId, Theme) VALUES (" +
        std::to_wstring(id) + L", N'" + sq_esc_u8(norm) + L"');";
    return exec_w(dbc_, sel, error);
  }

 private:
  SQLHDBC dbc_;
};

class SqlChatPersistence final : public IChatPersistence {
 public:
  explicit SqlChatPersistence(SQLHDBC dbc) : dbc_(dbc) {
    std::string er;
    exec_w(dbc_,
           L"IF OBJECT_ID(N'dbo.ChatThreads', N'U') IS NOT NULL "
           L"AND COL_LENGTH(N'dbo.ChatThreads', N'WorkspaceAnchor') IS NULL "
           L"ALTER TABLE dbo.ChatThreads ADD WorkspaceAnchor NVARCHAR(512) NOT NULL "
           L"CONSTRAINT DF_CT_WorkspaceAnchor DEFAULT (N'');",
           er);
  }

  std::mutex& mutex() override { return mu_; }
  std::unordered_map<std::string, std::vector<ChatThreadRow>>& threads_map() override { return threads_; }
  std::unordered_map<std::string, std::vector<cct::llm::ChatMessage>>& history_map() override { return history_; }
  std::unordered_set<std::uint64_t>& hydrated_users() override { return hydrated_; }

  void ensure_loaded(std::uint64_t uid) override {
    if (hydrated_.count(uid)) return;
    const std::string uk = user_chats_key(uid);
    std::wstring sql =
        L"SELECT ThreadId, Title, Updated, Ordinal, WorkspaceAnchor FROM dbo.ChatThreads WHERE UserId=" +
        std::to_wstring(uid) + L" ORDER BY Ordinal DESC";
    SQLHSTMT st = SQL_NULL_HSTMT;
    if (!rok(SQLAllocHandle(SQL_HANDLE_STMT, dbc_, &st))) {
      hydrated_.insert(uid);
      ensure_default_thread_list_vec(threads_[uk]);
      return;
    }
    if (!rok(SQLExecDirectW(st, reinterpret_cast<SQLWCHAR*>(const_cast<wchar_t*>(sql.c_str())), SQL_NTS))) {
      SQLFreeHandle(SQL_HANDLE_STMT, st);
      hydrated_.insert(uid);
      ensure_default_thread_list_vec(threads_[uk]);
      return;
    }
    std::vector<ChatThreadRow> rows;
    for (;;) {
      SQLRETURN fr = SQLFetch(st);
      if (fr != SQL_SUCCESS && fr != SQL_SUCCESS_WITH_INFO) break;
      SQLWCHAR tid[128]{}, tit[1024]{}, anch[1024]{};
      SQLBIGINT upd = 0, ord = 0;
      SQLLEN il = 0;
      SQLGetData(st, 1, SQL_C_WCHAR, tid, sizeof(tid), &il);
      SQLGetData(st, 2, SQL_C_WCHAR, tit, sizeof(tit), &il);
      SQLGetData(st, 3, SQL_C_SBIGINT, &upd, sizeof(upd), &il);
      SQLGetData(st, 4, SQL_C_SBIGINT, &ord, sizeof(ord), &il);
      SQLGetData(st, 5, SQL_C_WCHAR, anch, sizeof(anch), &il);
      ChatThreadRow r;
      r.id = w2u8(tid);
      r.title = w2u8(tit);
      r.updated = static_cast<std::uint64_t>(upd);
      r.ordinal = static_cast<std::uint64_t>(ord);
      r.workspace_anchor = w2u8(anch);
      rows.push_back(std::move(r));
    }
    SQLFreeHandle(SQL_HANDLE_STMT, st);
    if (rows.empty()) {
      hydrated_.insert(uid);
      ensure_default_thread_list_vec(threads_[uk]);
      return;
    }
    threads_[uk] = std::move(rows);
    for (const auto& row : threads_[uk]) load_msgs(uid, uk, row.id);
    hydrated_.insert(uid);
  }

  void persist(std::uint64_t uid) override {
    const std::string uk = user_chats_key(uid);
    std::wstring uws = std::to_wstring(uid);
    std::string er;
    exec_w(dbc_, L"DELETE FROM dbo.ChatMessages WHERE UserId=" + uws, er);
    exec_w(dbc_, L"DELETE FROM dbo.ChatThreads WHERE UserId=" + uws, er);
    auto it_th = threads_.find(uk);
    if (it_th == threads_.end()) return;
    for (const auto& row : it_th->second) {
      std::wstring ins_th =
          L"INSERT INTO dbo.ChatThreads (UserId, ThreadId, Title, Updated, Ordinal, WorkspaceAnchor) VALUES (" + uws +
          L", N'" + sq_esc_u8(row.id) + L"', N'" + sq_esc_u8(row.title) + L"', " +
          std::to_wstring(static_cast<SQLBIGINT>(row.updated)) + L", " +
          std::to_wstring(static_cast<SQLBIGINT>(row.ordinal)) + L", N'" + sq_esc_u8(row.workspace_anchor) + L"');";
      exec_w(dbc_, ins_th, er);
      const std::string k = chat_hist_key(uk, row.id);
      auto hi = history_.find(k);
      const auto& msgs = (hi == history_.end()) ? std::vector<cct::llm::ChatMessage>{} : hi->second;
      int seq = 0;
      for (const auto& m : msgs) {
        std::wstring ins_m =
            L"INSERT INTO dbo.ChatMessages (UserId, ThreadId, Seq, Role, Content) VALUES (" + uws + L", N'" +
            sq_esc_u8(row.id) + L"', " + std::to_wstring(seq) + L", N'" + sq_esc_u8(m.role) + L"', N'" +
            sq_esc_u8(m.content) + L"');";
        exec_w(dbc_, ins_m, er);
        seq++;
      }
    }
  }

 private:
  void load_msgs(std::uint64_t uid, const std::string& uk, const std::string& tid) {
    std::wstring sql =
        L"SELECT Role, Content FROM dbo.ChatMessages WHERE UserId=" + std::to_wstring(uid) + L" AND ThreadId=N'" +
        sq_esc_u8(tid) + L"' ORDER BY Seq ASC";
    SQLHSTMT st = SQL_NULL_HSTMT;
    if (!rok(SQLAllocHandle(SQL_HANDLE_STMT, dbc_, &st))) return;
    if (!rok(SQLExecDirectW(st, reinterpret_cast<SQLWCHAR*>(const_cast<wchar_t*>(sql.c_str())), SQL_NTS))) {
      SQLFreeHandle(SQL_HANDLE_STMT, st);
      history_[chat_hist_key(uk, tid)] = {};
      return;
    }
    std::vector<cct::llm::ChatMessage> mm;
    for (;;) {
      SQLRETURN fr = SQLFetch(st);
      if (fr != SQL_SUCCESS && fr != SQL_SUCCESS_WITH_INFO) break;
      SQLWCHAR rb[64]{};
      SQLLEN rlen = 0;
      SQLGetData(st, 1, SQL_C_WCHAR, rb, sizeof(rb), &rlen);
      std::string role = w2u8(rb);
      std::string content;
      for (;;) {
        SQLWCHAR chunk[8000]{};
        SQLLEN cl = 0;
        SQLRETURN dr = SQLGetData(st, 2, SQL_C_WCHAR, chunk, sizeof(chunk), &cl);
        if (dr == SQL_NO_DATA) break;
        if (cl > 0 && cl != SQL_NULL_DATA) content += w2u8(chunk);
        if (dr == SQL_SUCCESS) break;
      }
      mm.push_back(cct::llm::ChatMessage{std::move(role), std::move(content)});
    }
    SQLFreeHandle(SQL_HANDLE_STMT, st);
    history_[chat_hist_key(uk, tid)] = std::move(mm);
  }

  SQLHDBC dbc_;
  std::mutex mu_;
  std::unordered_map<std::string, std::vector<ChatThreadRow>> threads_;
  std::unordered_map<std::string, std::vector<cct::llm::ChatMessage>> history_;
  std::unordered_set<std::uint64_t> hydrated_;
};

class SqlComponentPersistence final : public IComponentPersistence {
 public:
  explicit SqlComponentPersistence(SQLHDBC dbc) : dbc_(dbc) {}

  void ensure_default_samples(std::uint64_t uid) override {
    /* 纠正误入另一类 Kind 的种子 Slug（曾出现在 Agents/Skils 下拉混淆的场景） */
    {
      std::string er;
      std::wstring u = std::to_wstring(uid);
      exec_w(dbc_,
             L"UPDATE dbo.Components SET Kind=N'agents' WHERE UserId=" + u +
                 L" AND Slug=N'" + sq_esc_u8(std::string("\xE6\xAF\x95\xE8\xAE\xBE\xE5\xAF\xBC\xE5\xB8\x88")) + L"'",
             er);
      exec_w(dbc_,
             L"UPDATE dbo.Components SET Kind=N'skills' WHERE UserId=" + u +
                 L" AND Slug=N'" + sq_esc_u8(std::string("\xE8\xAE\xBA\xE6\x96\x87\xE6\xA0\xBC\xE5\xBC\x8F")) + L"'",
             er);
    }
    SQLHSTMT st = SQL_NULL_HSTMT;
    std::wstring chk =
        L"SELECT COUNT(*) FROM dbo.Components WHERE UserId=" + std::to_wstring(uid) + L" AND Kind=N'agents'";
    SQLBIGINT cnt = -1;
    if (!rok(SQLAllocHandle(SQL_HANDLE_STMT, dbc_, &st))) return;
    if (rok(SQLExecDirectW(st, reinterpret_cast<SQLWCHAR*>(const_cast<wchar_t*>(chk.c_str())), SQL_NTS)) &&
        SQLFetch(st) == SQL_SUCCESS) {
      SQLGetData(st, 1, SQL_C_SBIGINT, &cnt, sizeof(cnt), nullptr);
    }
    SQLFreeHandle(SQL_HANDLE_STMT, st);
    if (cnt > 0) return;

    const std::string agent_md =
        std::string("---\nname: \xE6\xAF\x95\xE8\xAE\xBE\xE5\xAF\xBC\xE5\xB8\x88\n---\n\n") +
        "\xE4\xBD\xA0\xE6\x98\xAF\xE4\xB8\x80\xE4\xBD\x8D\xE8\x80\x90\xE5\xBF\x83\xE3\x80\x81\xE8\xB4\x9F\xE8\xB4\xA3\xE7\x9A\x84\xE9\xAB\x98\xE6\xA0\xA1\xE6\xAF\x95\xE4\xB8\x9A\xE8\xAE\xBE\xE8\xAE\xA1\xEF\xBC\x88\xE6\x88\x96\xE8\xAF\xBE\xE7\xA8\x8B\xE5\xA4\xA7\xE4\xBD\x9C\xE4\xB8\x9A\xEF\xBC\x89\xE6\x8C\x87\xE5\xAF\xBC\xE6\x95\x99\xE5\xB8\x88\xE3\x80\x82\n"
        "\xE5\x9B\x9E\xE7\xAD\x94\xE6\x97\xB6\xE5\x85\x88\xE8\x82\xAF\xE5\xAE\x9A\xE5\x90\x88\xE7\x90\x86\xE4\xB9\x8B\xE5\xA4\x84\xEF\xBC\x8C\xE5\x86\x8D\xE5\x88\x86\xE7\x82\xB9\xE8\xAF\xB4\xE6\x98\x8E\xE9\x97\xAE\xE9\xA2\x98\xE4\xB8\x8E\xE6\x94\xB9\xE8\xBF\x9B\xEF\xBC\x9B\xE7\xBB\x99\xE5\x87\xBA\xE5\x8F\xAF\xE6\x89\xA7\xE8\xA1\x8C\xE7\x9A\x84\xE4\xBF\xAE\xE6\x94\xB9\xE6\xAD\xA5\xE9\xAA\xA4\xEF\xBC\x8C\xE8\xAF\xAD\xE6\xB0\x94\xE6\xAD\xA3\xE5\xBC\x8F\xE4\xBD\x86\xE4\xB8\x8D\xE7\x94\x9F\xE7\xA1\xAC\xE3\x80\x82\n"
        "\xE9\x81\x87\xE4\xBB\xA3\xE7\xA0\x81/\xE6\xA0\xBC\xE5\xBC\x8F\xE9\x97\xAE\xE9\xA2\x98\xE7\x94\xA8\xE7\xAE\x80\xE7\x9F\xAD\xE5\xB0\x8F\xE6\xA0\x87\xE9\xA2\x98\xEF\xBC\x8C\xE9\x81\xBF\xE5\x85\x8D\xE7\xA9\xBA\xE8\xAF\x9D\xE3\x80\x82\n";
    const std::string skill_md =
        "---\nname: \xE8\xAE\xBA\xE6\x96\x87\xE6\xA0\xBC\xE5\xBC\x8F\n---\n\n"
        "\xE3\x80\x90\xE8\xAE\xBA\xE6\x96\x87\xE6\xA0\xBC\xE5\xBC\x8F\xE4\xB8\x8E\xE6\x8E\x92\xE7\x89\x88\xE3\x80\x91\n"
        "- \xE4\xB8\xAD\xE6\x96\x87\xE6\xAD\xA3\xE6\x96\x87\xEF\xBC\x9A\xE5\xAE\x8B\xE4\xBD\x93\xE6\x88\x96\xE5\xAD\xA6\xE6\xA0\xA1\xE6\x8C\x87\xE5\xAE\x9A\xE5\xAD\x97\xE4\xBD\x93\xEF\xBC\x9B\xE8\xA5\xBF\xE6\x96\x87/\xE6\x95\xB0\xE5\xAD\x97\xE5\x8F\xAF\xE9\x85\x8D\xE5\x90\x88 Times New Roman\xE3\x80\x82\n"
        "- \xE8\xA1\x8C\xE8\xB7\x9D 1.5 \xE5\x80\x8D\xE3\x80\x81\xE6\xAE\xB5\xE5\x89\x8D\xE6\xAE\xB5\xE5\x90\x8E 0.5 \xE8\xA1\x8C\xEF\xBC\x8C\xE7\xAB\xA0\xE8\x8A\x82\xE7\xBC\x96\xE5\x8F\xB7\xE6\x8C\x89 1 1.1 1.1.1 \xE5\xB1\x82\xE7\xBA\xA7\xE3\x80\x82\n"
        "- \xE5\x9B\xBE\xE8\xA1\xA8\xE9\xA1\xBB\xE6\x9C\x89\xE7\xBC\x96\xE5\x8F\xB7\xE4\xB8\x8E\xE9\xA2\x98\xE6\xB3\xA8\xEF\xBC\x9B\xE5\x8F\x82\xE8\x80\x83\xE6\x96\x87\xE7\x8C\xAE\xE6\x8C\x89\xE5\xAD\xA6\xE6\xA0\xA1\xE6\xA8\xA1\xE6\x9D\xBF\xEF\xBC\x88\xE5\xA6\x82 GB/T 7714\xEF\xBC\x89\xE5\x88\x97\xE5\x87\xBA\xE3\x80\x82\n"
        "\xE5\x9C\xA8\xE5\xAF\xB9\xE8\xAF\x9D\xE4\xB8\xAD\xE8\x8B\xA5\xE6\xB6\x89\xE5\x8F\x8A\xE5\xAF\xBC\xE5\x87\xBA Word/LaTeX\xEF\xBC\x8C\xE6\x8F\x90\xE9\x86\x92\xE7\x94\xA8\xE6\x88\xB7\xE6\x8C\x89\xE5\xAD\xA6\xE9\x99\xA2\xE6\x9C\x80\xE6\x96\xB0\xE9\x80\x9A\xE7\x9F\xA5\xE6\xA0\xB8\xE5\xAF\xB9\xE6\xA8\xA1\xE6\x9D\xBF\xE3\x80\x82\n";
    std::wstring u = std::to_wstring(uid);
    std::string er;
    exec_w(dbc_,
           L"INSERT INTO dbo.Components (UserId, Kind, Slug, ContentMd) VALUES (" + u +
               L", N'agents', N'" +
               sq_esc_u8(std::string("\xE6\xAF\x95\xE8\xAE\xBE\xE5\xAF\xBC\xE5\xB8\x88")) + L"', N'" +
               sq_esc_u8(agent_md) + L"');",
           er);
    exec_w(dbc_,
           L"INSERT INTO dbo.Components (UserId, Kind, Slug, ContentMd) VALUES (" + u +
               L", N'skills', N'" +
               sq_esc_u8(std::string("\xE8\xAE\xBA\xE6\x96\x87\xE6\xA0\xBC\xE5\xBC\x8F")) + L"', N'" +
               sq_esc_u8(skill_md) + L"');",
           er);
  }

  bool list_stems(std::uint64_t uid, const char* cat, std::vector<std::string>& stems) override {
    stems.clear();
    std::wstring sql =
        L"SELECT Slug FROM dbo.Components WHERE UserId=" + std::to_wstring(uid) + L" AND Kind=N'" +
        sq_esc_u8(std::string(cat)) + L"' ORDER BY Slug";
    SQLHSTMT st = SQL_NULL_HSTMT;
    if (!rok(SQLAllocHandle(SQL_HANDLE_STMT, dbc_, &st))) return false;
    if (!rok(SQLExecDirectW(st, reinterpret_cast<SQLWCHAR*>(const_cast<wchar_t*>(sql.c_str())), SQL_NTS))) {
      SQLFreeHandle(SQL_HANDLE_STMT, st);
      return true;
    }
    for (;;) {
      SQLRETURN fr = SQLFetch(st);
      if (fr != SQL_SUCCESS && fr != SQL_SUCCESS_WITH_INFO) break;
      SQLWCHAR sb[256]{};
      SQLLEN il = 0;
      SQLGetData(st, 1, SQL_C_WCHAR, sb, sizeof(sb), &il);
      stems.push_back(w2u8(sb));
    }
    SQLFreeHandle(SQL_HANDLE_STMT, st);
    return true;
  }

  bool get_content(std::uint64_t uid, const char* cat, const std::string& name, std::string& body,
                   std::string& error) override {
    body.clear();
    std::wstring sql =
        L"SELECT ContentMd FROM dbo.Components WHERE UserId=" + std::to_wstring(uid) + L" AND Kind=N'" +
        sq_esc_u8(std::string(cat)) + L"' AND Slug=N'" + sq_esc_u8(name) + L"'";
    SQLHSTMT st = SQL_NULL_HSTMT;
    if (!rok(SQLAllocHandle(SQL_HANDLE_STMT, dbc_, &st))) {
      error = "数据库错误";
      return false;
    }
    if (!rok(SQLExecDirectW(st, reinterpret_cast<SQLWCHAR*>(const_cast<wchar_t*>(sql.c_str())), SQL_NTS))) {
      SQLFreeHandle(SQL_HANDLE_STMT, st);
      error = "不存在";
      return false;
    }
    SQLRETURN fr_row = SQLFetch(st);
    if (fr_row != SQL_SUCCESS && fr_row != SQL_SUCCESS_WITH_INFO) {
      SQLFreeHandle(SQL_HANDLE_STMT, st);
      error = "不存在";
      return false;
    }
    for (;;) {
      SQLWCHAR chunk[8000]{};
      SQLLEN cl = 0;
      SQLRETURN dr = SQLGetData(st, 1, SQL_C_WCHAR, chunk, sizeof(chunk), &cl);
      if (dr == SQL_NO_DATA) break;
      if (cl > 0 && cl != SQL_NULL_DATA) body += w2u8(chunk);
      if (dr == SQL_SUCCESS) break;
    }
    SQLFreeHandle(SQL_HANDLE_STMT, st);
    return true;
  }

  bool create_new(std::uint64_t uid, const char* cat, const std::string& name, const std::string& content,
                  std::string& error) override {
    std::wstring sql =
        L"INSERT INTO dbo.Components (UserId, Kind, Slug, ContentMd) VALUES (" + std::to_wstring(uid) +
        L", N'" + sq_esc_u8(std::string(cat)) + L"', N'" + sq_esc_u8(name) + L"', N'" + sq_esc_u8(content) + L"');";
    return exec_w(dbc_, sql, error);
  }

  bool update_content(std::uint64_t uid, const char* cat, const std::string& name, const std::string& content,
                      std::string& error) override {
    std::wstring sql =
        L"UPDATE dbo.Components SET ContentMd=N'" + sq_esc_u8(content) + L"' WHERE UserId=" +
        std::to_wstring(uid) + L" AND Kind=N'" + sq_esc_u8(std::string(cat)) + L"' AND Slug=N'" +
        sq_esc_u8(name) + L"'";
    return exec_w(dbc_, sql, error);
  }

  bool remove(std::uint64_t uid, const char* cat, const std::string& name, std::string& error) override {
    std::wstring sql =
        L"DELETE FROM dbo.Components WHERE UserId=" + std::to_wstring(uid) + L" AND Kind=N'" +
        sq_esc_u8(std::string(cat)) + L"' AND Slug=N'" + sq_esc_u8(name) + L"'";
    return exec_w(dbc_, sql, error);
  }

 private:
  SQLHDBC dbc_;
};

class SqlTokenBillingPersistence final : public ITokenBillingPersistence {
 public:
  explicit SqlTokenBillingPersistence(SQLHDBC dbc) : dbc_(dbc) {}

  bool get_state(std::uint64_t user_id, TokenBillingState& out, std::string& error) override {
    std::lock_guard<std::mutex> lk(mu_);
    return get_state_unlocked(user_id, out, error);
  }

  bool check_can_use(std::uint64_t user_id, std::int64_t min_tokens_needed, TokenBillingState& state_out,
                     std::string& error) override {
    std::lock_guard<std::mutex> lk(mu_);
    if (!get_state_unlocked(user_id, state_out, error)) return false;
    const std::int64_t rem = state_out.token_quota - state_out.tokens_consumed;
    if (min_tokens_needed > rem) {
      error = "本月 Token 额度已用尽，请升级订阅后继续。";
      return false;
    }
    return true;
  }

  bool add_consumed(std::uint64_t user_id, std::int64_t delta, TokenBillingState& state_out,
                    std::string& error) override {
    std::lock_guard<std::mutex> lk(mu_);
    if (delta <= 0) return get_state_unlocked(user_id, state_out, error);
    if (!get_state_unlocked(user_id, state_out, error)) return false;
    state_out.tokens_consumed += delta;
    std::wstring sql =
        L"UPDATE dbo.UserBilling SET TokensConsumed=" + std::to_wstring(state_out.tokens_consumed) +
        L", TokenQuota=" + std::to_wstring(state_out.token_quota) + L", PeriodYm=" +
        std::to_wstring(state_out.period_yyyymm) + L", Tier=N'" + sq_esc_u8(state_out.tier) +
        L"' WHERE UserId=" + std::to_wstring(user_id);
    if (!exec_w(dbc_, sql, error)) return false;
    return true;
  }

  bool apply_subscription(std::uint64_t user_id, const std::string& tier, const std::string& pay_method,
                          std::string& out_txn_id, std::string& error) override {
    const std::string norm = billing_normalize_tier(tier);
    if (!billing_tier_is_paid(norm)) {
      error = "无效套餐";
      return false;
    }
    std::string pm = pay_method;
    for (char& c : pm) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (pm != "wechat" && pm != "alipay") {
      error = "支付方式须为 wechat 或 alipay";
      return false;
    }
    std::vector<unsigned char> rb;
    if (!cct::web::crypto::random_bytes(rb, 8)) {
      error = "随机数失败";
      return false;
    }
    out_txn_id = std::string("mock-") + pm + "-" + cct::web::crypto::bytes_to_hex(rb.data(), rb.size());

    std::lock_guard<std::mutex> lk(mu_);
    TokenBillingState s{};
    s.tier = norm;
    s.token_quota = billing_token_quota_for_tier(norm);
    s.tokens_consumed = 0;
    s.period_yyyymm = billing_current_period_yyyymm();
    std::wstring upsert =
        L"MERGE dbo.UserBilling AS T USING (SELECT " + std::to_wstring(user_id) +
        L" AS UserId) AS S ON T.UserId = S.UserId WHEN MATCHED THEN UPDATE SET "
        L"Tier=N'" +
        sq_esc_u8(s.tier) + L"', TokenQuota=" + std::to_wstring(s.token_quota) + L", TokensConsumed=" +
        std::to_wstring(s.tokens_consumed) + L", PeriodYm=" + std::to_wstring(s.period_yyyymm) +
        L" WHEN NOT MATCHED THEN INSERT (UserId, Tier, TokenQuota, TokensConsumed, PeriodYm) VALUES (" +
        std::to_wstring(user_id) + L", N'" + sq_esc_u8(s.tier) + L"', " + std::to_wstring(s.token_quota) + L", " +
        std::to_wstring(s.tokens_consumed) + L", " + std::to_wstring(s.period_yyyymm) + L");";
    if (!exec_w(dbc_, upsert, error)) return false;
    return true;
  }

 private:
  bool fetch_row(std::uint64_t user_id, TokenBillingState& out, bool& found, std::string& error) {
    found = false;
    std::wstring sql = L"SELECT Tier, TokenQuota, TokensConsumed, PeriodYm FROM dbo.UserBilling WHERE UserId=" +
                       std::to_wstring(user_id);
    SQLHSTMT st = SQL_NULL_HSTMT;
    if (!rok(SQLAllocHandle(SQL_HANDLE_STMT, dbc_, &st))) {
      error = "SQL_alloc";
      return false;
    }
    if (!rok(SQLExecDirectW(st, reinterpret_cast<SQLWCHAR*>(const_cast<wchar_t*>(sql.c_str())), SQL_NTS))) {
      SQLFreeHandle(SQL_HANDLE_STMT, st);
      error = "查询计费失败";
      return false;
    }
    SQLRETURN fr = SQLFetch(st);
    if (fr != SQL_SUCCESS && fr != SQL_SUCCESS_WITH_INFO) {
      SQLFreeHandle(SQL_HANDLE_STMT, st);
      return true;
    }
    found = true;
    SQLWCHAR tb[32]{};
    SQLBIGINT tq = 0, tc = 0;
    SQLINTEGER pym = 0;
    SQLLEN il = 0;
    SQLGetData(st, 1, SQL_C_WCHAR, tb, sizeof(tb), &il);
    SQLGetData(st, 2, SQL_C_SBIGINT, &tq, sizeof(tq), &il);
    SQLGetData(st, 3, SQL_C_SBIGINT, &tc, sizeof(tc), &il);
    SQLGetData(st, 4, SQL_C_SLONG, &pym, sizeof(pym), &il);
    SQLFreeHandle(SQL_HANDLE_STMT, st);
    out.tier = billing_normalize_tier(w2u8(tb));
    out.token_quota = static_cast<std::int64_t>(tq);
    out.tokens_consumed = static_cast<std::int64_t>(tc);
    out.period_yyyymm = static_cast<int>(pym);
    return true;
  }

  bool insert_free_row(std::uint64_t user_id, std::string& error) {
    const std::int64_t q = billing_token_quota_for_tier("free");
    const int pm = billing_current_period_yyyymm();
    std::wstring sql = L"INSERT INTO dbo.UserBilling (UserId, Tier, TokenQuota, TokensConsumed, PeriodYm) VALUES (" +
                       std::to_wstring(user_id) + L", N'free', " + std::to_wstring(q) + L", 0, " +
                       std::to_wstring(pm) + L")";
    return exec_w(dbc_, sql, error);
  }

  bool persist_state_row(std::uint64_t user_id, const TokenBillingState& s, std::string& error) {
    std::wstring sql =
        L"UPDATE dbo.UserBilling SET Tier=N'" + sq_esc_u8(s.tier) + L"', TokenQuota=" +
        std::to_wstring(s.token_quota) + L", TokensConsumed=" + std::to_wstring(s.tokens_consumed) + L", PeriodYm=" +
        std::to_wstring(s.period_yyyymm) + L" WHERE UserId=" + std::to_wstring(user_id);
    return exec_w(dbc_, sql, error);
  }

  bool get_state_unlocked(std::uint64_t user_id, TokenBillingState& out, std::string& error) {
    bool found = false;
    if (!fetch_row(user_id, out, found, error)) return false;
    if (!found) {
      if (!insert_free_row(user_id, error)) return false;
      if (!fetch_row(user_id, out, found, error)) return false;
      if (!found) {
        error = "计费记录异常";
        return false;
      }
    }
    out.tier = billing_normalize_tier(out.tier);
    if (out.token_quota <= 0) out.token_quota = billing_token_quota_for_tier(out.tier);
    const int curp = billing_current_period_yyyymm();
    if (out.period_yyyymm != curp) {
      out.tokens_consumed = 0;
      out.period_yyyymm = curp;
      out.token_quota = billing_token_quota_for_tier(out.tier);
      if (!persist_state_row(user_id, out, error)) return false;
    } else {
      const std::int64_t expect_q = billing_token_quota_for_tier(out.tier);
      if (out.token_quota != expect_q) {
        out.token_quota = expect_q;
        if (!persist_state_row(user_id, out, error)) return false;
      }
    }
    return true;
  }

  SQLHDBC dbc_;
  std::mutex mu_{};
};

struct Bundle {
  OdbcConn conn{};
  std::unique_ptr<SqlUserPersistence> users;
  std::unique_ptr<SqlChatPersistence> chats;
  std::unique_ptr<SqlComponentPersistence> components;
  std::unique_ptr<SqlTokenBillingPersistence> billing;
};

}  // namespace cct::storage::sql

extern "C" __declspec(dllexport) void* cct_sql_bundle_open(const char* conn_utf8, char* err, size_t err_cap) {
  if (!conn_utf8 || !conn_utf8[0]) {
    if (err && err_cap) strncpy_s(err, err_cap, "empty connection string", _TRUNCATE);
    return nullptr;
  }
  auto b = std::make_unique<cct::storage::sql::Bundle>();
  std::string er;
  if (!conn_open_u8(conn_utf8, b->conn, er)) {
    if (err && err_cap) strncpy_s(err, err_cap, er.c_str(), _TRUNCATE);
    return nullptr;
  }
  {
    std::string ddl_err;
    if (!ddl_ensure_user_billing(b->conn.dbc, ddl_err)) {
      conn_close(b->conn);
      const std::string msg = "无法创建/校验 dbo.UserBilling: " + ddl_err;
      if (err && err_cap) strncpy_s(err, err_cap, msg.c_str(), _TRUNCATE);
      return nullptr;
    }
  }
  b->users = std::make_unique<cct::storage::sql::SqlUserPersistence>(b->conn.dbc);
  b->chats = std::make_unique<cct::storage::sql::SqlChatPersistence>(b->conn.dbc);
  b->components = std::make_unique<cct::storage::sql::SqlComponentPersistence>(b->conn.dbc);
  b->billing = std::make_unique<cct::storage::sql::SqlTokenBillingPersistence>(b->conn.dbc);
  return b.release();
}

extern "C" __declspec(dllexport) void cct_sql_bundle_close(void* p) {
  auto* b = reinterpret_cast<cct::storage::sql::Bundle*>(p);
  if (!b) return;
  b->billing.reset();
  b->users.reset();
  b->chats.reset();
  b->components.reset();
  conn_close(b->conn);
  delete b;
}

extern "C" __declspec(dllexport) cct::storage::IUserPersistence* cct_sql_bundle_users(void* p) {
  auto* b = reinterpret_cast<cct::storage::sql::Bundle*>(p);
  return b && b->users ? b->users.get() : nullptr;
}

extern "C" __declspec(dllexport) cct::storage::IChatPersistence* cct_sql_bundle_chats(void* p) {
  auto* b = reinterpret_cast<cct::storage::sql::Bundle*>(p);
  return b && b->chats ? b->chats.get() : nullptr;
}

extern "C" __declspec(dllexport) cct::storage::IComponentPersistence* cct_sql_bundle_components(void* p) {
  auto* b = reinterpret_cast<cct::storage::sql::Bundle*>(p);
  return b && b->components ? b->components.get() : nullptr;
}

extern "C" __declspec(dllexport) cct::storage::ITokenBillingPersistence* cct_sql_bundle_token_billing(void* p) {
  auto* b = reinterpret_cast<cct::storage::sql::Bundle*>(p);
  return b && b->billing ? b->billing.get() : nullptr;
}
