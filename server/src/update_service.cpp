#include "update_service.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <sstream>

#include "devcard/update_package.h"

namespace devcard {
namespace {

std::int64_t epoch_now() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string random_install_id() {
  // 只用于灰度分桶，随机即可，不需要严格 UUID。
  std::random_device device;
  std::mt19937 gen(device());
  std::uniform_int_distribution<std::uint32_t> dist;
  char buf[37];
  std::snprintf(buf, sizeof(buf),
                "%08x-%04x-%04x-%04x-%08x%04x",
                dist(gen), dist(gen) & 0xffffu, (dist(gen) & 0x0fffu) | 0x4000u,
                (dist(gen) & 0x3fffu) | 0x8000u, dist(gen), dist(gen) & 0xffffu);
  return buf;
}

// Windows 旧壳（updater_win）的状态文件：install_id 继承过来，
// 避免收口后同机换桶导致灰度人群一次性抖动。
std::string migrate_install_id_from_win_shell() {
#ifdef _WIN32
  const char* local = std::getenv("LOCALAPPDATA");
  if (!local || !*local) return {};
  const std::filesystem::path legacy =
      std::filesystem::path(local) / "com.simem.devcard" / "updater_win.json";
  std::ifstream in(legacy, std::ios::binary);
  if (!in) return {};
  const nlohmann::json doc =
      nlohmann::json::parse(in, nullptr, false);
  if (doc.is_discarded() || !doc.is_object()) return {};
  const auto it = doc.find("install_id");
  if (it == doc.end() || !it->is_string()) return {};
  return it->get<std::string>();
#else
  return {};
#endif
}

const char* verdict_name(UpdateDecision::Verdict verdict) {
  switch (verdict) {
    case UpdateDecision::Verdict::Available: return "available";
    case UpdateDecision::Verdict::RolloutBlocked: return "rollout_blocked";
    case UpdateDecision::Verdict::Skipped: return "skipped";
    case UpdateDecision::Verdict::NoUpdate: break;
  }
  return "no_update";
}

}  // namespace

UpdateService::UpdateService(UpdateServiceConfig config)
    : config_(std::move(config)) {
  std::ifstream in(config_.data_dir / "update-state.json", std::ios::binary);
  if (in) {
    const nlohmann::json doc = nlohmann::json::parse(in, nullptr, false);
    if (!doc.is_discarded() && doc.is_object()) {
      auto_check_ = doc.value("auto_check", true);
      prefetch_ = doc.value("prefetch", false);
      install_id_ = doc.value("install_id", std::string{});
      last_check_epoch_ = doc.value("last_check_epoch", std::int64_t{0});
      next_check_epoch_ = doc.value("next_check_epoch", std::int64_t{0});
      consecutive_failures_ = static_cast<int>(
          doc.value("consecutive_failures", std::int64_t{0}));
      skipped_version_ = doc.value("skipped_version", std::string{});
      skip_until_epoch_ = doc.value("skip_until_epoch", std::int64_t{0});
      // 判定结果也读回：重启后界面立刻有上次结论，不用等下一轮检查。
      const auto decision_it = doc.find("decision");
      if (decision_it != doc.end() && decision_it->is_object()) {
        const nlohmann::json& d = *decision_it;
        decision_.verdict = [&d] {
          const std::string v = d.value("verdict", "no_update");
          if (v == "available") return UpdateDecision::Verdict::Available;
          if (v == "rollout_blocked")
            return UpdateDecision::Verdict::RolloutBlocked;
          if (v == "skipped") return UpdateDecision::Verdict::Skipped;
          return UpdateDecision::Verdict::NoUpdate;
        }();
        decision_.reason = d.value("reason", std::string{});
        decision_.version = d.value("version", std::string{});
        decision_.build = d.value("build", std::uint64_t{0});
        decision_.has_artifact = d.value("has_artifact", false);
        if (decision_.has_artifact) {
          const auto& a = d.at("artifact");
          decision_.artifact.role = a.value("role", std::string{});
          decision_.artifact.url = a.value("url", std::string{});
          decision_.artifact.name = a.value("name", std::string{});
          decision_.artifact.sha256 = a.value("sha256", std::string{});
          decision_.artifact.size = a.value("size", std::uint64_t{0});
        }
        decision_.mandatory = d.value("mandatory", false);
        decision_.mandatory_active = d.value("mandatory_active", false);
        decision_.mandatory_reason = d.value("mandatory_reason", std::string{});
        decision_.mandatory_deadline =
            d.value("mandatory_deadline", std::string{});
        decision_.mandatory_deadline_epoch =
            d.value("mandatory_deadline_epoch", std::int64_t{0});
        decision_.notes_url = d.value("notes_url", std::string{});
        has_decision_ = decision_.verdict != UpdateDecision::Verdict::NoUpdate ||
                        !decision_.version.empty();
      }
      last_error_ = doc.value("last_error", std::string{});
    }
  }
  if (install_id_.empty()) install_id_ = migrate_install_id_from_win_shell();
  if (install_id_.empty()) install_id_ = random_install_id();
  // 跳过窗口过期即失效，不带进新进程。
  if (skip_until_epoch_ <= epoch_now()) {
    skipped_version_.clear();
    skip_until_epoch_ = 0;
  }
  // 自更新重启后本地版本已追平上次判定的目标版本：过期的 available/skipped
  // 判定立即失效并落盘，否则顶栏会残留「有新版本 vX」提示直到下一轮检查
  // （最长 24 小时）。
  if (decision_.verdict == UpdateDecision::Verdict::Available ||
      decision_.verdict == UpdateDecision::Verdict::Skipped) {
    ProductVersion latest{}, mine{};
    if (!decision_.version.empty() &&
        parse_product_version(decision_.version, latest) &&
        parse_product_version(current_product_version(), mine)) {
      const int cmp = compare_product_version(mine, latest);
      const bool caught_up =
          cmp > 0 || (cmp == 0 && compiled_build_number() >= decision_.build);
      if (caught_up) {
        decision_ = UpdateDecision{};
        decision_.reason = "up_to_date";
        has_decision_ = false;
        const std::lock_guard<std::mutex> lock(mutex_);
        persist_locked();
      }
    }
  }
}

UpdateService::~UpdateService() { stop(); }

void UpdateService::start() {
  if (config_.endpoint.empty()) return;
  if (thread_.joinable()) return;
  stop_ = false;
  thread_ = std::thread([this] { loop(); });
}

void UpdateService::stop() {
  stop_ = true;
  if (thread_.joinable()) thread_.join();
}

void UpdateService::loop() {
  // 启动 30s 后首查（与旧 win 壳同口径），之后成功 24h±2h、失败按退避。
  for (int i = 0; i < 30 && !stop_; ++i) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  while (!stop_) {
    std::int64_t due = 0;
    bool auto_check = true;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      due = next_check_epoch_;
      auto_check = auto_check_;
    }
    if (!auto_check) {
      // 关了自动检查也要每分钟看一眼开关，不能睡死。
      for (int i = 0; i < 60 && !stop_; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
      }
      continue;
    }
    const std::int64_t now = epoch_now();
    if (now >= due) {
      run_check();
      continue;  // run_check 会更新 next_check_epoch_，回到等待
    }
    const std::int64_t remain = due - now;
    const std::int64_t slice = remain > 60 ? 60 : remain;
    for (std::int64_t i = 0; i < slice && !stop_; ++i) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }
}

void UpdateService::run_check() {
  std::lock_guard<std::mutex> check_lock(check_mutex_);
  const std::int64_t now = epoch_now();
  std::string install_id;
  std::string skipped_version;
  std::int64_t skip_until = 0;
  std::string channel;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    install_id = install_id_;
    skipped_version = skipped_version_;
    skip_until = skip_until_epoch_;
    channel = config_.channel;
  }

  std::string body;
  std::string error;
  bool fetched = false;
  if (config_.fetcher) {
    fetched = config_.fetcher(config_.endpoint, body, error);
  } else {
    // 生产路径：系统 curl 落临时文件再读回。URL 白名单在 curl_fetch 内部还有
    // 第二道闸，这里先查一次避免给不可信地址发起连接。
    error.clear();
    if (!is_trusted_update_url(config_.endpoint)) {
      error = "清单地址不在可信白名单内";
    } else {
      const std::filesystem::path updates_dir = config_.data_dir / "updates";
      std::error_code ec;
      std::filesystem::create_directories(updates_dir, ec);
      const std::filesystem::path tmp = updates_dir / "manifest-fetch.tmp";
      std::string fetch_error;
      std::string fetch_code;
      if (curl_fetch(config_.endpoint, tmp, &fetch_error, &fetch_code)) {
        std::ifstream in(tmp, std::ios::binary);
        std::ostringstream sink;
        sink << in.rdbuf();
        body = sink.str();
        fetched = !body.empty();
        if (!fetched) error = "清单为空";
      } else {
        error = fetch_error.empty() ? fetch_code : fetch_error;
      }
      std::filesystem::remove(tmp, ec);
    }
  }

  std::lock_guard<std::mutex> lock(mutex_);
  last_check_epoch_ = now;
  if (!fetched) {
    // 拉取失败：保留上一次判定（界面不塌），只累加退避。
    ++consecutive_failures_;
    last_error_ = "fetch_failed: " + error;
    next_check_epoch_ = now + check_retry_delay(consecutive_failures_);
    persist_locked();
    return;
  }
  const SignedManifest manifest =
      parse_signed_manifest(body, config_.pubkey_hex);
  if (!manifest.ok) {
    ++consecutive_failures_;
    last_error_ = "manifest_invalid: " + manifest.error;
    next_check_epoch_ = now + check_retry_delay(consecutive_failures_);
    persist_locked();
    return;
  }
  LocalUpdateState local;
  local.version = current_product_version();
  local.build = compiled_build_number();
  local.channel = channel;
  local.install_id = install_id;
  local.skipped_version = skipped_version;
  local.skip_until_epoch = skip_until;
  local.now_epoch = now;
  decision_ = evaluate_update(manifest, local);
  has_decision_ = true;
  consecutive_failures_ = 0;
  last_error_.clear();
  std::random_device device;
  std::uniform_int_distribution<std::int64_t> jitter(-2 * 3600, 2 * 3600);
  next_check_epoch_ = now + 24 * 3600 + jitter(device);
  persist_locked();
}

nlohmann::json UpdateService::check_now() {
  run_check();
  return state_json();
}

void UpdateService::set_options(bool auto_check, bool prefetch) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto_check_ = auto_check;
    prefetch_ = prefetch;
    if (auto_check_) next_check_epoch_ = 0;  // 重新打开时立刻补一轮，不等旧排期
    persist_locked();
  }
}

void UpdateService::skip_version(const std::string& version, std::int64_t hours) {
  if (version.empty()) return;
  if (hours < 1) hours = 1;
  if (hours > 720) hours = 720;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    skipped_version_ = version;
    skip_until_epoch_ = epoch_now() + hours * 3600;
    // 判定快照同步落到 Skipped：前端不用等下一轮检查就能收起提示。
    if (has_decision_ && decision_.version == version &&
        decision_.verdict == UpdateDecision::Verdict::Available) {
      decision_.verdict = UpdateDecision::Verdict::Skipped;
      decision_.reason = "skip";
    }
    persist_locked();
  }
}

UpdateDecision UpdateService::decision() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return decision_;
}

bool UpdateService::has_decision() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return has_decision_;
}

std::string UpdateService::install_id() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return install_id_;
}

std::string UpdateService::skipped_version() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return skipped_version_;
}

std::int64_t UpdateService::skip_until_epoch() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return skip_until_epoch_;
}

nlohmann::json UpdateService::state_json() const {
  std::lock_guard<std::mutex> lock(mutex_);
  nlohmann::json out{
      {"enabled", !config_.endpoint.empty()},
      {"endpoint", config_.endpoint},
      {"channel", config_.channel},
      {"auto_check", auto_check_},
      {"prefetch", prefetch_},
      {"install_id", install_id_},
      {"last_check_epoch", last_check_epoch_},
      {"next_check_epoch", next_check_epoch_},
      {"consecutive_failures", consecutive_failures_},
      {"skipped_version", skipped_version_},
      {"skip_until_epoch", skip_until_epoch_},
      {"last_error", last_error_.empty() ? nlohmann::json(nullptr)
                                         : nlohmann::json(last_error_)},
  };
  if (!has_decision_) {
    out["checked"] = false;
    return out;
  }
  out["checked"] = true;
  out["verdict"] = verdict_name(decision_.verdict);
  out["reason"] = decision_.reason;
  out["update_available"] =
      decision_.verdict == UpdateDecision::Verdict::Available;
  out["latest"] = decision_.version.empty() ? nlohmann::json(nullptr)
                                            : nlohmann::json(decision_.version);
  out["latest_build"] = decision_.build;
  out["notes_url"] = decision_.notes_url;
  out["mandatory"] = {
      {"declared", decision_.mandatory},
      {"active", decision_.mandatory_active},
      {"reason", decision_.mandatory_reason},
      {"deadline", decision_.mandatory_deadline},
      {"deadline_epoch", decision_.mandatory_deadline_epoch},
  };
  if (decision_.has_artifact) {
    out["artifact"] = {
        {"role", decision_.artifact.role},
        {"name", decision_.artifact.name},
        {"url", decision_.artifact.url},
        {"sha256", decision_.artifact.sha256},
        {"size", decision_.artifact.size},
    };
  } else {
    out["artifact"] = nullptr;
  }
  return out;
}

void UpdateService::persist_locked() const {
  nlohmann::json doc{
      {"auto_check", auto_check_},
      {"prefetch", prefetch_},
      {"install_id", install_id_},
      {"last_check_epoch", last_check_epoch_},
      {"next_check_epoch", next_check_epoch_},
      {"consecutive_failures", consecutive_failures_},
      {"skipped_version", skipped_version_},
      {"skip_until_epoch", skip_until_epoch_},
      {"last_error", last_error_},
  };
  if (has_decision_) {
    nlohmann::json d{
        {"verdict", verdict_name(decision_.verdict)},
        {"reason", decision_.reason},
        {"version", decision_.version},
        {"build", decision_.build},
        {"has_artifact", decision_.has_artifact},
        {"mandatory", decision_.mandatory},
        {"mandatory_active", decision_.mandatory_active},
        {"mandatory_reason", decision_.mandatory_reason},
        {"mandatory_deadline", decision_.mandatory_deadline},
        {"mandatory_deadline_epoch", decision_.mandatory_deadline_epoch},
        {"notes_url", decision_.notes_url},
    };
    if (decision_.has_artifact) {
      d["artifact"] = {
          {"role", decision_.artifact.role},
          {"name", decision_.artifact.name},
          {"url", decision_.artifact.url},
          {"sha256", decision_.artifact.sha256},
          {"size", decision_.artifact.size},
      };
    }
    doc["decision"] = std::move(d);
  }
  // 原子替换：先写 .tmp 再 rename；Windows 上 rename 不覆盖已存在文件，先删旧的。
  const std::filesystem::path target = config_.data_dir / "update-state.json";
  const std::filesystem::path tmp = target.string() + ".tmp";
  std::error_code ec;
  std::filesystem::create_directories(target.parent_path(), ec);
  std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
  if (!out) return;
  out << doc.dump(2);
  out.close();
  std::filesystem::remove(target, ec);
  std::filesystem::rename(tmp, target, ec);
}

}  // namespace devcard
