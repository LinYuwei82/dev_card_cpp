#import "updater.h"

// macOS 更新适配器：只实现「网络 / 持久化 / 原生弹窗 / Sparkle 安装」四件事，
// 状态机（时机、节流、验签→解析→判定、跳过规则、文案）全部在 devcard::UpdateEngine，
// 与 Windows 共用同一份，见 core/include/devcard/update_engine.h。

#include "devcard/update_engine.h"
#include "devcard/update_manifest.h"

#import <Sparkle/Sparkle.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

namespace {

const NSTimeInterval kStartupDelay = 10.0;
const NSTimeInterval kCheckInterval = 24 * 60 * 60;
const NSTimeInterval kRequestTimeout = 15.0;

// NSUserDefaults 键（带产品前缀，避免与其他 App 的全局域冲突）
NSString* const kLastCheckAtKey = @"DevCardLastUpdateCheckAt";
NSString* const kSkippedVersionKey = @"DevCardSkippedUpdateVersion";
NSString* const kSkipUntilEpochKey = @"DevCardSkippedUntilEpoch";
NSString* const kInstallIdKey = @"DevCardInstallId";

bool LogEnabled() {
  return std::getenv("DEVCARD_UPDATE_LOG") != nullptr;
}

void EngineLog(const std::string& line) {
  if (LogEnabled()) NSLog(@"%s", line.c_str());
}

void ShellLog(const char* format, ...) __attribute__((format(printf, 1, 2)));
void ShellLog(const char* format, ...) {
  if (!LogEnabled()) return;
  char buffer[512] = {};
  va_list args;
  va_start(args, format);
  std::vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  NSLog(@"NATIVE update %s", buffer);
}

NSString* ToStr(const std::string& text) {
  return [NSString stringWithUTF8String:text.c_str()] ?: @"";
}
std::string FromStr(NSString* text) {
  return text ? std::string(text.UTF8String) : std::string();
}

// ---- 四个平台适配器 ----

class MacFetcher : public devcard::ManifestFetcher {
 public:
  void asyncGet(const std::string& url, Callback done) override {
    NSURL* nsUrl = [NSURL URLWithString:ToStr(url)];
    if (!nsUrl) {
      if (done) done(0, "", "更新地址不合法");
      return;
    }
    NSMutableURLRequest* request = [NSMutableURLRequest requestWithURL:nsUrl];
    request.timeoutInterval = kRequestTimeout;
    request.cachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
    NSURLSessionDataTask* task = [NSURLSession.sharedSession
        dataTaskWithRequest:request
          completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
            const NSInteger status = [response isKindOfClass:NSHTTPURLResponse.class]
                                         ? ((NSHTTPURLResponse*)response).statusCode
                                         : 0;
            const std::string body(data ? (const char*)data.bytes : "",
                                   data ? data.length : 0);
            const std::string reason =
                error ? FromStr(error.localizedDescription) : std::string();
            // 引擎要求结果回到主线程
            dispatch_async(dispatch_get_main_queue(), ^{
              if (done) done((int)status, body, reason);
            });
          }];
    [task resume];
  }
};

class MacStore : public devcard::SettingsStore {
 public:
  std::string getString(const std::string& key) override {
    return FromStr([NSUserDefaults.standardUserDefaults stringForKey:DefaultKey(key)]);
  }
  void setString(const std::string& key, const std::string& value) override {
    [NSUserDefaults.standardUserDefaults setObject:ToStr(value) forKey:DefaultKey(key)];
  }
  void clearString(const std::string& key) override {
    [NSUserDefaults.standardUserDefaults removeObjectForKey:DefaultKey(key)];
  }
  std::string newInstallId() override { return FromStr(NSUUID.UUID.UUIDString); }

 private:
  static NSString* DefaultKey(const std::string& key) {
    if (key == devcard::update_keys::kLastCheckAt) return kLastCheckAtKey;
    if (key == devcard::update_keys::kSkippedVersion) return kSkippedVersionKey;
    if (key == devcard::update_keys::kSkipUntilEpoch) return kSkipUntilEpochKey;
    return kInstallIdKey;
  }
};

class MacUi : public devcard::UpdateUi {
 public:
  void showAlert(const std::string& title, const std::string& body) override {
    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = ToStr(title);
    alert.informativeText = ToStr(body);
    alert.alertStyle = NSAlertStyleInformational;
    [alert addButtonWithTitle:@"好"];
    [NSApp activateIgnoringOtherApps:YES];
    [alert runModal];
  }

  Choice showPrompt(const devcard::PromptSpec& spec) override {
    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = ToStr(spec.title);
    alert.informativeText = ToStr(spec.body);
    alert.alertStyle = spec.force ? NSAlertStyleCritical : NSAlertStyleInformational;
    [alert addButtonWithTitle:spec.force ? @"立即更新" : @"下载并安装"];
    if (spec.allow_later) [alert addButtonWithTitle:@"稍后"];

    if (!spec.notes_url.empty()) {
      NSURL* notesUrl = [NSURL URLWithString:ToStr(spec.notes_url)];
      if (notesUrl) {
        NSString* linkText = @"查看完整更新说明…";
        NSMutableAttributedString* attr =
            [[NSMutableAttributedString alloc] initWithString:linkText];
        [attr addAttribute:NSLinkAttributeName value:notesUrl
                     range:NSMakeRange(0, linkText.length)];
        [attr addAttribute:NSFontAttributeName
                     value:[NSFont systemFontOfSize:[NSFont smallSystemFontSize]]
                     range:NSMakeRange(0, linkText.length)];
        NSTextField* link = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 240, 16)];
        [link setEditable:NO];
        [link setSelectable:YES];
        link.allowsEditingTextAttributes = YES;
        link.drawsBackground = NO;
        link.bordered = NO;
        link.attributedStringValue = attr;
        alert.accessoryView = link;
      }
    }

    [NSApp activateIgnoringOtherApps:YES];
    return [alert runModal] == NSAlertFirstButtonReturn ? Choice::Install : Choice::Later;
  }
};

class MacInstaller : public devcard::PlatformInstaller {
 public:
  std::function<void()> triggerSparkle;  // 由 DevCardUpdaterBridge 注入

  bool preflight(std::string* title, std::string* body) override {
    // Gatekeeper 会把带 quarantine 的应用放到 AppTranslocation 只读路径运行，
    // 此时 Sparkle 无法替换 bundle。提前识别，给出可执行的指引。
    if ([NSBundle.mainBundle.bundlePath containsString:@"/AppTranslocation/"]) {
      *title = "需要先移动到“应用程序”";
      *body = "DevCard 正在从系统临时位置运行，无法自动安装。\n\n"
              "请退出 DevCard，把它拖到“应用程序”文件夹，重新打开后再检查更新。";
      return false;
    }
    return true;
  }

  long long preferredArtifactSize(const devcard::SignedManifest& manifest) override {
    for (const devcard::ManifestArtifact& artifact : manifest.platform.artifacts) {
      if (artifact.role == "dmg") return static_cast<long long>(artifact.size);
    }
    return 0;
  }

  void install(const devcard::SignedManifest& manifest, const devcard::UpdateDecision&,
               const std::string&) override {
    ShellLog("install via Sparkle version=%s", manifest.platform.version.c_str());
    if (triggerSparkle) triggerSparkle();
  }
};

// 清单地址：DEVCARD_UPDATE_URL 覆盖（本地联调），否则从 SUFeedURL 推导同目录 manifest.json。
// 开发期两键缺失 → 空串，引擎给出「应用未配置更新地址」。
std::string ResolveManifestUrl() {
  const char* overrideUrl = std::getenv("DEVCARD_UPDATE_URL");
  if (overrideUrl && *overrideUrl) return overrideUrl;
  NSString* feed = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"SUFeedURL"];
  NSString* pub = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"SUPublicEDKey"];
  if (feed.length == 0 || pub.length == 0) return std::string();
  NSURL* feedUrl = [NSURL URLWithString:feed];
  if (!feedUrl) return std::string();
  return FromStr([[feedUrl URLByDeletingLastPathComponent]
      URLByAppendingPathComponent:@"manifest.json"].absoluteString);
}

}  // namespace

@interface DevCardUpdaterBridge () <SPUUpdaterDelegate>
@end

@implementation DevCardUpdaterBridge {
  BOOL _started;
  std::unique_ptr<MacFetcher> _fetcher;
  std::unique_ptr<MacStore> _store;
  std::unique_ptr<MacUi> _ui;
  std::unique_ptr<MacInstaller> _installer;
  std::unique_ptr<devcard::UpdateEngine> _engine;
  SPUStandardUpdaterController* _sparkleController;
}

- (void)start {
  if (_started) return;
  _started = YES;

  // 菜单绑定：清单地址可解析（Sparkle 两键齐全或环境变量覆盖）才显示并接管，
  // 开发期缺配置直接隐藏，避免用户点了报错。
  const bool configured = !ResolveManifestUrl().empty();
  if (self.checkUpdateMenuItem) {
    self.checkUpdateMenuItem.hidden = !configured;
    if (configured) {
      self.checkUpdateMenuItem.target = self;
      self.checkUpdateMenuItem.action = @selector(checkUpdateMenuAction:);
    }
  }
  if (!configured) {
    NSLog(@"[update] 跳过初始化：SUFeedURL / SUPublicEDKey 未配置");
    return;
  }

  // 引擎懒建：不占启动关键路径，首个 tick（启动后 10s）才建。
  // 本文件按 MRC 编译（DevCard target 未开 -fobjc-arc），不能用 __weak；
  // bridge 与 app 同生命周期、永不释放，块直接捕获 self 不构成悬挂。
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(kStartupDelay * NSEC_PER_SEC)),
                 dispatch_get_main_queue(), ^{
                   [self tick];
                 });
  [NSTimer scheduledTimerWithTimeInterval:kCheckInterval
                                  repeats:YES
                                    block:^(NSTimer* timer) {
                                      (void)timer;
                                      [self tick];
                                    }];

  // 发布终验钩子：DEVCARD_SPARKLE_SELFTEST=1 时启动后自动触发一次 Sparkle
  // 检查（appcast 拉取 + EdDSA 校验），便于脚本核对安装执行器链路。
  if ([[NSProcessInfo.processInfo environment][@"DEVCARD_SPARKLE_SELFTEST"]
          isEqualToString:@"1"]) {
    NSLog(@"[sparkle] SELFTEST 准备触发 checkForUpdates");
    dispatch_after(
        dispatch_time(DISPATCH_TIME_NOW, (int64_t)(3 * NSEC_PER_SEC)),
        dispatch_get_main_queue(), ^{
          NSLog(@"[sparkle] SELFTEST 触发 checkForUpdates");
          [[[self sparkleController] updater] checkForUpdates];
        });
  }
}

- (void)buildEngine {
  _fetcher = std::make_unique<MacFetcher>();
  _store = std::make_unique<MacStore>();
  _ui = std::make_unique<MacUi>();
  _installer = std::make_unique<MacInstaller>();
  _installer->triggerSparkle = [self] {
    [[[self sparkleController] updater] checkForUpdates];
  };

  devcard::UpdateEngineConfig config;
  config.manifest_url = ResolveManifestUrl();
  config.current_version = devcard::current_product_version();
  config.current_build = devcard::compiled_build_number();
  config.channel = "stable";
  config.os_label = "macOS";
  config.install_note = "安装过程会退出并重新打开 DevCard。";
  _engine = std::make_unique<devcard::UpdateEngine>(std::move(config), *_fetcher, *_store,
                                                    *_ui, *_installer, EngineLog);
}

- (void)tick {
  // 懒建：缺这个判空就会在定时器首跳时空指针崩溃（mote 1.0.27 回归教训）。
  if (!_engine) [self buildEngine];
  _engine->tick(std::time(nullptr));
}

- (void)checkUpdateMenuAction:(id)sender {
  (void)sender;
  [self checkForUpdates];
}

- (BOOL)checkForUpdates {
  if (ResolveManifestUrl().empty()) {
    NSLog(@"[update] 未配置清单地址，跳过 checkForUpdates");
    return NO;
  }
  if (!_engine) [self buildEngine];
  // 在途防重由引擎处理（会给「已有一次更新检查正在进行」提示）
  _engine->checkNow(std::time(nullptr));
  return YES;
}

// 更新 UI 由自研清单链路负责（引擎的 NSAlert），Sparkle 只当安装执行器：
// 结果回调静音，它负责下载、EdDSA 校验与替换。
- (SPUStandardUpdaterController*)sparkleController {
  if (_sparkleController == nil) {
    _sparkleController = [[SPUStandardUpdaterController alloc]
        initWithStartingUpdater:YES
                updaterDelegate:self
            userDriverDelegate:nil];
  }
  return _sparkleController;
}

- (void)updater:(SPUUpdater*)updater didFindValidUpdate:(SUAppcastItem*)item {
  NSLog(@"[sparkle] didFindValidUpdate version=%@", item.versionString);
}

- (void)updaterDidNotFindUpdate:(SPUUpdater*)updater error:(NSError*)error {
  NSLog(@"[sparkle] didNotFindUpdate error=%@", error.localizedDescription);
}

- (void)updater:(SPUUpdater*)updater didDownloadUpdate:(SUAppcastItem*)item {
  NSLog(@"[sparkle] didDownloadUpdate version=%@", item.versionString);
}

- (void)updater:(SPUUpdater*)updater willInstallUpdate:(SUAppcastItem*)item {
  NSLog(@"[sparkle] willInstallUpdate version=%@", item.versionString);
}

- (void)updater:(SPUUpdater*)updater didAbortWithError:(NSError*)error {
  NSLog(@"[sparkle] didAbortWithError error=%@ code=%ld", error.localizedDescription,
        (long)error.code);
}

@end
