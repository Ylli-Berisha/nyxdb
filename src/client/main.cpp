#include "server/shard_client.h"
#include "storage/disk/type_id.h"
#include "storage/disk/value.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace nyx;
using namespace nyx::server;

static std::string trim(const std::string& s) {
    usize a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos)
        return {};
    usize b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::string format_value(const Value& v, TypeId t) {
    if (std::holds_alternative<std::monostate>(v))
        return "";
    if (t == TypeId::INT32)
        return std::to_string(std::get<i32>(v));
    if (t == TypeId::INT64)
        return std::to_string(std::get<i64>(v));
    if (t == TypeId::DOUBLE) {
        std::ostringstream oss;
        oss << std::get<f64>(v);
        return oss.str();
    }
    if (t == TypeId::BOOL)
        return std::get<bool>(v) ? "t" : "f";
    if (t == TypeId::VARCHAR)
        return std::get<std::string>(v);
    if (t == TypeId::DATE) {
        time_t secs = static_cast<time_t>(std::get<Date>(v).days) * 86400;
        struct tm tm {};
        gmtime_r(&secs, &tm);
        char buf[16];
        strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
        return buf;
    }
    if (t == TypeId::TIMESTAMP) {
        time_t secs = static_cast<time_t>(std::get<Timestamp>(v).micros / 1'000'000);
        struct tm tm {};
        gmtime_r(&secs, &tm);
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
        return buf;
    }
    return "";
}

static bool is_numeric(TypeId t) {
    return t == TypeId::INT32 || t == TypeId::INT64 || t == TypeId::DOUBLE;
}

static void print_result(const ExecuteResult& r) {
    if (r.schema.empty()) {
        if (r.rows_affected == 0)
            std::cout << "OK\n";
        else
            std::cout << r.rows_affected << " rows affected\n";
        return;
    }

    usize ncols = r.schema.size();
    usize nrows = r.row_count();

    std::vector<std::vector<std::string>> cells(nrows, std::vector<std::string>(ncols));
    for (usize c = 0; c < ncols; ++c)
        for (usize row = 0; row < nrows; ++row)
            cells[row][c] = format_value(r.columns[c][row], r.schema[c].type);

    std::vector<usize> widths(ncols);
    for (usize c = 0; c < ncols; ++c) {
        widths[c] = r.schema[c].name.size();
        for (usize row = 0; row < nrows; ++row)
            widths[c] = std::max(widths[c], cells[row][c].size());
    }

    auto print_row = [&](const std::vector<std::string>& vals, bool header) {
        std::cout << " ";
        for (usize c = 0; c < ncols; ++c) {
            if (c > 0)
                std::cout << " | ";
            usize w = widths[c];
            const std::string& v = vals[c];
            if (!header && is_numeric(r.schema[c].type))
                std::cout << std::string(w - v.size(), ' ') << v;
            else
                std::cout << v << std::string(w - v.size(), ' ');
        }
        std::cout << '\n';
    };

    std::vector<std::string> headers(ncols);
    for (usize c = 0; c < ncols; ++c)
        headers[c] = r.schema[c].name;
    print_row(headers, true);

    std::cout << "-";
    for (usize c = 0; c < ncols; ++c) {
        if (c > 0)
            std::cout << "-+-";
        std::cout << std::string(widths[c], '-');
    }
    std::cout << "-\n";

    for (usize row = 0; row < nrows; ++row)
        print_row(cells[row], false);

    std::cout << '(' << nrows << (nrows == 1 ? " row)\n" : " rows)\n");
}

static std::optional<std::string> handle_meta(const std::string& cmd) {
    if (cmd == "\\q" || cmd == "exit" || cmd == "quit")
        return std::nullopt;
    if (cmd == "\\dt")
        return "SHOW TABLES;";
    if (cmd.size() > 3 && cmd.substr(0, 3) == "\\d ")
        return "SHOW COLUMNS FROM " + cmd.substr(3) + ";";
    std::cerr << "unknown meta-command: " << cmd << '\n';
    return "";
}

static void run_repl(ShardClient& client, const std::string& addr) {
    std::cout << "nyxdb " << addr << "\n";
    std::string buf;
    std::string line;

    while (true) {
        std::cout << (buf.empty() ? "nyxdb " + addr + "> " : "              ...> ") << std::flush;

        if (!std::getline(std::cin, line))
            break;

        std::string t = trim(line);

        if (t.empty()) {
            if (!buf.empty())
                std::cout << (buf.empty() ? "nyxdb " + addr + "> " : "              ...> ")
                          << std::flush;
            continue;
        }

        if (t[0] == '\\' || t == "exit" || t == "quit") {
            auto sql = handle_meta(t);
            if (!sql.has_value())
                break;
            if (sql->empty())
                continue;
            buf = *sql;
        } else {
            if (!buf.empty())
                buf += ' ';
            buf += line;
        }

        if (!buf.empty() && buf.back() == ';') {
            auto r = client.execute(trim(buf));
            if (r.is_ok())
                print_result(r.value());
            else
                std::cerr << "ERROR: " << r.error().message << '\n';
            buf.clear();
        }
    }

    std::cout << '\n';
}

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    std::string port = "4433";
    std::string token;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--host" && i + 1 < argc)
            host = argv[++i];
        else if (a == "--port" && i + 1 < argc)
            port = argv[++i];
        else if (a == "--token" && i + 1 < argc)
            token = argv[++i];
        else {
            std::cerr << "usage: nyxdb_client [--host <h>] [--port <p>] --token <token>\n";
            return 1;
        }
    }

    if (token.empty()) {
        std::cerr << "error: --token is required\n";
        return 1;
    }

    std::string addr = host + ":" + port;
    auto r = ShardClient::connect(addr, token);
    if (!r.is_ok()) {
        std::cerr << "error: " << r.error().message << '\n';
        return 1;
    }

    run_repl(r.value(), addr);
    return 0;
}
