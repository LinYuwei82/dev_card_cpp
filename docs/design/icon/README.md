# 应用图标

## 图形与理由

**深青底 + 白色卡片 + 2×2 窗格 + 铜色顶带。**

- **卡片**直接对应产品名「设备管理卡」，不是抽象字母或齿轮；
- **2×2 窗格**同时读出「楼栋窗户」和「设备格位」，与物业设备台账的业务语义一致；
- **铜色顶带**取自界面强调色 `#C07A45`，让图标与四页样张的「青墨 + 暖纸 + 铜」是同一套语言，而不是另做一个风格；
- **深青底**用 `#0E4B52 → #083238` 的轻渐变，与主色 `#12646B` 同族，在白底 Dock、深色任务栏、开始菜单里都能立住；
- 图形只用三种颜色、无文字、无细线，16px 时仍能看出「卡片 + 四个点」，不会糊成色块。

## 文件

| 文件 | 说明 |
|---|---|
| `icon.svg` | 唯一源文件，1024×1024，squircle 由系统裁切 |
| `png/icon-1024.png` | 母版 |
| `png/icon-512.png` … `icon-16.png` | 各尺寸预览（16/32/48/64/128/256/512） |

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

接入 `.app`：在 `Info.plist` 写 `CFBundleIconFile = icon`，把 `icon.icns` 放进 `Contents/Resources/`。

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

接入：qmake/CMake 把 `.ico` 写进 `RC_ICONS` / `version.rc`，Inno Setup 的 `SetupIconFile` 与快捷方式 `IconFilename` 复用同一文件。

## 小尺寸检查清单

- [ ] 16px：卡片轮廓可辨，四个窗格不粘连；
- [ ] 32px：铜色顶带可见，但不抢主体；
- [ ] 48px：Dock / 任务栏无锯齿；
- [ ] 深色任务栏与浅色桌面各看一次；
- [ ] 与四页样张并排，颜色属于同一套（深青 + 暖白 + 铜）。

> 图标与卡片上的公司 Logo（`resources/icons/logo.jpg`）是两件事：前者是产品标识，后者是甲方标识，不合并、不互相替代。
