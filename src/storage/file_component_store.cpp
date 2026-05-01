#include "file_component_store.hpp"

#include "../util/component_name_utf8.hpp"

#include <fstream>
#include <iterator>

namespace cct::storage {

namespace {
constexpr std::size_t kUserComponentMaxBytes = 400000;
}

void FileComponentPersistence::ensure_default_samples(std::uint64_t uid) {
  std::error_code ec;
  /* 若种子 Markdown 曾被保存在错误的类别目录下，移到对应文件夹 */
  {
    auto move_if_wrong = [&](const char* from_cat, const char* to_cat, const char* stem) {
      std::filesystem::path from = base_for(uid, from_cat) / (std::string(stem) + ".md");
      std::filesystem::path to = base_for(uid, to_cat) / (std::string(stem) + ".md");
      std::error_code e2;
      if (!std::filesystem::is_regular_file(from, e2)) return;
      if (std::filesystem::exists(to, e2)) {
        std::filesystem::remove(from, e2);
        return;
      }
      std::filesystem::create_directories(to.parent_path(), e2);
      std::filesystem::rename(from, to, e2);
    };
    move_if_wrong("skills", "agents", "毕设导师");
    move_if_wrong("agents", "skills", "论文格式");
  }
  auto ensure_one = [&](const char* comp, const char* stem, const char* md) {
    std::filesystem::path base = base_for(uid, comp);
    std::filesystem::create_directories(base, ec);
    bool has = false;
    for (const auto& e : std::filesystem::directory_iterator(base, ec)) {
      if (e.is_regular_file() && e.path().extension() == ".md") {
        has = true;
        break;
      }
    }
    if (has) return;
    std::filesystem::path fp = base / (std::string(stem) + ".md");
    std::ofstream o(fp, std::ios::binary | std::ios::trunc);
    o << md;
  };
  ensure_one("agents", "毕设导师",
             "---\nname: \xE6\xAF\x95\xE8\xAE\xBE\xE5\xAF\xBC\xE5\xB8\x88\n---\n\n"
             "你是一位耐心、负责的高校毕业设计（或课程大作业）指导教师。\n"
             "回答时先肯定合理之处，再分点说明问题与改进；给出可执行的修改步骤，语气正式但不生硬。\n"
             "遇代码/格式问题用简短小标题，避免空话。\n");
  ensure_one("skills", "论文格式", "---\nname: \xE8\xAE\xBA\xE6\x96\x87\xE6\xA0\xBC\xE5\xBC\x8F\n---\n\n"
                                   "【论文格式与排版】\n"
                                   "- 中文正文：宋体或学校指定字体；西文/数字可配合 Times New Roman。\n"
                                   "- 行距 1.5 倍、段前段后 0.5 行，章节编号按 1 1.1 1.1.1 层级。\n"
                                   "- 图表须有编号与题注；参考文献按学校模板（如 GB/T 7714）列出。\n"
                                   "在对话中若涉及导出 Word/LaTeX，提醒用户按学院最新通知核对模板。\n");
}

bool FileComponentPersistence::list_stems(std::uint64_t uid, const char* cat, std::vector<std::string>& stems) {
  stems.clear();
  std::error_code ec;
  std::filesystem::path base = base_for(uid, cat);
  std::filesystem::create_directories(base, ec);
  for (const auto& e : std::filesystem::directory_iterator(base, ec)) {
    if (!e.is_regular_file()) continue;
    if (e.path().extension() != ".md") continue;
    stems.push_back(e.path().stem().string());
  }
  return true;
}

bool FileComponentPersistence::get_content(std::uint64_t uid, const char* cat, const std::string& name,
                                           std::string& body, std::string& error) {
  body.clear();
  if (name.empty() || !cct::util::valid_component_name_utf8(name)) {
    error = "非法名称";
    return false;
  }
  std::error_code ec;
  std::filesystem::path fp = base_for(uid, cat) / (name + ".md");
  if (!std::filesystem::is_regular_file(fp, ec) || ec) {
    error = "不存在";
    return false;
  }
  std::ifstream in(fp, std::ios::binary);
  if (!in) {
    error = "读取失败";
    return false;
  }
  body.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (body.size() > kUserComponentMaxBytes) body.resize(kUserComponentMaxBytes);
  return true;
}

bool FileComponentPersistence::create_new(std::uint64_t uid, const char* cat, const std::string& name,
                                        const std::string& content, std::string& error) {
  if (!cct::util::valid_component_name_utf8(name)) {
    error = "非法名称";
    return false;
  }
  std::error_code ec;
  std::filesystem::path base = base_for(uid, cat);
  std::filesystem::create_directories(base, ec);
  std::filesystem::path fp = base / (name + ".md");
  if (std::filesystem::exists(fp, ec)) {
    error = "已存在同名项";
    return false;
  }
  std::ofstream out(fp, std::ios::binary | std::ios::trunc);
  out << content;
  return static_cast<bool>(out);
}

bool FileComponentPersistence::update_content(std::uint64_t uid, const char* cat, const std::string& name,
                                              const std::string& content, std::string& error) {
  if (!cct::util::valid_component_name_utf8(name)) {
    error = "非法名称";
    return false;
  }
  std::error_code ec;
  std::filesystem::path base = base_for(uid, cat);
  std::filesystem::create_directories(base, ec);
  std::filesystem::path fp = base / (name + ".md");
  std::ofstream out(fp, std::ios::binary | std::ios::trunc);
  out << content;
  return static_cast<bool>(out);
}

bool FileComponentPersistence::remove(std::uint64_t uid, const char* cat, const std::string& name,
                                      std::string& error) {
  if (!cct::util::valid_component_name_utf8(name)) {
    error = "非法名称";
    return false;
  }
  std::error_code ec;
  std::filesystem::path fp = base_for(uid, cat) / (name + ".md");
  std::filesystem::remove(fp, ec);
  (void)error;
  return true;
}

}  // namespace cct::storage
