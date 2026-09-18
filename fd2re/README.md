# fd2re — FD2.EXE 的 C 重构

中文 · [English](#fd2re--c-reconstruction-of-fd2exe)

对 DOS 游戏 `FD2.EXE`(WATCOM 32 位保护模式,DOS/4GW 风格 extender)运行时
的 C 语言重构。玩法规则、资源解码、存档布局与战斗状态机逐项对应原版
行为;宿主层负责呈现与系统适配。

## 目录

```text
fd2re/
  include/               公共头
  src/                   各子系统实现(battle/duel/scene/states/event/text/…)
  tools/                 可复现的资源/存档工具链与资源查看器
  vendor/                固定版本的 SDL2 与 libADLMIDI
  build.bat / build.ps1  MSVC 构建(SDL2 宿主后端)
  Makefile               build.ps1 的 make 包装
  *.DAT, *.B24, *.TMP    两端共用的游戏资源(构建输入)
  FD2SAMPLE.wopl         OPL3 音色库(由游戏自带音色转换而来)
```

## Windows / Web 双端

Web 端复用同一套 C 源码,构建入口在仓库根目录的 `web/build-web.ps1`。
默认 Asyncify 构建适合普通静态托管,音频、存档和临时文件都在浏览器侧
完成;`web/CMakeLists.txt` 是 GitHub Pages 使用的 canonical 构建入口。
Windows 端使用 `build.ps1` 和 SDL2 原生宿主,不与 Web 端共享生成物。
仓库边界详见 `docs/repository-layout.md`。

```powershell
powershell -ExecutionPolicy Bypass -File web/build-web.ps1
python -m http.server 8199 --directory web/dist
```

## Windows 构建

SDL2 宿主后端使用与仓库内 SDL2 静态库同 ABI 的 MSVC。路径和系统库在
`.env` 固定,不读取系统环境变量。

```powershell
.\build.bat          # 或 powershell -ExecutionPolicy Bypass -File build.ps1
```

> 注:`make` 可作为 `build.ps1` 的包装;直接运行脚本即可。

## 当前状态

**核心子系统全量落地,`FD2_TODO()` 调用点清零**。已覆盖:标题/主菜单/
序章链、状态机 27 态、ADV 对白引擎(逐字上屏/滚行/肖像/对白窗)、城镇
场景(酒馆/商店/教会/神秘商店/宝箱/村庄交互)、战斗子系统(移动域/
行动菜单/攻击决斗/AI 决策链 1:1/36 种法术/战况板/回合循环)、决斗与
特效演出(fx 控制器 0..9/粒子/多段/大招)、存读档(FD2.SAV 布局/槽位
菜单)、音频(winmm MIDI + DIG 音效;Web 端为 OPL3 合成)、90 项事件
处理器、结局链。原生宿主为 Windows(SDL2 呈现 + winmm MIDI)。

已知开放项:决斗演出逐帧像素比对、options_menu_run 逐帧布局、对话五
缓冲逐字节等价。

---

# fd2re — C Reconstruction of FD2.EXE

[中文](#fd2re--fd2exe-的-c-重构) · English

A C reimplementation of the DOS game `FD2.EXE` runtime (WATCOM 32-bit
protected mode, DOS/4GW-style extender). Gameplay rules, resource decoders,
the save layout and the battle state machine match the original behavior
item by item; the host layer owns presentation and system adaptation.

## Layout

```text
fd2re/
  include/               shared public headers
  src/                   per-subsystem implementations (battle/duel/scene/states/event/text/…)
  tools/                 reproducible resource/save toolchain and resource viewers
  vendor/                pinned SDL2 and libADLMIDI
  build.bat / build.ps1  MSVC build (SDL2 host backend)
  Makefile               make wrapper around build.ps1
  *.DAT, *.B24, *.TMP    game assets shared by both hosts (build inputs)
  FD2SAMPLE.wopl         OPL3 bank (converted from the game's own bank)
```

## Windows / Web dual targets

The Web host reuses the same C sources; its build entry is `web/build-web.ps1`
at the repository root. The default Asyncify build runs on ordinary static
hosting, with audio, saves and temporary files handled entirely in the
browser; `web/CMakeLists.txt` is the canonical build used by GitHub Pages.
The Windows side uses `build.ps1` with the native SDL2 host and does not
share build outputs with the Web host. Repository boundaries are described in
`docs/repository-layout.md`.

```powershell
powershell -ExecutionPolicy Bypass -File web/build-web.ps1
python -m http.server 8199 --directory web/dist
```

## Windows build

The SDL2 host backend requires an MSVC toolset with the same ABI as the
vendored SDL2 static library. Paths and system libraries are pinned in
`.env`; process environment variables are not read.

```powershell
.\build.bat          # or powershell -ExecutionPolicy Bypass -File build.ps1
```

> `make` wraps `build.ps1`; run the script directly.

## Status

**All core subsystems are in place with zero `FD2_TODO()` call sites.**
Covered: title / main menu / prologue chain, the 27-state state machine, the
ADV dialogue engine (typewriter text / line scroll / portraits / dialogue
window), town scenes (tavern / shops / church / secret shop / chests /
village interactions), battle (movement domain / action menu / attack duels /
1:1 AI decision chain / 36 spells / battle board / turn loop), duel and
effect staging (fx controllers 0..9 / particles / multi-hit / supers),
save-load (FD2.SAV layout / slot menu), audio (winmm MIDI + DIG sound
effects; OPL3 synthesis on the Web host), 90 event handlers and the ending
chain. The native host is Windows (SDL2 presentation + winmm MIDI).

Known open items: duel staging frame-by-frame pixel comparison,
options_menu_run frame-by-frame layout, and byte-exact equivalence of the
five dialogue buffers.
