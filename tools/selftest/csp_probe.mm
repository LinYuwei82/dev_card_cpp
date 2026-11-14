// macOS 上复现「fetch 只报 TypeError: Load failed」的最小探针。
//
// 与 App 走同一个 WebKit：同一个 WKWebView、同一段 CSP，只是把页面换成
// 内联 HTML，避开登录态与数据库。三种模式：
//   none  无 CSP           → 预期 200
//   old   修复前的 CSP      → 预期 TypeError: Load failed
//   new   当前 CSP（含 connect-src）→ 预期 200
//
// 编译与运行见同目录 run_csp_probe.sh。

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

@interface Probe : NSObject <WKNavigationDelegate>
@property(strong) NSString* mode;
@end

@implementation Probe
- (void)webView:(WKWebView*)web didFinishNavigation:(WKNavigation*)nav {
  NSString* js =
      @"try {"
       "const r = await fetch("
       "'https://api.github.com/repos/LinYuwei82/dev_card_cpp/releases/latest',"
       "{cache:'no-store'});"
       "return JSON.stringify({ok:true,status:r.status});"
       "} catch (e) {"
       "return JSON.stringify({ok:false,name:e.name,message:e.message});"
       "}";
  [web callAsyncJavaScript:js
                 arguments:@{}
                   inFrame:nil
            inContentWorld:WKContentWorld.pageWorld
         completionHandler:^(id value, NSError* error) {
           NSString* text = value ? [NSString stringWithFormat:@"%@", value]
                                  : [NSString stringWithFormat:@"ERR %@",
                                                               error.localizedDescription];
           printf("{\"csp\":\"%s\",\"result\":%s}\n", [self.mode UTF8String],
                  [text UTF8String]);
           fflush(stdout);
           exit(0);
         }];
}
@end

int main(int argc, const char* argv[]) {
  @autoreleasepool {
    if (argc < 2) {
      fprintf(stderr, "usage: %s none|old|new\n", argv[0]);
      return 2;
    }
    NSString* mode = [NSString stringWithUTF8String:argv[1]];
    NSString* csp = nil;
    if ([mode isEqualToString:@"old"]) {
      csp = @"default-src 'self'; img-src 'self' data:; style-src 'self'";
    } else if ([mode isEqualToString:@"new"]) {
      csp = @"default-src 'self'; connect-src 'self' https: http:; "
             "img-src 'self' data:; style-src 'self'";
    } else {
      csp = @"";
    }
    NSString* html =
        csp.length == 0
            ? @"<html><body>probe</body></html>"
            : [NSString
                  stringWithFormat:@"<html><head><meta http-equiv=\"Content-Security-Policy\" "
                                   @"content=\"%@\"></head><body>probe</body></html>",
                                   csp];
    [NSApplication sharedApplication];
    WKWebView* web = [[WKWebView alloc] initWithFrame:NSMakeRect(0, 0, 320, 200)];
    Probe* probe = [Probe new];
    probe.mode = mode;
    web.navigationDelegate = probe;
    [web loadHTMLString:html baseURL:[NSURL URLWithString:@"http://127.0.0.1:18080/"]];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 20ull * NSEC_PER_SEC),
                   dispatch_get_main_queue(), ^{
                     printf("{\"csp\":\"%s\",\"result\":{\"ok\":false,"
                            "\"name\":\"Timeout\",\"message\":\"no result\"}}\n",
                            [mode UTF8String]);
                     exit(3);
                   });
    [NSApp run];
  }
}
