# FD2 Reconstruction

English · [中文](#fd2-重建)

This repository contains a C reimplementation of the reverse-engineered FD2
game runtime. The gameplay and resource code is shared by two hosts:

- a native Windows/SDL2 build under `fd2re/`;
- a static WebAssembly build under `web/`, deployable to GitHub Pages.

## Play in the browser

**https://freeng.github.io/fd2re/**

The playable WebAssembly build is published to GitHub Pages by GitHub Actions
(`.github/workflows/pages.yml`) on every push that touches the game sources,
assets or web code. Click the overlay or press Enter to start; saves and
temporary files persist in the browser through IndexedDB, and the first load
downloads the ~28 MB game data bundle.

The project is still an active reverse-engineering effort. The repository
carries the reconstruction only: the game resource files under `fd2re/` are
kept as build inputs, while the reverse-engineering workspace (the IDA
database, analysis scripts, evidence notes) and the original game install
files intentionally stay out of the repository.

## Repository map

- `fd2re/` - shared C implementation, native host, build assets and resource tools.
- `web/` - Emscripten build, browser shell and local static-server helpers.
- `docs/` - repository-level architecture and contribution boundaries.
- `.github/workflows/pages.yml` - builds and deploys the Web target to GitHub Pages.

See [docs/repository-layout.md](docs/repository-layout.md) for the ownership
and generated-file boundaries.

## Native Windows build

Configure the paths used by the native build in `fd2re/.env`, then run:

```powershell
powershell -ExecutionPolicy Bypass -File .\fd2re\build.ps1
```

The script produces native build output below `fd2re/build/`; that directory is
not part of source control.

## Static Web build

Copy `web/.env.example` to `web/.env`, set `EMCC`, and run:

```powershell
Copy-Item .\web\.env.example .\web\.env
powershell -ExecutionPolicy Bypass -File .\web\build-web.ps1
```

The default build uses Asyncify, bundles SDL2 and libADLMIDI into WebAssembly,
and needs only an ordinary static file host. Browser save and temporary-file
state is mirrored to IndexedDB; no game server or runtime API is required.

GitHub Actions builds the same target and publishes the generated artifact via
GitHub Pages on pushes to `master` or `main`.

## License

No project license has been declared yet. Choose and add one before publishing
if the intended distribution terms are known; the original game assets may
have separate rights.

---

[English](#fd2-reconstruction) · 中文

# FD2 重建

本仓库包含对逆向还原的 FD2 游戏运行时的 C 语言重构实现。玩法与资源代码由
两个宿主共享:

- `fd2re/` 下的原生 Windows/SDL2 构建;
- `web/` 下的静态 WebAssembly 构建,可发布到 GitHub Pages。

## 浏览器游玩

**https://freeng.github.io/fd2re/**

可玩的 WebAssembly 版本由 GitHub Actions(`.github/workflows/pages.yml`)
在每次推送触及游戏源码、资源或 web 代码时自动构建,并发布到 GitHub
Pages。点击遮罩或按回车开始;存档与临时文件通过 IndexedDB 持久化在浏览
器中,首次加载需下载约 28 MB 的游戏数据包。

项目仍处于活跃的逆向工程阶段。本仓库只承载重构产物:`fd2re/` 下的游戏
资源文件作为构建输入保留,而逆向工作区(IDA 数据库、分析脚本、证据笔
记)与原版游戏安装文件刻意不进入仓库。

## 仓库结构

- `fd2re/` - 共享 C 实现、原生宿主、构建资源与资源工具链。
- `web/` - Emscripten 构建、浏览器外壳与本地静态服务器辅助脚本。
- `docs/` - 仓库级架构与贡献边界说明。
- `.github/workflows/pages.yml` - 构建并把 Web 目标发布到 GitHub Pages。

所有权与生成文件边界见 [docs/repository-layout.md](docs/repository-layout.md)。

## 原生 Windows 构建

在 `fd2re/.env` 中配置原生构建所需路径,然后运行:

```powershell
powershell -ExecutionPolicy Bypass -File .\fd2re\build.ps1
```

脚本把原生构建输出放在 `fd2re/build/` 之下;该目录不纳入版本控制。

## 静态 Web 构建

把 `web/.env.example` 复制为 `web/.env`,设置 `EMCC`,然后运行:

```powershell
Copy-Item .\web\.env.example .\web\.env
powershell -ExecutionPolicy Bypass -File .\web\build-web.ps1
```

默认构建使用 Asyncify,把 SDL2 与 libADLMIDI 一并打包进 WebAssembly,只需
普通静态文件托管即可运行。浏览器侧的存档与临时文件镜像到 IndexedDB,
不需要游戏服务器或运行时 API。

GitHub Actions 构建同一目标,并在推送到 `master` 或 `main` 时把生成物
发布到 GitHub Pages。

## 许可证

项目尚未声明许可证。若分发条款已定,发布前请选择并添加;原版游戏资源
可能有独立权利。
