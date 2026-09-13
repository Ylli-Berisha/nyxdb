#include "common/logger.h"
#include "database/database.h"

#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <variant>

using namespace nyx;

struct Config {
    std::string data_dir = "./nyxdb_data";
    std::string log_level = "warn";
};

static Config parse_args(int argc, char* argv[]) {
    Config cfg;
    for (int i = 1; i < argc - 1; ++i) {
        if (std::strcmp(argv[i], "--data-dir") == 0)
            cfg.data_dir = argv[i + 1];
        else if (std::strcmp(argv[i], "--log-level") == 0)
            cfg.log_level = argv[i + 1];
    }
    return cfg;
}

static std::string fmt_value(const Value& v) {
    return std::visit(
        [](const auto& x) -> std::string {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, std::monostate>)
                return "NULL";
            else if constexpr (std::is_same_v<T, i32> || std::is_same_v<T, i64>)
                return std::to_string(x);
            else {
                std::ostringstream oss;
                oss << x;
                return oss.str();
            }
        },
        v);
}

static void print_result(const ExecuteResult& res) {
    size_t ncols = res.schema.size();
    size_t nrows = res.row_count();

    std::vector<size_t> widths(ncols);
    for (size_t c = 0; c < ncols; c++)
        widths[c] = res.schema[c].name.size();

    std::vector<std::vector<std::string>> cells(ncols, std::vector<std::string>(nrows));
    for (size_t c = 0; c < ncols; c++)
        for (size_t r = 0; r < nrows; r++) {
            cells[c][r] = fmt_value(res.columns[c][r]);
            widths[c] = std::max(widths[c], cells[c][r].size());
        }

    for (size_t c = 0; c < ncols; c++) {
        std::cout << " " << std::left << std::setw(static_cast<int>(widths[c]))
                  << res.schema[c].name;
        if (c + 1 < ncols)
            std::cout << " |";
    }
    std::cout << "\n";

    for (size_t c = 0; c < ncols; c++) {
        std::cout << "-" << std::string(widths[c], '-');
        if (c + 1 < ncols)
            std::cout << "-+";
    }
    std::cout << "\n";

    for (size_t r = 0; r < nrows; r++) {
        for (size_t c = 0; c < ncols; c++) {
            std::cout << " " << std::left << std::setw(static_cast<int>(widths[c])) << cells[c][r];
            if (c + 1 < ncols)
                std::cout << " |";
        }
        std::cout << "\n";
    }

    std::cout << "(" << nrows << (nrows == 1 ? " row)\n" : " rows)\n");
}

int main(int argc, char* argv[]) {
    Config cfg = parse_args(argc, argv);
    init_logger(cfg.log_level);

    auto db_r = Database::open(cfg.data_dir);
    if (!db_r.is_ok()) {
        std::cerr << "Failed to open database: " << db_r.error().message << "\n";
        return 1;
    }
    auto db = std::move(db_r.value());

    std::cout << "nyxdb  data=" << cfg.data_dir << "  (exit or Ctrl-D to quit)\n\n";

    std::string line;
    while (true) {
        std::cout << "nyx> " << std::flush;
        if (!std::getline(std::cin, line))
            break;

        auto s = line.find_first_not_of(" \t");
        if (s == std::string::npos)
            continue;
        line = line.substr(s);
        auto e = line.find_last_not_of(" \t;");
        if (e != std::string::npos)
            line = line.substr(0, e + 1);
        if (line.empty())
            continue;
        if (line == "exit" || line == "quit" || line == "\\q")
            break;

        auto r = db.execute(line);
        if (r.is_err()) {
            std::cerr << "Error: " << r.error().message << "\n";
            continue;
        }

        auto& res = r.value();
        if (res.rows_affected > 0)
            std::cout << res.rows_affected
                      << (res.rows_affected == 1 ? " row affected\n" : " rows affected\n");
        else if (!res.schema.empty())
            print_result(res);
        else
            std::cout << "OK\n";
    }

    return 0;
}
