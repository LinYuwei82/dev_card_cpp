#pragma once

#import <Foundation/Foundation.h>

@class WKWebView;

/// 自动更新自检（诊断模式）：驱动页面里的更新检查链路，把结果写盘/打日志后退出。
/// 仅在环境变量 DEVCARD_SELFTEST_UPDATE 非空时由 main() 调用：
///   live    —— 打真实发布接口，成功后保留窗口供截屏；
///   replay  —— 把 DEVCARD_UPDATE_FIXTURE 的响应喂进页面解析链；
///   其他值  —— 跑离线失败路径自检（超时 / DNS / 非 JSON / 版本号非法）。
/// web 必须已创建，函数内部会轮询页面是否就绪。
void DevCardUpdateSelftestStart(WKWebView* web, NSString* mode);
