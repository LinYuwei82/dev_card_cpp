// macOS 壳的下载与页面原生面板宿主（M2 从 main.mm 拆出）。
#import "download_handler.h"

#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

@implementation DevCardDownloadHandler

// JS 的 alert/confirm 只有实现这几个回调才会真的弹出；不实现时 WebKit 直接
// 当作用户取消（confirm 返回 false、alert 什么都不显示），删除账号 / 恢复备份
// 就会变成「点了没反应」。统一挂成窗口 sheet，modal 结束再回报结果。
- (void)presentPanelAlert:(NSAlert*)alert
               completion:(void (^)(NSModalResponse))completion {
  if (self.window != nil) {
    [alert beginSheetModalForWindow:self.window completionHandler:completion];
  } else {
    completion([alert runModal]);
  }
}

// 确认按钮按调用点语义命名：页面里的 confirm 文案已写明动作，这里只做识别，
// 认不出时退回「确定」。取消永远是默认按钮，回车不会误执行破坏性操作。
static NSString* DevCardConfirmActionTitle(NSString* message) {
  if ([message rangeOfString:@"删除"].location != NSNotFound) return @"删除";
  if ([message rangeOfString:@"覆盖"].location != NSNotFound) return @"覆盖";
  if ([message rangeOfString:@"恢复"].location != NSNotFound) return @"恢复";
  return @"确定";
}

- (void)webView:(WKWebView*)webView
    runJavaScriptAlertPanelWithMessage:(NSString*)message
                      initiatedByFrame:(WKFrameInfo*)frame
                     completionHandler:(void (^)(void))completionHandler
    API_AVAILABLE(macos(10.12)) {
  NSAlert* alert = [[NSAlert alloc] init];
  alert.messageText = @"设备管理卡";
  alert.informativeText = message.length > 0 ? message : @"";
  [alert addButtonWithTitle:@"好"];
  [self presentPanelAlert:alert
               completion:^(NSModalResponse response) { completionHandler(); }];
}

- (void)webView:(WKWebView*)webView
    runJavaScriptConfirmPanelWithMessage:(NSString*)message
                        initiatedByFrame:(WKFrameInfo*)frame
                       completionHandler:(void (^)(BOOL))completionHandler
    API_AVAILABLE(macos(10.12)) {
  NSString* action = DevCardConfirmActionTitle(message);
  NSAlert* alert = [[NSAlert alloc] init];
  alert.messageText = @"确认操作";
  alert.informativeText = message.length > 0 ? message : @"";
  [alert addButtonWithTitle:@"取消"];
  [alert addButtonWithTitle:action];
  [self presentPanelAlert:alert completion:^(NSModalResponse response) {
    completionHandler(response == NSAlertSecondButtonReturn);
  }];
}

// 导入用的文件选择：WKWebView 不实现这个方法时，点 <input type=file> 完全无反应
// （既不弹面板也不报错）。面板由壳层出，选中后把 URL 交回 WebKit，页面照常收到
// change 事件再上传给服务端；取消则回 nil，页面什么也不做。
- (void)webView:(WKWebView*)webView
    runOpenPanelWithParameters:(WKOpenPanelParameters*)parameters
              initiatedByFrame:(WKFrameInfo*)frame
             completionHandler:(void (^)(NSArray<NSURL*>* _Nullable))completionHandler
    API_AVAILABLE(macos(10.12)) {
  NSOpenPanel* panel = [NSOpenPanel openPanel];
  panel.title = @"选择要导入的设备台账";
  panel.message = @"仅支持 .xlsx（导入模板由「下载模板」生成）";
  panel.prompt = @"导入";
  panel.canChooseFiles = YES;
  panel.canChooseDirectories = NO;
  panel.allowsMultipleSelection = NO;
  panel.allowsOtherFileTypes = NO;
  // 服务端只解析 xlsx（xlsx::read），放 csv 进来只会选完再报错。
  UTType* xlsx = [UTType typeWithFilenameExtension:@"xlsx"];
  if (xlsx != nil) panel.allowedContentTypes = @[ xlsx ];

  void (^done)(NSModalResponse) = ^(NSModalResponse result) {
    NSURL* url = (result == NSModalResponseOK) ? panel.URL : nil;
    if (url != nil && url.path.length > 0) {
      completionHandler(@[ url ]);
    } else {
      completionHandler(nil);
    }
  };
  if (self.window != nil) {
    [panel beginSheetModalForWindow:self.window completionHandler:done];
  } else {
    done([panel runModal]);
  }
}

- (void)webView:(WKWebView*)webView
    decidePolicyForNavigationAction:(WKNavigationAction*)navigationAction
                    preferences:(WKWebpagePreferences*)preferences
            decisionHandler:(void (^)(WKNavigationActionPolicy,
                                      WKWebpagePreferences*))decisionHandler {
  if (navigationAction.shouldPerformDownload) {
    decisionHandler(WKNavigationActionPolicyDownload, preferences);
    return;
  }
  decisionHandler(WKNavigationActionPolicyAllow, preferences);
}

- (void)webView:(WKWebView*)webView
    navigationAction:(WKNavigationAction*)navigationAction
    didBecomeDownload:(WKDownload*)download API_AVAILABLE(macos(11.3)) {
  download.delegate = self;
}

- (void)webView:(WKWebView*)webView
    navigationResponse:(WKNavigationResponse*)navigationResponse
     didBecomeDownload:(WKDownload*)download API_AVAILABLE(macos(11.3)) {
  download.delegate = self;
}

- (void)download:(WKDownload*)download
    decideDestinationUsingResponse:(NSURLResponse*)response
                 suggestedFilename:(NSString*)suggestedFilename
              completionHandler:(void (^)(NSURL* _Nullable))completionHandler
    API_AVAILABLE(macos(11.3)) {
  NSString* name = suggestedFilename.length > 0 ? suggestedFilename : @"download.xlsx";
  NSURL* directory = [[NSFileManager defaultManager]
      URLForDirectory:NSDownloadsDirectory
             inDomain:NSUserDomainMask
    appropriateForURL:nil
               create:YES
                error:nil];
  completionHandler([directory URLByAppendingPathComponent:name]);
}

- (void)webView:(WKWebView*)webView didFinishNavigation:(WKNavigation*)navigation {
  if (self.onDidFinish) self.onDidFinish();
}

@end
