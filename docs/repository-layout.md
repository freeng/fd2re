# Repository Layout

This repository has one gameplay implementation and two host targets. The
host boundary is intentional: browser code must not fork the reverse-engineered
gameplay rules, resource decoders, save layout, or battle state machine.

```text
fd2re/
  include/              shared C contracts and subsystem headers
  src/                  shared gameplay, resource, save, audio and video code
  tools/                reproducible resource/save tools and optional viewers
  vendor/               pinned SDL2 and libADLMIDI source/development files
  build.ps1             Windows/MSVC convenience build
  *.DAT, *.B24, *.TMP   original game assets used by both targets

web/
  CMakeLists.txt        canonical Emscripten build used by CI/Pages
  build-web.ps1         local Windows convenience wrapper
  shell.html             browser page and persistent-file bootstrap
  jslib.js               Asyncify sleep bridge for static hosting
  .env.example           local tool-path configuration template

.github/workflows/
  pages.yml             build web/build-pages/dist and deploy GitHub Pages

README.md               repository entry point and build overview
```

Generated `build/`, `web/build-pages/`, `web/dist/`, local `.env` files and
analysis databases stay out of version control. The reverse-engineering
workspace — the IDA database, analysis scripts and evidence notes, and the
verbatim original game install files — is intentionally kept out of the
repository as well. GitHub Pages publishes only
the generated `web/build-pages/dist` artifact; it does not serve the source
tree or require a game server/API.

The default Pages build is single-threaded Asyncify. SDL2 and libADLMIDI are
compiled into the WebAssembly bundle, while game data is preloaded into the
generated `.data` file. `FD2.SAV` and `FD2.TMP` are mirrored to IndexedDB by
the page shell, so browser storage is local to the user and no backend is
needed.

---

# 仓库结构

本仓库只有一份玩法实现和两个宿主目标。宿主边界是有意设计的:浏览器代码
不得分叉逆向得到的玩法规则、资源解码器、存档布局或战斗状态机。

```text
fd2re/
  include/              共享 C 契约与子系统头文件
  src/                  共享的玩法、资源、存档、音频与视频代码
  tools/                可复现的资源/存档工具链与可选查看器
  vendor/               固定版本的 SDL2 与 libADLMIDI 源码/开发文件
  build.ps1             Windows/MSVC 便捷构建
  *.DAT, *.B24, *.TMP   两端共用的游戏资源

web/
  CMakeLists.txt        CI/Pages 使用的 canonical Emscripten 构建
  build-web.ps1         本地 Windows 便捷包装
  shell.html            浏览器页面与持久化文件引导
  jslib.js              静态托管下的 Asyncify 睡眠桥接
  .env.example          本地工具路径配置模板

.github/workflows/
  pages.yml             构建 web/build-pages/dist 并部署 GitHub Pages

README.md               仓库入口与构建总览
```

生成的 `build/`、`web/build-pages/`、`web/dist/`、本地 `.env` 文件与分析
数据库不入版本控制。逆向工作区——IDA 数据库、分析脚本与证据笔记,以及
逐字节的原版游戏安装文件——同样刻意不入仓库。GitHub Pages 只发布生成的
`web/build-pages/dist` 产物;它不提供源码树,也不需要游戏服务器/API。

默认的 Pages 构建是单线程 Asyncify。SDL2 与 libADLMIDI 编译进 WebAssembly
包,游戏数据预载进生成的 `.data` 文件。`FD2.SAV` 与 `FD2.TMP` 由页面外壳
镜像到 IndexedDB,因此浏览器存储对用户是本地的,无需任何后端。
