#pragma once

#import <Cocoa/Cocoa.h>
#import <Sparkle/Sparkle.h>

NS_ASSUME_NONNULL_BEGIN

/// DevCard macOS Sparkle 自更新桥
///
/// 职责（从 main.mm 抽离）：
///   1. 读取 Info.plist 的 SUFeedURL + SUPublicEDKey，初始化 Sparkle 2.x
///   2. 持有 SPUStandardUpdaterController + SPUUpdaterDelegate 代理（日志观测）
///   3. 供壳层菜单和前端 JS bridge 调用 checkForUpdates
///
/// 开发期没有 SUFeedURL / SUPublicEDKey 时 updaterController 为 nil，
/// checkForUpdates 返回 NO，菜单被隐藏——不阻断壳层其他功能。
@interface DevCardUpdaterBridge : NSObject

@property (nonatomic, strong, nullable) SPUStandardUpdaterController* updaterController;
@property (nonatomic, strong, nullable) id<SPUUpdaterDelegate> updaterDelegate;
@property (nonatomic, assign, nullable) NSMenuItem* checkUpdateMenuItem;

/// 从 Info.plist 读取 Sparkle 配置并初始化。
/// 同时绑定 checkUpdateMenuItem.target（Sparkle 启用时接管，未启用时隐藏）。
- (void)start;

/// 供前端 action=checkForUpdates 或菜单调用。
/// @return YES 表示 Sparkle 已触发更新检查；NO 表示未启用（开发期 / 配置缺失）。
- (BOOL)checkForUpdates;

@end

NS_ASSUME_NONNULL_END
