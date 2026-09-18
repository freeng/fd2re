# fd2re Web

The Web build compiles the same C sources used by the Windows executable to
WebAssembly. Rendering remains the original 320x200 indexed VGA surface; SDL2
provides the browser canvas, keyboard and audio adapters. The published
GitHub Pages build lives at <https://freeng.github.io/fd2re/> and is rebuilt
by CI from `web/CMakeLists.txt`.

## Build

The script reads `web/.env` for `EMCC`. Copy `.env.example` to `.env` when the
SDK is installed elsewhere. It does not read process environment variables.

```powershell
Copy-Item .env.example .env
powershell -ExecutionPolicy Bypass -File build-web.ps1
# Any ordinary static file server is sufficient for this default build.
python -m http.server 8199 --directory dist
```

The default Asyncify build works on ordinary static hosting and includes both
sound effects and music. The music sequencer is advanced by the browser host
event pump, so no special response headers or server are required.

`FD2.SAV` and `FD2.TMP` are initially loaded from the preload image and then
mirrored to IndexedDB under `/persistent`, so saves and temporary sprite data
survive a page refresh without a server API. The generated `dist/` and
intermediate `build/` directories are ignored. Game assets are read from the
sibling `fd2re` directory and the root `FD2.SAV` file.

---

# fd2re Web(中文)

Web 构建把 Windows 可执行程序所用的同一套 C 源码编译为 WebAssembly。渲染
仍是原始的 320x200 索引 VGA 表面;SDL2 提供浏览器画布、键盘与音频适配。
已发布的 GitHub Pages 版本位于 <https://freeng.github.io/fd2re/>,由 CI 依据
`web/CMakeLists.txt` 重建。

## 构建

脚本从 `web/.env` 读取 `EMCC`。若 SDK 安装在其他位置,请把 `.env.example`
复制为 `.env`。脚本不读取进程环境变量。

```powershell
Copy-Item .env.example .env
powershell -ExecutionPolicy Bypass -File build-web.ps1
# 默认构建用任意普通静态文件服务器即可运行。
python -m http.server 8199 --directory dist
```

默认的 Asyncify 构建可在普通静态托管上运行,同时包含音效与音乐。音序器由
浏览器宿主事件泵推进,无需特殊响应头或专用服务器。

`FD2.SAV` 与 `FD2.TMP` 先从预载镜像读入,随后镜像到 `/persistent` 下的
IndexedDB,因此存档与临时精灵数据可在页面刷新后保留,且不需要服务器 API。
生成的 `dist/` 与中间目录 `build/` 均被忽略。游戏资源读取自同级 `fd2re`
目录与仓库根的 `FD2.SAV` 文件。
