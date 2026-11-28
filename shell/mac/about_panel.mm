// macOS 壳「关于」面板的实现（M1 从 main.mm 拆出）。
#include "about_panel.h"

#include "devcard/update_manifest.h"

@implementation DevCardAboutHelper
- (void)showAbout:(id)sender {
  (void)sender;
  const std::string ver = devcard::current_product_version();
  const uint64_t build = devcard::compiled_build_number();
  NSString* version_line =
      build > 0
          ? [NSString stringWithFormat:@"版本 v%@（构建 %llu）",
                                       [NSString stringWithUTF8String:ver.c_str()],
                                       (unsigned long long)build]
          : [NSString stringWithFormat:@"版本 v%@",
                                        [NSString stringWithUTF8String:ver.c_str()]];
  NSAlert* alert = [[NSAlert alloc] init];
  alert.icon = NSApp.applicationIconImage;
  alert.messageText = @"DevCard 设备管理卡";
  alert.informativeText =
      [NSString stringWithFormat:@"%@\n物业设备台账与设备管理卡打印工具\n\n© 2026 小兔在线",
                                 version_line];
  [alert addButtonWithTitle:@"好"];
  [alert addButtonWithTitle:@"更新记录"];
  [alert addButtonWithTitle:@"用户手册"];
  [alert addButtonWithTitle:@"发布页"];
  NSModalResponse response = [alert runModal];
  NSString* url = nil;
  if (response == NSAlertSecondButtonReturn) {
    url = [NSString stringWithFormat:
               @"https://dl.xiaotu.online/devcard/release-notes-%@.html",
               [NSString stringWithUTF8String:ver.c_str()]];
  } else if (response == NSAlertThirdButtonReturn) {
    url = @"https://dl.xiaotu.online/devcard/manual.pdf";
  } else if (response == NSAlertFirstButtonReturn + 3) {  // 第 4 个按钮（SDK 无 Fourth 常量）
    url = @"https://github.com/LinYuwei82/dev_card_cpp/releases";
  }
  if (url) {
    [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:url]];
  }
}
@end

DevCardAboutHelper* DevCardSharedAboutHandler(void) {
  static DevCardAboutHelper* handler = [DevCardAboutHelper new];
  return handler;
}
