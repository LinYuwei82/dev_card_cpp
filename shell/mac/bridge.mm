// macOS 壳的 JS 桥（M3 从 main.mm 拆出）。
#import "bridge.h"

#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#import "about_panel.h"
#import "print_export.h"
#import "shell_state.h"
#import "updater.h"

@implementation DevCardBridge

- (void)userContentController:(WKUserContentController*)controller
      didReceiveScriptMessage:(WKScriptMessage*)message {
  if (![message.body isKindOfClass:[NSDictionary class]]) return;
  NSDictionary* body = (NSDictionary*)message.body;
  NSString* action = body[@"action"];
  if ([action isEqualToString:@"exportPdf"]) {
    [self.printBridge exportPdfNamed:body[@"filename"]];
  } else if ([action isEqualToString:@"print"]) {
    [self.printBridge printCards];
  } else if ([action isEqualToString:@"saveFile"]) {
    [self saveFileNamed:body[@"filename"]
                  title:body[@"title"]
                 base64:body[@"contentBase64"]];
  } else if ([action isEqualToString:@"pickSavePath"]) {
    // 只让用户选落点，不下发字节：更新包由服务端按这个路径落盘（大文件不转 base64）。
    [self pickSavePathNamed:body[@"filename"] title:body[@"title"]];
  } else if ([action isEqualToString:@"openDataDir"]) {
    NSString* path = body[@"path"];
    if ([path isKindOfClass:[NSString class]] && path.length > 0) {
      [[NSWorkspace sharedWorkspace]
          openURL:[NSURL fileURLWithPath:path isDirectory:YES]];
    }
  } else if ([action isEqualToString:@"openExternal"]) {
    // 版本检查的「下载页」跳转：只放行 http/https，不做任何下载动作。
    NSString* value = body[@"url"];
    NSURL* target = [value isKindOfClass:[NSString class]]
                        ? [NSURL URLWithString:value]
                        : nil;
    NSString* scheme = target.scheme.lowercaseString;
    if (target && ([scheme isEqualToString:@"https"] ||
                   [scheme isEqualToString:@"http"])) {
      [[NSWorkspace sharedWorkspace] openURL:target];
    } else {
      [self notify:@"下载页地址无效" kind:@"error"];
    }
  } else if ([action isEqualToString:@"showAbout"]) {
    // 状态栏版本号点击：与 App 菜单「关于 DevCard」同一个面板。
    [self.aboutHandler showAbout:nil];
  } else if ([action isEqualToString:@"checkForUpdates"]) {
    // 原生更新引擎：前端桥触发 core UpdateEngine 立即检查（与菜单同一入口）。
    if (![self.updaterBridge checkForUpdates]) {
      [self notify:@"更新检查未配置（缺少 SUFeedURL / SUPublicEDKey）" kind:@"error"];
    }
  } else if ([action isEqualToString:@"quit"]) {
    // 【回归 Bug 修复｜请勿改动】页面顶栏「退出」在桌面版等于退出应用。
    // 前端的 el.logout 会先尝试走这条桥；如果这里被删掉，点击「退出」会静默
    // 落回前端的注销分支，停在登录页（历史上已经因此产生两次重复反馈）。
    [NSApp terminate:nil];
  }
}

// 页面导出 xlsx（设备台账 / 导入模板）统一走这里：内容由页面取好（带会话），
// 壳层只负责「问路径 + 原子写入」。取消时不写任何字节，也不弹提示，
// 只把 cancel 回给页面用于恢复按钮状态——与小兔进销存的导出桥同一口径。
- (void)saveFileNamed:(NSString*)name
                title:(NSString*)title
               base64:(NSString*)base64 {
  NSData* data = base64.length > 0
                     ? [[NSData alloc] initWithBase64EncodedString:base64 options:0]
                     : nil;
  if (data.length == 0) {
    [self finishSave:@"error" path:nil message:@"导出内容为空"];
    return;
  }
  NSString* filename = name.length > 0 ? name : @"设备台账.xlsx";
  NSSavePanel* panel = [NSSavePanel savePanel];
  panel.title = title.length > 0 ? title : @"保存文件";
  panel.nameFieldStringValue = filename;
  panel.canCreateDirectories = YES;
  NSString* ext = filename.pathExtension.lowercaseString;
  if (ext.length > 0) {
    UTType* type = [UTType typeWithFilenameExtension:ext];
    if (type != nil) panel.allowedContentTypes = @[ type ];
  }

  void (^done)(NSModalResponse) = ^(NSModalResponse result) {
    if (result != NSModalResponseOK || panel.URL == nil) {
      [self finishSave:@"cancel" path:nil message:nil];
      return;
    }
    NSURL* url = panel.URL;
    NSError* error = nil;
    if (![data writeToURL:url options:NSDataWritingAtomic error:&error]) {
      [self finishSave:@"error"
                  path:url.path
               message:error.localizedDescription ?: @"无法写入目标文件"];
      return;
    }
    [self finishSave:@"ok" path:url.path message:nil];
  };
  if (self.window != nil) {
    [panel beginSheetModalForWindow:self.window completionHandler:done];
  } else {
    done([panel runModal]);
  }
}

- (void)pickSavePathNamed:(NSString*)name title:(NSString*)title {
  NSSavePanel* panel = [NSSavePanel savePanel];
  panel.title = title.length > 0 ? title : @"保存文件";
  panel.nameFieldStringValue =
      name.length > 0 ? name : @"DevCard-update";
  panel.canCreateDirectories = YES;
  // 默认落在用户的下载目录（符合大多数人的预期）；没有下载目录就退到文稿，
  // 绝不默认写进应用自己的数据目录或临时目录。
  NSString* dir = NSSearchPathForDirectoriesInDomains(
      NSDownloadsDirectory, NSUserDomainMask, YES).firstObject;
  if (dir.length == 0 ||
      ![[NSFileManager defaultManager] fileExistsAtPath:dir]) {
    dir = NSSearchPathForDirectoriesInDomains(
        NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
  }
  if (dir.length > 0) {
    panel.directoryURL = [NSURL fileURLWithPath:dir isDirectory:YES];
  }
  NSString* ext = panel.nameFieldStringValue.pathExtension.lowercaseString;
  if (ext.length > 0) {
    UTType* type = [UTType typeWithFilenameExtension:ext];
    if (type != nil) panel.allowedContentTypes = @[ type ];
  }
  void (^done)(NSModalResponse) = ^(NSModalResponse result) {
    if (result != NSModalResponseOK || panel.URL == nil) {
      [self finishPath:@"cancel" path:nil];
      return;
    }
    [self finishPath:@"ok" path:panel.URL.path];
  };
  if (self.window != nil) {
    [panel beginSheetModalForWindow:self.window completionHandler:done];
  } else {
    done([panel runModal]);
  }
}

- (void)finishPath:(NSString*)status path:(NSString*)path {
  NSMutableDictionary* payload = [NSMutableDictionary dictionary];
  payload[@"status"] = status ?: @"cancel";
  if (path.length > 0) payload[@"path"] = path;
  NSData* data = [NSJSONSerialization dataWithJSONObject:payload options:0 error:nil];
  NSString* json = data != nil
                       ? [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding]
                       : @"{}";
  NSString* js = [NSString
      stringWithFormat:@"window.__devcardPathPicked && window.__devcardPathPicked(%@);",
                       json];
  [self.webView evaluateJavaScript:js completionHandler:nil];
}

- (void)finishSave:(NSString*)status path:(NSString*)path message:(NSString*)message {
  NSMutableDictionary* payload = [NSMutableDictionary dictionary];
  payload[@"status"] = status ?: @"error";
  if (path.length > 0) payload[@"path"] = path;
  if (message.length > 0) payload[@"message"] = message;
  NSData* data = [NSJSONSerialization dataWithJSONObject:payload options:0 error:nil];
  NSString* json = data != nil
                       ? [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding]
                       : @"{}";
  NSString* js = [NSString
      stringWithFormat:@"window.__devcardSaveResult && window.__devcardSaveResult(%@);",
                       json];
  [self.webView evaluateJavaScript:js completionHandler:nil];
}

- (void)notify:(NSString*)message kind:(NSString*)kind {
  DevCardNotify(self.webView, message, kind);
}

@end
