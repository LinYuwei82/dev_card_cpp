#include "devcard/update_engine.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace devcard {

namespace {

// 决策/失败提示的标题与正文（唯一一份，两端共用）
const char* kCheckFailedTitle = "检查更新失败";

// 强制更新原因文案：与旧前端 update.js 的 MANDATORY_REASON_LABELS 逐字一致；
// 未知原因码原样展示（清单来自发布方，验签已保证来源可信）。
const char* mandatory_reason_label(const std::string& code) {
  if (code == "SECURITY") return "安全漏洞修复";
  if (code == "PROTOCOL") return "协议不兼容";
  if (code == "REGULATORY") return "法规或税控要求";
  if (code == "MIN_SUPPORTED") return "低于服务端最低支持版本";
  return code.c_str();
}

}  // namespace

UpdateEngine::UpdateEngine(UpdateEngineConfig config, ManifestFetcher& fetcher,
                           SettingsStore& store, UpdateUi& ui, PlatformInstaller& installer,
                           Logger logger)
    : config_(std::move(config)), fetcher_(fetcher), store_(store), ui_(ui),
      installer_(installer), logger_(std::move(logger)) {}

void UpdateEngine::log(const char* format, ...) const {
  if (!logger_) return;
  char buffer[512] = {};
  va_list args;
  va_start(args, format);
  std::vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  logger_(std::string("NATIVE update ") + buffer);
}

std::string UpdateEngine::formatSize(long long bytes) {
  if (bytes <= 0) return std::string();
  char buffer[64] = {};
  const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
  if (mb >= 1.0)
    std::snprintf(buffer, sizeof(buffer), "%.1f MB", mb);
  else
    std::snprintf(buffer, sizeof(buffer), "%.0f KB", mb * 1024.0);
  return buffer;
}

std::string UpdateEngine::installId() {
  std::string id = store_.getString(update_keys::kInstallId);
  if (id.empty()) {
    id = store_.newInstallId();
    if (!id.empty()) store_.setString(update_keys::kInstallId, id);
  }
  return id;
}

void UpdateEngine::tick(std::time_t now) {
  if (checking_) {
    log("skip background check (check in flight)");
    return;
  }
  const std::string lastText = store_.getString(update_keys::kLastCheckAt);
  if (!lastText.empty()) {
    const long long last = std::strtoll(lastText.c_str(), nullptr, 10);
    if (last > 0 && static_cast<std::time_t>(last) <= now &&
        now - static_cast<std::time_t>(last) < config_.check_interval) {
      log("skip background check (last %lld s ago)",
          static_cast<long long>(now - static_cast<std::time_t>(last)));
      return;
    }
  }
  startCheck(false);
}

void UpdateEngine::checkNow(std::time_t now) {
  (void)now;
  startCheck(true);
}

void UpdateEngine::startCheck(bool interactive) {
  if (checking_) {
    log(interactive ? "manual check ignored (check in flight)"
                    : "skip check (check in flight)");
    if (interactive)
      ui_.showAlert(kCheckFailedTitle, "已有一次更新检查正在进行，请稍候再试。");
    return;
  }
  if (config_.manifest_url.empty()) {
    log("no manifest url configured");
    if (interactive) ui_.showAlert(kCheckFailedTitle, "应用未配置更新地址。");
    return;
  }
  checking_ = true;
  interactive_ = interactive;
  log("GET %s", config_.manifest_url.c_str());
  fetcher_.asyncGet(config_.manifest_url,
                    [this](int status, std::string body, std::string error) {
                      onManifest(status, body, error);
                    });
}

void UpdateEngine::handleFailure(bool interactive, const std::string& reason) {
  log("check failed: %s", reason.c_str());
  if (interactive) ui_.showAlert(kCheckFailedTitle, reason);
}

void UpdateEngine::onManifest(int status, const std::string& body, const std::string& error) {
  if (!checking_) {
    log("ignore out-of-band manifest result");
    return;
  }
  checking_ = false;
  const bool interactive = interactive_;

  // 检查时间戳在拿到响应后写入（无论成败）：失败也节流，避免断网时每次定时器都打服务器
  store_.setString(update_keys::kLastCheckAt,
                   std::to_string(static_cast<long long>(std::time(nullptr))));

  if (!error.empty() || status != 200 || body.empty()) {
    const std::string reason =
        !error.empty() ? error : "服务器返回 HTTP " + std::to_string(status);
    handleFailure(interactive, reason);
    return;
  }

  // 统一顺序：验签+解析（parse_signed_manifest 内部先验签，签名不过直接失败）。
  // 历史上 win 壳先判定后验签、server 与前端各判各的，均属双胞胎偏差，收口于此。
  const SignedManifest manifest = parse_signed_manifest(body, config_.pubkey_hex);
  if (!manifest.ok) {
    log("manifest invalid: %s", manifest.error.c_str());
    handleFailure(interactive, "更新清单校验失败：" + manifest.error);
    return;
  }

  handleDecision(interactive, manifest, body, std::time(nullptr));
}

void UpdateEngine::handleDecision(bool interactive, const SignedManifest& manifest,
                                  const std::string& manifest_text, std::time_t now) {
  LocalUpdateState local;
  local.version = config_.current_version;
  local.build = config_.current_build;
  local.channel = config_.channel;
  local.install_id = installId();
  local.now_epoch = static_cast<std::int64_t>(now);
  // 手动检查无视「稍后」记录；后台检查把跳过窗口交给判据（Skipped 静默）。
  if (!interactive) {
    local.skipped_version = store_.getString(update_keys::kSkippedVersion);
    const std::string until = store_.getString(update_keys::kSkipUntilEpoch);
    local.skip_until_epoch =
        until.empty() ? 0 : static_cast<std::int64_t>(std::strtoll(until.c_str(), nullptr, 10));
  }

  const UpdateDecision decision = evaluate_update(manifest, local);
  log("current=%s latest=%s decision=%s", config_.current_version.c_str(),
      manifest.platform.version.c_str(), decision.reason.c_str());

  switch (decision.verdict) {
  case UpdateDecision::Verdict::NoUpdate: {
    if (!interactive) return;
    if (decision.reason == "version_invalid") {
      handleFailure(interactive, "更新清单格式错误：版本号无法解析。");
      return;
    }
    ui_.showAlert("已是最新版本",
                  "当前版本 " + config_.current_version + " 已经是最新版本。");
    return;
  }
  case UpdateDecision::Verdict::RolloutBlocked:
    log("rollout hold: version=%s percent=%lld", manifest.platform.version.c_str(),
        static_cast<long long>(manifest.platform.rollout_percent));
    if (interactive)
      ui_.showAlert("暂无可用更新",
                    "新版本 " + manifest.platform.version + " 正在分批推送中，请稍后再试。");
    return;
  case UpdateDecision::Verdict::Skipped:
    // 后台检查命中「稍后」窗口：不打扰用户，等窗口过期或下次发布。
    log("skip prompt for version=%s", manifest.platform.version.c_str());
    return;
  case UpdateDecision::Verdict::Available:
    break;
  }

  std::string preTitle, preBody;
  if (!installer_.preflight(&preTitle, &preBody)) {
    log("preflight blocked: %s", preTitle.c_str());
    ui_.showAlert(preTitle, preBody);
    return;
  }

  const PromptSpec spec = buildPrompt(manifest, decision);
  if (ui_.showPrompt(spec) == UpdateUi::Choice::Later) {
    store_.setString(update_keys::kSkippedVersion, decision.version);
    store_.setString(update_keys::kSkipUntilEpoch,
                     std::to_string(static_cast<long long>(now) +
                                    config_.skip_hours * 3600));
    return;
  }
  store_.clearString(update_keys::kSkippedVersion);
  store_.clearString(update_keys::kSkipUntilEpoch);
  installer_.install(manifest, decision, manifest_text);
}

PromptSpec UpdateEngine::buildPrompt(const SignedManifest& manifest,
                                     const UpdateDecision& decision) const {
  const bool force = decision.mandatory_active;
  PromptSpec spec;
  spec.title = (force ? "必须更新到 " : "发现新版本 ") + decision.version;
  spec.notes_url = decision.notes_url;
  spec.force = force;
  spec.allow_later = !force;

  std::string body;
  if (decision.mandatory) {
    // 强制更新：原因始终显示；期限在截止前提示「到期后不能跳过」，到期后直接强制
    body += force ? "当前版本必须升级。" : "该版本为强制更新。";
    const std::string reason = mandatory_reason_label(decision.mandatory_reason);
    if (!reason.empty()) body += "原因：" + reason + "。";
    if (!decision.mandatory_deadline.empty()) {
      body += force
          ? "\n已到截止时间（" + decision.mandatory_deadline + "），无法跳过。"
          : "\n请在 " + decision.mandatory_deadline + " 前完成升级，到期后将无法跳过。";
    }
    body += "\n";
  }
  body += "当前版本 " + config_.current_version + "。";
  const std::string size = formatSize(installer_.preferredArtifactSize(manifest));
  if (!size.empty()) body += "\n安装包大小：" + size;
  if (!config_.install_note.empty()) body += "\n\n" + config_.install_note;
  spec.body = body;
  return spec;
}

}  // namespace devcard
