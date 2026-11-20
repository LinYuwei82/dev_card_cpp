#import "updater.h"

// Sparkle 更新事件日志代理。只做观测，不改变 Sparkle 的默认行为。
@interface DevCardUpdaterDelegate : NSObject <SPUUpdaterDelegate>
@end

@implementation DevCardUpdaterDelegate

- (void)updater:(SPUUpdater*)updater didFindValidUpdate:(SUAppcastItem*)item {
  NSLog(@"[sparkle] didFindValidUpdate version=%@ display=%@ url=%@",
        item.versionString, item.displayVersionString, item.fileURL);
}

- (void)updaterDidNotFindUpdate:(SPUUpdater*)updater error:(NSError*)error {
  NSLog(@"[sparkle] didNotFindUpdate error=%@", error.localizedDescription);
}

- (void)updater:(SPUUpdater*)updater
    didFailToFetchAppcastWithError:(NSError*)error {
  NSLog(@"[sparkle] didFailToFetchAppcast error=%@ code=%ld",
        error.localizedDescription, (long)error.code);
}

- (void)updater:(SPUUpdater*)updater didDownloadUpdate:(SUAppcastItem*)item {
  NSLog(@"[sparkle] didDownloadUpdate version=%@", item.versionString);
}

- (void)updater:(SPUUpdater*)updater willInstallUpdate:(SUAppcastItem*)item {
  NSLog(@"[sparkle] willInstallUpdate version=%@", item.versionString);
}

- (void)updater:(SPUUpdater*)updater didAbortWithError:(NSError*)error {
  NSLog(@"[sparkle] didAbortWithError error=%@ code=%ld",
        error.localizedDescription, (long)error.code);
}

@end

@implementation DevCardUpdaterBridge

- (void)start {
  // Sparkle 2 自更新：Info.plist 里必须有 SUFeedURL + SUPublicEDKey。
  // 开发期两键为空时跳过——updaterController 是 nil，前端调 checkForUpdates 会收到提示。
  NSDictionary* bundle_info = [NSBundle mainBundle].infoDictionary;
  NSString* feed_url = bundle_info[@"SUFeedURL"];
  NSString* pub_key = bundle_info[@"SUPublicEDKey"];

  if (feed_url.length > 0 && pub_key.length > 0) {
    DevCardUpdaterDelegate* del = [[DevCardUpdaterDelegate alloc] init];
    self.updaterDelegate = del;
    self.updaterController =
        [[SPUStandardUpdaterController alloc] initWithStartingUpdater:YES
                                                        updaterDelegate:del
                                                    userDriverDelegate:nil];
    NSLog(@"[sparkle] 初始化完成 feed=%@", feed_url);
  } else {
    NSLog(@"[sparkle] 跳过初始化：SUFeedURL=%@ SUPublicEDKey=%@",
          feed_url.length > 0 ? feed_url : @"(空)",
          pub_key.length > 0 ? @"(已填)" : @"(空)");
  }

  // 绑定 App 菜单「检查更新…」的 target。
  // Sparkle 启用时 target 设为 updaterController（它自己实现 checkForUpdates: + validateMenuItem:）；
  // 未启用时把菜单项隐藏，避免用户点了报错。
  if (self.checkUpdateMenuItem) {
    if (self.updaterController) {
      self.checkUpdateMenuItem.target = self.updaterController;
      self.checkUpdateMenuItem.hidden = NO;
    } else {
      self.checkUpdateMenuItem.hidden = YES;
    }
  }

  // 自动化验证：DEVCARD_SPARKLE_SELFTEST=1 时启动后自动发起一次更新检查，
  // 便于脚本核对 feed 拉取、EdDSA 校验与版本比较。
  if (self.updaterController &&
      [NSProcessInfo.processInfo.environment[@"DEVCARD_SPARKLE_SELFTEST"]
          isEqualToString:@"1"]) {
    NSLog(@"[sparkle] SELFTEST 准备触发 checkForUpdates");
    dispatch_after(
        dispatch_time(DISPATCH_TIME_NOW, (int64_t)(3 * NSEC_PER_SEC)),
        dispatch_get_main_queue(), ^{
          NSLog(@"[sparkle] SELFTEST 触发 checkForUpdates");
          [self.updaterController checkForUpdates:nil];
        });
  }
}

- (BOOL)checkForUpdates {
  if (self.updaterController) {
    [self.updaterController checkForUpdates:nil];
    return YES;
  }
  NSLog(@"[sparkle] 未启用，跳过 checkForUpdates");
  return NO;
}

@end
