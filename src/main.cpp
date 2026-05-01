#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "context/scanner.hpp"
#include "generator/writer.hpp"
#include "llm/api.hpp"
#include "template/loader.hpp"
#include "util/config.hpp"
#include "util/paths.hpp"
#ifdef _WIN32
#include "web/server.hpp"
#endif

#ifdef CCT_CN_VERSION
#define CCT_CN_VERSION_STR CCT_CN_VERSION
#else
#define CCT_CN_VERSION_STR "dev"
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
/** 让 MSVC 调试控制台按 UTF-8 显示中文（源码为 UTF-8 /utf-8） */
static void configure_windows_console_utf8() {
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
}
#endif

static void print_help() {
  std::cout
      << "cct-cn — 汉化代码生成 CLI\n"
      << "用法:\n"
      << "  cct-cn init [--config 路径]\n"
      << "  cct-cn template-list [--template-root 目录]\n"
      << "  cct-cn generate -t <模板id> -p <中文需求> [选项]\n"
#ifdef _WIN32
      << "  cct-cn serve [--port 端口] [--workspace 目录] [-c 配置]\n"
      << "       （持久化：使用 SQL Server；需配置 sql_odbc_connection_string 并执行 scripts/sql/schema.sql）\n"
#endif
      << "选项:\n"
      << "  -c, --config <路径>     配置文件（默认 ./.cct-cn/config.json）\n"
      << "  --template-file <路径>   直接指定 .md 模板\n"
      << "  --project <目录>        上下文扫描根目录（默认 .）\n"
      << "  -o, --out <目录>        输出根目录（默认 .）\n"
      << "  --dry-run               只预览，不写文件\n"
      << "  --force                 覆盖已存在文件\n"
      << "  --mock                  强制 Mock API\n"
      << "  --version               版本号\n";
}

static bool streq(const char* a, const char* b) { return std::strcmp(a, b) == 0; }

static int run_init(const std::string& config_override) {
  auto p = config_override.empty() ? cct::util::default_config_path() : std::filesystem::path(config_override);
  std::string err;
  cct::util::save_example_config(p, err);
  if (!err.empty()) {
    std::cerr << err << std::endl;
    return 1;
  }
  std::cout << "已写入示例配置: " << p.string() << std::endl;
  return 0;
}

static int run_template_list(const std::string& tpl_root_opt) {
  std::filesystem::path root = tpl_root_opt.empty() ? cct::util::template_root_from_env_or_default()
                                                      : std::filesystem::path(tpl_root_opt);
  std::error_code ec;
  if (!std::filesystem::exists(root, ec)) {
    std::cerr << "模板目录不存在: " << root.string() << std::endl;
    return 1;
  }
  for (const auto& e : std::filesystem::directory_iterator(root, ec)) {
    if (!e.is_regular_file()) continue;
    if (e.path().extension() == ".md") {
      std::cout << e.path().stem().string() << "  —  " << e.path().filename().string() << std::endl;
    }
  }
  return 0;
}

struct GenArgs {
  std::string config_path;
  std::string template_name;
  std::string template_file;
  std::string prompt;
  std::string project_dir = ".";
  std::string out_dir = ".";
  bool dry_run = false;
  bool force = false;
  bool mock = false;
};

static int parse_generate(int argc, char** argv, GenArgs& a, std::string& err) {
  for (int i = 2; i < argc; ++i) {
    const char* s = argv[i];
    auto need = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        err = std::string("选项 ") + name + " 缺少参数";
        return nullptr;
      }
      return argv[++i];
    };
    if (streq(s, "-c") || streq(s, "--config")) {
      const char* v = need(s);
      if (!v) return 1;
      a.config_path = v;
    } else if (streq(s, "-t") || streq(s, "--template")) {
      const char* v = need(s);
      if (!v) return 1;
      a.template_name = v;
    } else if (streq(s, "--template-file")) {
      const char* v = need(s);
      if (!v) return 1;
      a.template_file = v;
    } else if (streq(s, "-p") || streq(s, "--prompt")) {
      const char* v = need(s);
      if (!v) return 1;
      a.prompt = v;
    } else if (streq(s, "--project")) {
      const char* v = need(s);
      if (!v) return 1;
      a.project_dir = v;
    } else if (streq(s, "-o") || streq(s, "--out")) {
      const char* v = need(s);
      if (!v) return 1;
      a.out_dir = v;
    } else if (streq(s, "--dry-run")) {
      a.dry_run = true;
    } else if (streq(s, "--force")) {
      a.force = true;
    } else if (streq(s, "--mock")) {
      a.mock = true;
    } else {
      err = std::string("未知参数: ") + s;
      return 1;
    }
  }
  if (a.prompt.empty()) {
    err = "请使用 -p / --prompt 提供中文需求描述";
    return 1;
  }
  if (a.template_name.empty() && a.template_file.empty()) {
    err = "请指定 -t <模板id> 或 --template-file <路径>";
    return 1;
  }
  return 0;
}

static int run_generate(GenArgs a) {
  std::filesystem::path cfg_path =
      a.config_path.empty() ? cct::util::default_config_path() : std::filesystem::path(a.config_path);
  cct::util::AppConfig cfg;
  std::string err;
  if (!std::filesystem::exists(cfg_path)) {
    std::cerr << "未找到配置文件，请先运行: cct-cn init" << std::endl;
    return 1;
  }
  if (!cct::util::load_config(cfg_path, cfg, err)) {
    std::cerr << err << std::endl;
    return 1;
  }
  if (a.mock) cfg.use_mock = true;

  std::filesystem::path tpl_root = cct::util::template_root_from_env_or_default();
  std::filesystem::path tpl_path;
  if (!a.template_file.empty()) {
    tpl_path = std::filesystem::path(a.template_file);
  } else {
    tpl_path = tpl_root / (a.template_name + ".md");
  }

  cct::template_ns::LoadedTemplate loaded;
  if (!cct::template_ns::load_markdown_template(tpl_path.string(), loaded, err)) {
    std::cerr << err << std::endl;
    return 1;
  }

  cct::context::ScanOptions scan;
  scan.root = std::filesystem::path(a.project_dir);
  scan.max_total_chars = cfg.max_context_chars;
  std::string ctx = cct::context::scan_project_context(scan, err);
  if (!err.empty()) {
    std::cerr << "上下文扫描: " << err << std::endl;
    return 1;
  }

  std::string user_block;
  user_block += "【角色与约束模板】\n";
  user_block += loaded.body;
  user_block += "\n\n【用户需求】\n";
  user_block += a.prompt;
  user_block += "\n\n【项目上下文（可能已截断）】\n";
  user_block += ctx.empty() ? "(无或目录为空)" : ctx;
  user_block +=
      "\n\n请输出可落地的代码。若需写入多文件，请使用 Markdown 代码块，且第一行写相对路径，例如:\n";
  user_block += "```src/main.cpp\n// code\n```\n";

  cct::llm::LlmResult llm_out;
  if (cfg.use_mock) {
    llm_out = cct::llm::call_mock(a.prompt);
  } else {
    std::string prov;
    for (char c : cfg.llm_provider) prov += static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    if (prov == "zhipu")
      llm_out = cct::llm::call_zhipu(cfg, user_block);
    else
      llm_out = cct::llm::call_anthropic(cfg, user_block);
  }
  if (!llm_out.ok) {
    std::cerr << "模型调用失败: " << llm_out.error << std::endl;
    return 1;
  }

  auto base_out = std::filesystem::path(a.out_dir);
  auto apply = cct::generator::apply_model_output(llm_out.text, base_out, a.dry_run, a.force);
  std::cout << "写入文件数: " << apply.files_written << std::endl;
  for (const auto& m : apply.messages) {
    std::cout << m << std::endl;
  }
  if (a.dry_run) {
    std::cout << "\n--- 模型原始输出 ---\n" << llm_out.text << std::endl;
  }
  return 0;
}

static int parse_template_list_args(int argc, char** argv, std::string& tpl_root, std::string& err) {
  for (int i = 2; i < argc; ++i) {
    if (streq(argv[i], "--template-root") && i + 1 < argc) {
      tpl_root = argv[++i];
    } else {
      err = std::string("未知参数: ") + argv[i];
      return 1;
    }
  }
  return 0;
}

int main(int argc, char** argv) {
#ifdef _WIN32
  configure_windows_console_utf8();
#endif
  if (argc < 2) {
    print_help();
    return 1;
  }
  if (streq(argv[1], "--version") || streq(argv[1], "-version")) {
    std::cout << "cct-cn " << CCT_CN_VERSION_STR << std::endl;
    return 0;
  }
  if (streq(argv[1], "-h") || streq(argv[1], "--help")) {
    print_help();
    return 0;
  }
  if (streq(argv[1], "init")) {
    std::string cfg;
    for (int i = 2; i < argc; ++i) {
      if ((streq(argv[i], "-c") || streq(argv[i], "--config")) && i + 1 < argc) {
        cfg = argv[++i];
      } else {
        std::cerr << "未知参数: " << argv[i] << std::endl;
        return 1;
      }
    }
    return run_init(cfg);
  }
  if (streq(argv[1], "template-list")) {
    std::string root, err;
    if (parse_template_list_args(argc, argv, root, err) != 0) {
      std::cerr << err << std::endl;
      return 1;
    }
    return run_template_list(root);
  }
  if (streq(argv[1], "generate")) {
    GenArgs a;
    std::string err;
    if (parse_generate(argc, argv, a, err) != 0) {
      std::cerr << err << std::endl;
      return 1;
    }
    return run_generate(std::move(a));
  }
#ifdef _WIN32
  if (streq(argv[1], "serve")) {
    int port = 8787;
    std::string config_override;
    std::string data_dir = "data";
    std::string workspace_cli;
    for (int i = 2; i < argc; ++i) {
      if ((streq(argv[i], "-c") || streq(argv[i], "--config")) && i + 1 < argc) {
        config_override = argv[++i];
      } else if (streq(argv[i], "--port") && i + 1 < argc) {
        port = std::atoi(argv[++i]);
      } else if (streq(argv[i], "--workspace") && i + 1 < argc) {
        workspace_cli = argv[++i];
      } else {
        std::cerr << "未知参数: " << argv[i] << std::endl;
        return 1;
      }
    }
    if (port <= 0 || port > 65535) {
      std::cerr << "端口无效\n";
      return 1;
    }
    std::filesystem::path cfg_path =
        config_override.empty() ? cct::util::default_config_path() : std::filesystem::path(config_override);
    cct::util::AppConfig cfg;
    std::string err;
    if (!std::filesystem::exists(cfg_path)) {
      std::cerr << "未找到配置文件，请先运行: cct-cn init\n";
      return 1;
    }
    if (!cct::util::load_config(cfg_path, cfg, err)) {
      std::cerr << err << std::endl;
      return 1;
    }
    cct::util::apply_cli_workspace_override(cfg, workspace_cli.empty() ? nullptr : workspace_cli.c_str());
    std::error_code ec;
    std::filesystem::path ui_root = std::filesystem::path(argv[0]).parent_path() / "ui";
    if (!std::filesystem::is_directory(ui_root, ec)) {
      ui_root = std::filesystem::current_path() / "ui";
    }
    cct::web::run_http_server(static_cast<std::uint16_t>(port), ui_root, std::filesystem::path(data_dir), cfg);
    return 0;
  }
#endif

  std::cerr << "未知子命令: " << argv[1] << std::endl;
  print_help();
  return 1;
}
