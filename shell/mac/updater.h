#pragma once

#import <Cocoa/Cocoa.h>

NS_ASSUME_NONNULL_BEGIN

/// DevCard macOS 更新适配器（mote 模式）
///
/// 分层：判定与编排全在 core 的 devcard::UpdateEngine（与 Windows 共用同一份），
/// 本桥只实现四个平台适配器——NSURLSession 拉清单、NSUserDefaults 持久化、
/// NSAlert 原生弹窗、Sparkle 安装执行器（用户确认后才拉起 Sparkle 下载与替换）。
///
/// 开发期 Info.plist 缺 SUFeedURL / SUPublicEDKey 时清单地址为空，
/// 引擎检查直接提示「未配置」，菜单隐藏——不阻断壳层其他功能。
@interface DevCardUpdaterBridge : NSObject

/// 菜单「检查更新…」项（start 时绑定 target；未配置更新时隐藏）。
@property (nonatomic, assign, nullable) NSMenuItem* checkUpdateMenuItem;

/// 构建引擎、调度后台检查（启动延迟后首查，之后每 24 小时）。
- (void)start;

/// 菜单 / 前端桥触发：引擎立即检查，无视节流与「稍后」记录。
/// @return YES 表示已发起；NO 表示未配置更新地址（开发期）。
- (BOOL)checkForUpdates;

@end

NS_ASSUME_NONNULL_END
