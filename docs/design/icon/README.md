# 应用图标

> **当前做法**：三端图标（macOS `.icns`、Windows `.ico`、web favicon / Apple Touch / PWA / OG）统一由 `tools/package/make_icons.sh` 从母版 `png/icon-1024.png` 生成，不要手工逐尺寸导出。下文的 `sips` / `iconutil` / ImageMagick 命令是早期手工流程，仅作原理参考。
>
> 产物位置：`shell/mac/DevCard.icns`、`shell/win/DevCard.ico`、`web/` 下各 PNG/ICO。macOS 的 `Info.plist` 由 CMake 生成（`CFBundleIconFile=DevCard.icns`），图标随包复制到 `Contents/Resources/`；Windows `.ico` 由 `shell/win/app.rc` 引用，安装包 `SetupIconFile` 复用同一文件。

## 图形与理由

**深青底 + 米白 DC 字标。**

- `DC` 是 DevCard 的首字母缩写，也是界面侧栏品牌位的同一个标记，图标与产品名对得上；
- 字形取自 SF Pro Display Black（`wght 900`）并已转曲，D 的碗与 C 的开口在小尺寸下仍能读出，不依赖本机是否装了 SF；
- 底板 `#0B3A40` 与主色 `#12646B` 同族，米白 `#FCFAF5` 与打印卡片的纸色一致，图标和四页样张是同一套「青墨 + 暖纸」语言；
- 只有两种颜色、无渐变、无细线：16px 时仍是「D + C」两个实心形，不会糊成一块；
- 底板 824×824、相对画布内缩 100、圆角 185，四角透明，由系统裁切——macOS Dock、Windows 任务栏都不会露白角。

几何按现役 `shell/mac/DevCard.icns` 实测对齐（1024 画布上：D 占 x 179–478，C 占 x 541–844，字高 316–707），字形 IoU 0.86，肉眼看与打包版本一致。

## 文件

| 文件 | 说明 |
|---|---|
| `icon.svg` | 唯一源文件，1024×1024；DC 已转曲为 `<path>`，底板即 squircle |
| `png/icon-1024.png` | 母版（透明四角，供 `tools/package/make_icons.sh` 与各平台导出） |
| `png/icon-512.png` … `icon-16.png` | 各尺寸预览（16/24/32/48/64/128/256/512） |

## 生成 macOS `.icns`

```bash
cd docs/design/icon
mkdir -p icon.iconset
for s in 16 32 64 128 256 512; do
  sips -z $s $s png/icon-1024.png --out icon.iconset/icon_${s}x${s}.png
done
sips -z 32 32   png/icon-1024.png --out icon.iconset/icon_16x16@2x.png
sips -z 64 64   png/icon-1024.png --out icon.iconset/icon_32x32@2x.png
sips -z 256 256 png/icon-1024.png --out icon.iconset/icon_128x128@2x.png
sips -z 512 512 png/icon-1024.png --out icon.iconset/icon_256x256@2x.png
sips -z 1024 1024 png/icon-1024.png --out icon.iconset/icon_512x512@2x.png
iconutil -c icns icon.iconset -o icon.icns
```

接入 `.app`：CMake 生成的 `Info.plist` 已写 `CFBundleIconFile=DevCard.icns`，并把 `DevCard.icns` 放进 `Contents/Resources/`；手工流程仅作参考。

## 生成 Windows `.ico`

Windows 需要单文件多尺寸（16/24/32/48/64/128/256）。任选一种：

```bash
# ImageMagick
magick png/icon-16.png png/icon-24.png png/icon-32.png png/icon-48.png \
       png/icon-64.png png/icon-128.png png/icon-256.png icon.ico
```

或用 ImageMagick 直接从 SVG 生成：

```bash
magick -background none icon.svg -define icon:auto-resize=256,128,64,48,32,24,16 icon.ico
```

接入：CMake 把 `.ico` 写进 Windows 版本资源（`cmake/version.rc.in` 生成的 `version.rc`），Inno Setup 的 `SetupIconFile` 与快捷方式 `IconFilename` 复用同一文件。

## 小尺寸检查清单

- [ ] 16px：D 与 C 之间的留白还在，两个字母不粘；
- [ ] 32px：字标笔画不糊，底板圆角无锯齿；
- [ ] 48px：Dock / 任务栏边缘干净；
- [ ] 深色任务栏与浅色桌面各看一次；
- [ ] 与界面侧栏的 `DC` 标记并排，颜色属于同一套（深青 + 米白）。

> 图标与卡片上的公司 Logo 是两件事：前者是产品标识，后者是甲方标识——由管理员在运行期通过「打印设置」上传，存于数据目录 `assets/`（不进程序包、不进仓库）；两者不合并、不互相替代。
