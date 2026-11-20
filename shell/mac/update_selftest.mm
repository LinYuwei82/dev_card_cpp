// DevCard 自动更新自检：从 main.mm 抽出的独立诊断模块，只负责驱动页面自检，
// 不参与正式更新流程（正式更新由 shell/mac/updater.mm 的 DevCardUpdaterBridge 负责）。

#import "update_selftest.h"

#import <WebKit/WebKit.h>

#include <cstdlib>

// 自动更新自检：四种失败路径（超时 / DNS 非 JSON / 版本号非法）必须全静默。
// 状态放文件级变量而不是 __block：跨回调的 __block 状态在本项目第一次实现时直接
// SIGSEGV（dispatch 回调里调到了空 block），换成静态变量后不再依赖编译器对
// ObjC++ 下 __block 的处理。
static WKWebView* g_update_selftest_web = nil;
static BOOL g_update_selftest_started = NO;

// replay 模式：把已抓到的真实 Release 响应体喂进页面的解析/比较/提示链路，
// 再用 WKWebView 自己的快照落 PNG——不经过窗口层级，绕开「Trae 永远在前」的问题。
static void devcard_poll_replay(int attempts) {
  WKWebView* web = g_update_selftest_web;
  NSString* fixture = NSProcessInfo.processInfo.environment[@"DEVCARD_UPDATE_FIXTURE"];
  if (fixture.length == 0) {
    fixture = @"/tmp/m6_release_fixture.json";
  }
  NSString* body = [NSString stringWithContentsOfFile:fixture
                                             encoding:NSUTF8StringEncoding
                                                error:nil];
  if (!web || body.length == 0) {
    NSLog(@"DEVCARD_SELFTEST replay_fail 读不到 fixture：%@", fixture);
    exit(2);
  }
  [web evaluateJavaScript:@"typeof window.__devcardUpdateReplay"
        completionHandler:^(id value, NSError* error) {
          NSString* kind = [value isKindOfClass:NSString.class] ? value : @"";
          if (![kind isEqualToString:@"function"]) {
            if (attempts <= 0) {
              NSLog(@"DEVCARD_SELFTEST replay_fail 页面未就绪");
              exit(2);
            }
            dispatch_after(
                dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)),
                dispatch_get_main_queue(), ^{
                  devcard_poll_replay(attempts - 1);
                });
            return;
          }
          NSData* encoded = [NSJSONSerialization dataWithJSONObject:@[ body ]
                                                           options:0
                                                             error:nil];
          NSString* literal = [[NSString alloc] initWithData:encoded
                                                    encoding:NSUTF8StringEncoding];
          NSString* call =
              [NSString stringWithFormat:@"window.__devcardUpdateReplay((%@)[0])", literal];
          [web evaluateJavaScript:call
                completionHandler:^(id result, NSError* call_error) {
                  NSString* summary =
                      [result isKindOfClass:NSString.class] ? result : @"{}";
                  NSLog(@"DEVCARD_SELFTEST replay=%@", summary);
                  [web evaluateJavaScript:
                           @"(function(){var p=document.getElementById('update-pill');"
                            "if(!p||p.hidden)return 'no-pill';p.click();return 'clicked';})()"
                        completionHandler:^(id clicked, NSError* click_error) {
                          NSLog(@"DEVCARD_SELFTEST replay_ui=%@",
                                [clicked isKindOfClass:NSString.class] ? clicked : @"?");
                          dispatch_after(
                              dispatch_time(DISPATCH_TIME_NOW,
                                            (int64_t)(0.8 * NSEC_PER_SEC)),
                              dispatch_get_main_queue(), ^{
                                WKSnapshotConfiguration* config =
                                    [[WKSnapshotConfiguration alloc] init];
                                [web takeSnapshotWithConfiguration:config
                                                 completionHandler:^(NSImage* image,
                                                                     NSError* snap_error) {
                                                   if (image == nil) {
                                                     NSLog(@"DEVCARD_SELFTEST replay_fail 快照失败：%@",
                                                           snap_error.localizedDescription
                                                               ?: @"(无图)");
                                                   } else {
                                                     NSBitmapImageRep* rep =
                                                         [NSBitmapImageRep imageRepWithData:
                                                                               [image TIFFRepresentation]];
                                                     NSData* png = [rep representationUsingType:
                                                                            NSBitmapImageFileTypePNG
                                                                                    properties:@{}];
                                                     NSString* png_path =
                                                         @"/tmp/m6_update_ui.png";
                                                     [png writeToFile:png_path atomically:YES];
                                                     NSLog(@"DEVCARD_SELFTEST replay_shot=%@ (%lu B)",
                                                           png_path,
                                                           (unsigned long)png.length);
                                                   }
                                                   [summary writeToFile:@"/tmp/m6_update_replay.json"
                                                             atomically:YES
                                                               encoding:NSUTF8StringEncoding
                                                                  error:nil];
                                                   BOOL ok =
                                                       [summary rangeOfString:@"\"ok\":true"]
                                                           .location != NSNotFound;
                                                   NSLog(@"DEVCARD_SELFTEST %@",
                                                         ok ? @"OK" : @"FAIL");
                                                   exit(ok ? 0 : 1);
                                                 }];
                              });
                        }];
                }];
        }];
}

static void devcard_poll_update_step(int attempts);

// 超时诊断再进一步：直接问页面「你自己的 /api/version 拿不拿得到」。
// 更新检查失败只报 Load failed，分不清是会话丢了，还是请求被拦在网络上。
static void devcard_live_api_probe(WKWebView* web, int attempts, NSString* out_path) {
  NSString* js =
      @"(function(){"
       "if(!window.__devcardApiProbe){"
       "window.__devcardApiProbe='pending';"
       "fetch('/api/version',{cache:'no-store',credentials:'same-origin'})"
       ".then(function(r){return r.text().then(function(t){"
       "window.__devcardApiProbe='HTTP '+r.status+' '+t.slice(0,160);});})"
       ".catch(function(e){window.__devcardApiProbe='ERR '+e.name+': '+e.message;});}"
       "return window.__devcardApiProbe;})()";
  [web evaluateJavaScript:js
        completionHandler:^(id value, NSError* error) {
          NSString* probe = [value isKindOfClass:NSString.class] ? value : @"?";
          if ([probe isEqualToString:@"pending"] && attempts > 0) {
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
                                         (int64_t)(0.3 * NSEC_PER_SEC)),
                           dispatch_get_main_queue(), ^{
                             devcard_live_api_probe(web, attempts - 1, out_path);
                           });
            return;
          }
          [web evaluateJavaScript:@"(window.__devcardUpdateDiag ? window.__devcardUpdateDiag() : '{}')"
                completionHandler:^(id diag, NSError* diag_error) {
                  NSString* body = [diag isKindOfClass:NSString.class] ? diag : @"{}";
                  NSData* encoded = [NSJSONSerialization dataWithJSONObject:@[ probe ]
                                                                   options:0
                                                                     error:nil];
                  NSString* quoted = [[NSString alloc] initWithData:encoded
                                                          encoding:NSUTF8StringEncoding];
                  NSString* text_out = [NSString
                      stringWithFormat:@"{\"status\":\"no-result\",\"api_probe\":%@,"
                                       @"\"diag\":%@}",
                                       [quoted substringWithRange:NSMakeRange(1, quoted.length - 2)],
                                       body];
                  [text_out writeToFile:out_path
                              atomically:YES
                                encoding:NSUTF8StringEncoding
                                   error:nil];
                  NSLog(@"DEVCARD_SELFTEST update_live=%@", text_out);
                  exit(2);
                }];
        }];
}

// live 模式：不替换传输层，直接打真实发布接口，把请求地址、响应原文长度与片段、
// 解析结果打印出来。留在前台不退出，便于同时截屏——验收记录要的是「真实元数据 + 界面」。
static void devcard_poll_update_live(int attempts) {
  WKWebView* web = g_update_selftest_web;
  if (!web) {
    NSLog(@"DEVCARD_SELFTEST update_fail 没有页面");
    exit(2);
  }
  NSString* probe =
      @"(function(){"
       "var s=window.__devcardUpdateState?window.__devcardUpdateState():null;"
       "var c=s&&s.check;"
       "if(!c||!c.status)return '';"
       "return JSON.stringify({version:s.version,pill:s.pill,endpoint:c.endpoint||'',"
       "status:c.status||'',rawLen:(c.raw||'').length,rawHead:(c.raw||'').slice(0,200),"
       "release:c.release||null,detail:c.detail||''});})()";
  [web evaluateJavaScript:probe
        completionHandler:^(id value, NSError* error) {
          NSString* text = [value isKindOfClass:NSString.class] ? value : @"";
          if (text.length == 0) {
            if (attempts <= 0) {
              // 把中间状态写盘：只报「超时」不足以判断卡在哪一步。
              NSString* out_path =
                  NSProcessInfo.processInfo.environment[@"DEVCARD_SELFTEST_UPDATE_OUT"];
              if (out_path.length == 0) {
                out_path = @"/tmp/m6_update_live.json";
              }
              devcard_live_api_probe(web, 20, out_path);
              return;
            }
            dispatch_after(
                dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)),
                dispatch_get_main_queue(), ^{
                  devcard_poll_update_live(attempts - 1);
                });
            return;
          }
          NSLog(@"DEVCARD_SELFTEST update_live=%@", text);
          // 同时落盘：NSLog 在输出重定向时会被缓冲，进程被清理就什么都不剩。
          NSString* out_path =
              NSProcessInfo.processInfo.environment[@"DEVCARD_SELFTEST_UPDATE_OUT"];
          if (out_path.length == 0) {
            out_path = @"/tmp/m6_update_live.json";
          }
          [text writeToFile:out_path atomically:YES
                   encoding:NSUTF8StringEncoding
                      error:nil];
          // 有提示就把弹窗打开，供截屏；没有提示也把状态打完，别伪装成成功。
          [web evaluateJavaScript:
                   @"(function(){var p=document.getElementById('update-pill');"
                    "if(!p||p.hidden)return 'no-pill';p.click();return 'clicked';})()"
                completionHandler:^(id clicked, NSError* click_error) {
                  NSString* ui =
                      [clicked isKindOfClass:NSString.class] ? clicked : @"?";
                  NSLog(@"DEVCARD_SELFTEST update_live_ui=%@", ui);
                  if (![ui isEqualToString:@"clicked"]) {
                    NSLog(@"DEVCARD_SELFTEST live_ready");
                    exit(3);
                    return;
                  }
                  // 弹窗打开后等一下再拍：快照拿到的是 WebView 自己渲染的位图，
                  // 不经过窗口层级，Trae 挡在前面也不影响。
                  dispatch_after(
                      dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.8 * NSEC_PER_SEC)),
                      dispatch_get_main_queue(), ^{
                        WKSnapshotConfiguration* shot_config =
                            [[WKSnapshotConfiguration alloc] init];
                        [web takeSnapshotWithConfiguration:shot_config
                                         completionHandler:^(NSImage* image,
                                                             NSError* snap_error) {
                                           NSString* png_path =
                                               NSProcessInfo.processInfo.environment
                                                   [@"DEVCARD_SELFTEST_UPDATE_SHOT"];
                                           if (png_path.length == 0) {
                                             png_path = @"/tmp/m6_update_live_ui.png";
                                           }
                                           if (image == nil) {
                                             NSLog(@"DEVCARD_SELFTEST live_shot_fail=%@",
                                                   snap_error.localizedDescription
                                                       ?: @"(无图)");
                                             exit(4);
                                             return;
                                           }
                                           NSBitmapImageRep* rep =
                                               [NSBitmapImageRep imageRepWithData:
                                                                     [image TIFFRepresentation]];
                                           NSData* png = [rep representationUsingType:
                                                                  NSBitmapImageFileTypePNG
                                                                          properties:@{}];
                                           [png writeToFile:png_path atomically:YES];
                                           NSLog(@"DEVCARD_SELFTEST live_shot=%@ (%lu B)",
                                                 png_path, (unsigned long)png.length);
                                           exit(0);
                                         }];
                      });
                }];
        }];
}

static void devcard_poll_update_later(int attempts) {
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)),
                 dispatch_get_main_queue(), ^{
                   devcard_poll_update_step(attempts);
                 });
}

static void devcard_poll_update_step(int attempts) {
  WKWebView* web = g_update_selftest_web;
  if (!web) {
    NSLog(@"DEVCARD_SELFTEST update_fail 没有页面");
    exit(2);
  }
  NSString* probe = g_update_selftest_started
                        ? @"window.__devcardUpdateSelftestResult ? "
                           "JSON.stringify(window.__devcardUpdateSelftestResult) : ''"
                        : @"typeof window.__devcardUpdateSelftestRun";
  [web evaluateJavaScript:probe
        completionHandler:^(id value, NSError* error) {
          NSString* text = [value isKindOfClass:NSString.class] ? value : @"";
          if (!g_update_selftest_started) {
            if ([text isEqualToString:@"function"]) {
              g_update_selftest_started = YES;
              [web evaluateJavaScript:@"window.__devcardUpdateSelftestRun()"
                    completionHandler:nil];
            }
          } else if (text.length > 0) {
            NSLog(@"DEVCARD_SELFTEST update=%@", text);
            BOOL all_ok = [text rangeOfString:@"\"ok\":false"].location == NSNotFound;
            NSLog(@"DEVCARD_SELFTEST %@", all_ok ? @"OK" : @"FAIL");
            exit(all_ok ? 0 : 1);
          }
          if (attempts <= 0) {
            NSLog(@"DEVCARD_SELFTEST update_fail 自检超时（started=%@）",
                  g_update_selftest_started ? @"YES" : @"NO");
            exit(2);
          }
          devcard_poll_update_later(attempts - 1);
        }];
}

void DevCardUpdateSelftestStart(WKWebView* web, NSString* mode) {
  g_update_selftest_web = web;
  g_update_selftest_started = NO;
  if ([mode isEqualToString:@"live"]) {
    devcard_poll_update_live(80);
  } else if ([mode isEqualToString:@"replay"]) {
    devcard_poll_replay(60);
  } else {
    devcard_poll_update_step(60);
  }
}
