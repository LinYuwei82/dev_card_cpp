// macOS 壳的打印自检辅助（M4 从 main.mm 拆出）。
#import "print_selftest.h"

#import <PDFKit/PDFKit.h>
#import <WebKit/WebKit.h>

#include <cstdlib>
#include <functional>
#include <memory>

#import "print_export.h"
#import "shell_state.h"

// 轮询函数用 std::function + shared_ptr 自引用：ObjC 块递归 + 在 GCD
// 上下文拷贝块捕获的写法实测触发 _Block_copy(NULL) 段错误（DevCard-*.ips）。
static std::shared_ptr<std::function<void(int)>> g_poll_slot;
static BOOL g_poll_started = NO;

static BOOL DevCardSelftestNoSubtle(void) {
  return NSProcessInfo.processInfo
             .environment[@"DEVCARD_SELFTEST_NOSUBTLE"].length > 0;
}

NSString* DevCardSelftestPdfPath(void) {
  return NSProcessInfo.processInfo
      .environment[@"DEVCARD_PRINT_SELFTEST_PDF"];
}

NSString* DevCardSelftestPagePath(void) {
  if (DevCardSelftestPdfPath().length == 0) return @"/";
  return DevCardSelftestNoSubtle() ? @"/?view=print&selftest_nosubtle=1"
                                   : @"/?view=print";
}

void DevCardSelftestPrepare(WKWebView* web, DevCardPrintBridge* print_bridge) {
  NSString* selftest_pdf = DevCardSelftestPdfPath();
  if (selftest_pdf.length == 0) return;
  const BOOL selftest_nosubtle = DevCardSelftestNoSubtle();

  g_poll_slot = std::make_shared<std::function<void(int)>>();
  auto poll_fn = g_poll_slot;
  *poll_fn = [poll_fn, web, print_bridge, selftest_pdf,
              selftest_nosubtle](int attempts) {
    // 先取安全上下文与闸门的真实状态：不依赖数据，打印视图有没有卡片都能读到。
    [web evaluateJavaScript:@"window.__devcardFontGate ? JSON.stringify(window.__devcardFontGate()) : ''"
          completionHandler:^(id gate, NSError* gate_error) {
            NSString* gate_json = [gate isKindOfClass:NSString.class] ? gate : @"";
            if (gate_json.length > 0 && attempts == 50) {
              NSLog(@"DEVCARD_SELFTEST font_gate=%@", gate_json);
            }
            if (selftest_nosubtle && gate_json.length > 0) {
              // 负向自检：字体未就绪时按钮是禁用的，点按钮不会有回调，所以直接调版式入口，
              // 断言拿到的是「校验机制不可用」这条错误，且没有产出 HTML/PDF。
              [web evaluateJavaScript:@"window.__devcardEnsurePrintFonts()"
                    completionHandler:^(id ignored, NSError* ensure_error) {
                      dispatch_after(
                          dispatch_time(DISPATCH_TIME_NOW,
                                        (int64_t)(0.8 * NSEC_PER_SEC)),
                          dispatch_get_main_queue(), ^{
                            [web evaluateJavaScript:
                                     @"(function(){try{var r=window.__devcardExportLayout();"
                                      "return JSON.stringify({error:(r&&r.error)||'',html:(r&&r.html)?r.html.length:0});}"
                                      "catch(e){return JSON.stringify({thrown:String(e)});}})()"
                                completionHandler:^(id value, NSError* error) {
                                  NSString* json = [value isKindOfClass:NSString.class] ? value : @"";
                                  NSLog(@"DEVCARD_SELFTEST font_gate_export=%@",
                                        json.length ? json
                                                    : (error.localizedDescription ?: @"nil"));
                                  BOOL blocked = [json containsString:@"crypto.subtle"] &&
                                                 [json containsString:@"\"html\":0"];
                                  NSLog(@"DEVCARD_SELFTEST %@", blocked ? @"OK" : @"FAIL");
                                  exit(blocked ? 0 : 1);
                                }];
                          });
                    }];
              return;
            }
            [web evaluateJavaScript:@"document.querySelectorAll('.sheet').length"
                  completionHandler:^(id result, NSError* error) {
                    NSInteger sheets = [result respondsToSelector:@selector(integerValue)]
                                           ? [result integerValue]
                                           : 0;
                    if (sheets > 0) {
                      NSLog(@"DEVCARD_SELFTEST sheets=%ld", (long)sheets);
                      print_bridge.onExportResult = ^(BOOL ok, NSString* message) {
                        if (ok) {
                          // 直接打印通道：用同一份 PDF 构造打印任务（不弹面板）。
                          PDFDocument* document = [[PDFDocument alloc]
                              initWithURL:[NSURL fileURLWithPath:selftest_pdf]];
                          NSPrintOperation* operation =
                              [document printOperationForPrintInfo:DevCardA4PrintInfo()
                                                      scalingMode:kPDFPrintPageScaleNone
                                                       autoRotate:NO];
                          NSLog(@"DEVCARD_SELFTEST print_channel pages=%lu operation=%@",
                                (unsigned long)document.pageCount,
                                operation ? @"ok" : @"nil");
                        }
                        NSLog(@"DEVCARD_SELFTEST %@", ok ? @"OK" : @"FAIL");
                        if (!ok) {
                          NSLog(@"DEVCARD_SELFTEST error=%@", message ?: @"(无消息)");
                        }
                        exit(ok ? 0 : 1);
                      };
                      // 走真实链路：页面按钮 -> WKScriptMessageHandler -> 导出。
                      [web evaluateJavaScript:@"document.getElementById('print-export').click()"
                            completionHandler:nil];
                      return;
                    }
                    if (attempts <= 0) {
                      NSLog(@"DEVCARD_SELFTEST FAIL 打印视图未渲染");
                      exit(2);
                    }
                    dispatch_after(
                        dispatch_time(DISPATCH_TIME_NOW,
                                      (int64_t)(0.3 * NSEC_PER_SEC)),
                        dispatch_get_main_queue(), ^{
                          (*poll_fn)(attempts - 1);
                        });
                  }];
          }];
  };
}

void DevCardSelftestKickoff(WKWebView* web) {
  if (!g_poll_slot || g_poll_started) return;
  // splash→真实页导航进行中时 evaluateJavaScript 会被 WebKit 竞态吞掉。
  if (![web.URL.absoluteString hasPrefix:@"http://"]) return;
  g_poll_started = YES;
  auto poll_fn = g_poll_slot;
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.3 * NSEC_PER_SEC)),
                 dispatch_get_main_queue(), ^{
                   (*poll_fn)(50);
                 });
}
