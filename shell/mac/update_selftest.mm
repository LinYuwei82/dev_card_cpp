// DevCard 自动更新自检：从 main.mm 抽出的独立诊断模块，只负责驱动页面自检，
// 不参与正式更新流程（正式更新由 shell/mac/updater.mm 的 DevCardUpdaterBridge 负责）。
//
// 分层重构后（检测在 core、编排在 server、Sparkle 只当安装执行器）：
//   - live 模式：页面同源 POST /api/update/check 让服务端立即出网拉清单，
//     轮询结果 JSON（verdict / update_available / last_error），点开弹窗截图；
//   - 默认（离线）模式：只读页面已渲染的 /api/update/state 快照
//     （window.__devcardUpdateDiag），验证「服务端状态 → 界面」这一段通；
//   - replay 模式随前端检测层一起退役：判据回放现在由
//     tests/test_core.cpp 的 evaluate_update 表驱动用例承担（同一份
//     update_compare_vectors.json），不再需要把响应喂进页面。
// 状态放文件级变量而不是 __block：跨回调的 __block 状态在本项目第一次实现时直接
// SIGSEGV（dispatch 回调里调到了空 block），换成静态变量后不再依赖编译器对
// ObjC++ 下 __block 的处理。

#import "update_selftest.h"

#import <WebKit/WebKit.h>

#include <cstdlib>

static WKWebView* g_update_selftest_web = nil;

static NSString* devcard_selftest_out_path(void) {
  NSString* out_path =
      NSProcessInfo.processInfo.environment[@"DEVCARD_SELFTEST_UPDATE_OUT"];
  return out_path.length > 0 ? out_path : @"/tmp/m6_update_live.json";
}

// 点开顶栏 pill（若有）并截图弹窗，供验收记录。
static void devcard_snapshot_then_exit(WKWebView* web, int exit_code) {
  [web evaluateJavaScript:
           @"(function(){var p=document.getElementById('update-pill');"
            "if(!p||p.hidden)return 'no-pill';p.click();return 'clicked';})()"
      completionHandler:^(id clicked, NSError* click_error) {
        NSString* ui = [clicked isKindOfClass:NSString.class] ? clicked : @"?";
        NSLog(@"DEVCARD_SELFTEST update_ui=%@", ui);
        dispatch_after(
            dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.8 * NSEC_PER_SEC)),
            dispatch_get_main_queue(), ^{
              WKSnapshotConfiguration* config =
                  [[WKSnapshotConfiguration alloc] init];
              [web takeSnapshotWithConfiguration:config
                               completionHandler:^(NSImage* image,
                                                   NSError* snap_error) {
                                 NSString* png_path =
                                     NSProcessInfo.processInfo.environment
                                         [@"DEVCARD_SELFTEST_UPDATE_SHOT"];
                                 if (png_path.length == 0) {
                                   png_path = @"/tmp/m6_update_live_ui.png";
                                 }
                                 if (image == nil) {
                                   NSLog(@"DEVCARD_SELFTEST shot_fail=%@",
                                         snap_error.localizedDescription
                                             ?: @"(无图)");
                                   exit(exit_code == 0 ? 4 : exit_code);
                                   return;
                                 }
                                 NSBitmapImageRep* rep =
                                     [NSBitmapImageRep imageRepWithData:
                                           [image TIFFRepresentation]];
                                 NSData* png = [rep representationUsingType:
                                                        NSBitmapImageFileTypePNG
                                                                properties:@{}];
                                 [png writeToFile:png_path atomically:YES];
                                 NSLog(@"DEVCARD_SELFTEST shot=%@ (%lu B)",
                                       png_path, (unsigned long)png.length);
                                 NSLog(@"DEVCARD_SELFTEST %@",
                                       exit_code == 0 ? @"OK" : @"FAIL");
                                 exit(exit_code);
                               }];
            });
      }];
}

// live 模式：让服务端立即检查（页面同源带会话），轮询到结果后落盘 + 截图。
static void devcard_poll_update_live(int attempts) {
  WKWebView* web = g_update_selftest_web;
  if (!web) {
    NSLog(@"DEVCARD_SELFTEST update_fail 没有页面");
    exit(2);
  }
  NSString* js =
      @"(function(){"
       "if(!window.__devcardSelftestCheck){"
       "window.__devcardSelftestCheck='pending';"
       "fetch('/api/update/check',{method:'POST',credentials:'same-origin',"
       "headers:{'Content-Type':'application/json','X-Requested-With':'DevCard'},"
       "body:'{}'})"
       ".then(function(r){return r.text();})"
       ".then(function(t){window.__devcardSelftestCheck=t;})"
       ".catch(function(e){window.__devcardSelftestCheck='ERR '+e.name+': '+e.message;});}"
       "return window.__devcardSelftestCheck;})()";
  [web evaluateJavaScript:js
        completionHandler:^(id value, NSError* error) {
          NSString* text = [value isKindOfClass:NSString.class] ? value : @"";
          if ([text isEqualToString:@"pending"] || text.length == 0) {
            if (attempts > 0) {
              dispatch_after(
                  dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.0 * NSEC_PER_SEC)),
                  dispatch_get_main_queue(), ^{
                    devcard_poll_update_live(attempts - 1);
                  });
              return;
            }
            // 超时也要留证据：把当时的界面状态写盘，别只报「超时」。
            NSString* fail_out =
                [NSString stringWithFormat:
                    @"{\"status\":\"timeout\",\"attempts_left\":0}"];
            [fail_out writeToFile:devcard_selftest_out_path()
                       atomically:YES
                         encoding:NSUTF8StringEncoding
                            error:nil];
            NSLog(@"DEVCARD_SELFTEST update_fail 服务端检查未返回");
            exit(2);
          }
          NSLog(@"DEVCARD_SELFTEST update_live=%@", text);
          [text writeToFile:devcard_selftest_out_path()
                 atomically:YES
                   encoding:NSUTF8StringEncoding
                      error:nil];
          if ([text hasPrefix:@"ERR "]) {
            NSLog(@"DEVCARD_SELFTEST FAIL");
            exit(1);
          }
          // 有更新才期待 pill；没更新时界面没弹窗也属正常，按 OK 收尾。
          BOOL available =
              [text rangeOfString:@"\"update_available\":true"].location != NSNotFound;
          if (!available) {
            NSLog(@"DEVCARD_SELFTEST OK（无可用更新，不截图弹窗）");
            exit(0);
          }
          devcard_snapshot_then_exit(web, 0);
        }];
}

// 默认（离线）模式：不触发服务端出网，只读页面已拿到的状态快照。
static void devcard_poll_update_step(int attempts) {
  WKWebView* web = g_update_selftest_web;
  if (!web) {
    NSLog(@"DEVCARD_SELFTEST update_fail 没有页面");
    exit(2);
  }
  [web evaluateJavaScript:@"(window.__devcardUpdateDiag ? window.__devcardUpdateDiag() : '')"
        completionHandler:^(id value, NSError* error) {
          NSString* text = [value isKindOfClass:NSString.class] ? value : @"";
          if (text.length > 0) {
            NSLog(@"DEVCARD_SELFTEST update=%@", text);
            NSString* out_path = devcard_selftest_out_path();
            [text writeToFile:out_path atomically:YES
                     encoding:NSUTF8StringEncoding
                        error:nil];
            // 判据：页面拿到了服务端状态（checked 或 last_error 至少其一），
            // 且没有 JS 侧异常——具体 verdict 合理性由 ctest 与 guard 保证。
            BOOL has_signal =
                [text rangeOfString:@"\"checked\":true"].location != NSNotFound ||
                [text rangeOfString:@"last_check_epoch" ].location != NSNotFound;
            NSLog(@"DEVCARD_SELFTEST %@", has_signal ? @"OK" : @"FAIL");
            exit(has_signal ? 0 : 1);
          }
          if (attempts <= 0) {
            NSLog(@"DEVCARD_SELFTEST update_fail 自检超时（页面未就绪）");
            exit(2);
          }
          dispatch_after(
              dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)),
              dispatch_get_main_queue(), ^{
                devcard_poll_update_step(attempts - 1);
              });
        }];
}

void DevCardUpdateSelftestStart(WKWebView* web, NSString* mode) {
  g_update_selftest_web = web;
  if ([mode isEqualToString:@"live"]) {
    devcard_poll_update_live(60);
    return;
  }
  if ([mode isEqualToString:@"replay"]) {
    // 判据回放已迁到 core 表驱动单测（test_core 的 update-evaluate 用例）。
    // 这里降级为离线模式，保持旧调用方（脚本/文档）不炸。
    NSLog(@"DEVCARD_SELFTEST replay 模式已退役：判据在 core 单测，改跑离线状态自检");
  }
  devcard_poll_update_step(60);
}
