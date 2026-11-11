# SimplePic

Windows 桌面图片批量处理工具。单文件 exe、纯 Win32 自绘 UI（无第三方 UI 框架），开箱即用、无需安装。

![平台](https://img.shields.io/badge/platform-Windows%2064bit-blue) ![语言](https://img.shields.io/badge/C%2B%2B-17-blue) ![构建](https://img.shields.io/badge/build-MSVC%20cl.exe-orange)

## 功能

| 分组 | 模块 | 说明 |
|------|------|------|
| 证件照相关 | 证件照转换 | 标准规格缩放居中裁剪，JPG q95 / 300DPI 输出 |
| 证件照相关 | 修改 DPI | 直改元数据，宽高像素值不变 |
| 体积压缩 | 压缩到指定大小 | 二分搜索质量参数，目标体积精确达标 |
| 体积压缩 | 智能压缩 | 滑块实时预览体积，确认后批量执行 |
| 体积压缩 | 批量增加体积 | 元数据填充，像素零改动 |
| 编辑转换 | 居中裁剪 | 中心截取，8 种预设比例 + 自定义 |
| 编辑转换 | 格式转换 | JPG / PNG / WEBP / BMP 等多格式输出，EXIF 方向自动纠正 |
| 编辑转换 | 旋转 / 翻转镜像 | 保持原格式，仅变角度或镜像 |
| 编辑转换 | 文字水印 / 图片水印 | 字体、颜色、角度、平铺、透明度、布局均可调 |
| 其他 | 长图拼接 | 纵向 / 横向 / 九宫格拼接多张图片 |
| 其他 | 图标导出 | 多尺寸 ICO：16 / 32 / 48 / 64 / 128 / 256 |
| 系统 | 清除元数据 | 剥离 EXIF / GPS / XMP / ICC / COM，像素零改动 |
| 系统 | 右键菜单设置 | 资源管理器右键菜单自由组合 |

**输入格式**：jpg / jpeg / png / gif / bmp / webp / tif / tiff / pdf / heic / heif / hif / avif

## 界面

- 左侧功能列表 + 中间文件列表（列表 / 缩略图双视图、列宽可拖拽）+ 右侧参数面板
- 选中文件实时预览处理效果；缩略图异步解码，大图不卡界面
- 高 DPI 适配（Per-Monitor V2），单实例运行；支持拖拽 / 右键菜单添加文件

## 构建

依赖 MSVC（Visual Studio 2022 BuildTools）与 Windows SDK，第三方库（stb、libwebp）已内置于 `third_party/`，无需额外安装。

```bash
# 推荐：一键构建（自动调用 cl.exe 编译 + rc 资源 + mt manifest）
python build_py.py

# 或使用批处理 / CMake
build.bat
cmake -B build && cmake --build build
```

产物为根目录 `SimplePic.exe`（静态链接 /MT，单文件绿色运行）。

## 目录结构

```
├── src/
│   ├── main.cpp            # 入口
│   ├── codec/              # 解码编码（stb_image / libwebp / JPEG 元数据 / PDF）
│   ├── ops/                # 图像操作（压缩、裁剪、水印、DPI、元数据剥离…）
│   ├── pipeline/           # 批处理流水线
│   └── ui/                 # Win32 自绘界面
├── third_party/            # stb / libwebp（源码内置）
├── tools/                  # 图标生成等辅助脚本
├── tests/                  # 回归与冒烟测试（python + c++ probe）
├── build_py.py             # 构建脚本
└── CMakeLists.txt
```

## 测试

```bash
python tests/test_viewseg_click.py    # 视图切换点击回归
python tests/test_param_scroll.py     # 参数区滚动
python tests/test_shell_menu.py       # 右键菜单
python tests/test_single_instance.py  # 单实例
python test_pipeline.py               # 处理流水线
```
