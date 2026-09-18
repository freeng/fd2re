# fd2re C++ OOP 化重构开发方案

> 目标一句话：把 22.7k 行全局变量 + 魔数偏移的过程式 C 代码，重构为**对象边界清晰、
> 所有权明确、证据注释不丢**的 C++20 代码——更对象化、更易读、更易维护，
> 同时保持与 FD2.EXE 的 1:1 行为等价和 FD2.SAV 字节级兼容。

---

## 0. 文档定位

- 本方案是**重构蓝图**，不改逆向结论。`docs/architecture.md` 仍是逆向证据的唯一权威；
  本方案只规定"代码怎么组织"，不推翻任何已定案的地址/格式/行为结论。
- 重构期间**禁止**混入任何逆向新结论或行为修正：重构提交 = 行为零差异；
  行为修正走独立提交（与现在的方法纪律一致）。
- 已知开放项（决斗逐帧像素比对、options_menu 布局、对话五缓冲逐字节等价，
  见 architecture.md §11/§13.77 尾注）不受本重构影响，重构前后保持同一开放状态。

## 1. 目标与非目标

### 1.1 目标（按优先级）

1. **行为等价**：重构前后游戏行为、FD2.SAV 读写、RNG 序列完全一致（§7）。
2. **对象化**：每个子系统一个类边界，状态归属明确；消灭"谁都能写"的 114 个
   extern 全局（§5）。
3. **易读**：魔数偏移收敛为命名访问器（`rec[5] & 0x85` → `unit.is_actionable()`）；
   匿名 `sub_XXXX` 在语义已知处命名；每个方法保留 `IDA 0x…` 证据注释，
   与 architecture.md 保持可互相 grep。
4. **易维护**：所有权 RAII 化（malloc/free 配对错误从此绝迹）、类型化的数据表、
   单元测试覆盖纯逻辑公式、每个阶段结束游戏可运行可存档。

### 1.2 非目标

- 不改 FD2.SAV / FDFIELD / FDSHAP / g_battle_ctx / 80B 实体记录等**已逆向定案的
  二进制布局**（§3.3 论证为什么不改）。
- 不引入并发、不引入异常流（§9）、不引入重量级框架/ORM/反射式注册。
- 不做"顺手"的逆向补全：重构过程中发现的未逆明字段，保持原样 + TODO 注释。

## 2. 现状盘点：痛点举证

以下全部来自当前代码，是本方案各项设计的直接动因。

| # | 痛点 | 证据 | 已造成的（或险些造成的）事故 |
|---|------|------|------------------------------|
| P1 | **114 个 extern 全局**，fd2.h 独占 90 个；写点散布全库，归属不明 | `include/*.h` grep `^extern` | `g_death_fx_pkg` 忘记与 `g_pkg_fdother_5` 同步别名 → NULL 解引用崩溃（main.c:44 注释）；`g_scene_bg_pkg` 在三处调用点各自 malloc/free，收尾路径极易漏 |
| P2 | **魔数偏移访问实体**：`rec[5] & 0x85`、`rec[+0x40] HP` | battle.c:129, entities.h:8-21 | fd2.h 与 entities.h 对同一 80B 记录的 +3/+5/+6 语义注释出现过分歧（2026-09-08 才更正）——同一事实写两遍必然漂移 |
| P3 | **2211B g_battle_ctx 按偏移宏访问** | battle.c:47-53 CTX_OFF_* | 越界守卫写错常量导致章节载入恒 abort（statemachine.c:88-92 "2026-09-07 事故修复"） |
| P4 | **函数指针表当状态机**：`g_state_enter/exit[30]`、`g_state_post_action[30]`、`g_event_script_table[90]` | fd2.h:78-79, event.h:39 | 表项 [23]/[24] 是 IDA 未拆分段内入口这类坑只能靠注释背；90 个处理器无名字、无参类型约束（全靠 int unit 约定） |
| P5 | **void\* 资源指针 + 配套 \*_size 全局**，malloc/free 生命周期靠人肉纪律 | fd2.h:45-76 | `state_run_scene` 每轮 `free(g_field_map)` 系手工配对；statemachine.c 的 pickup/teardown 流程 60 行里 5 次 free/置 NULL |
| P6 | **匿名函数**：`sub_17AED`（单位状态页）、`sub_13512`、`sub_1F183` 等语义已明却仍用地址名 | entities.h:132-134, event.h:97 | 读代码需先查注释；architecture.md 引用与代码名两套词汇 |
| P7 | **tests/ 为空**，纯逻辑公式（伤害/命中/移动 BFS/商店货源）无任何自动验证 | tests/ 目录 | 公式回归只能靠人肉跑图 |
| P8 | 菜单/对话框缓冲三件套（work/snap/backup）在 scene.c/text.c 两大半边重复出现，收尾函数 `menu_buffers_teardown` 靠"双半边各自守卫"免重复释放 | text.h:72-77 | 已经是打补丁的信号——正确做法是一个对象拥有自己的缓冲 |

## 3. 总体架构设计

### 3.1 分层与模块划分

```
┌────────────────────────────────────────────────────────┐
│ core    Game(组合根) · MainLoop · StateMachine · 各State │
├────────────────────────────────────────────────────────┤
│ domain  EntityTable/UnitRef · BattleField · Battle 派生类 │
│         DuelScene/FxController · SpellCast · EventScripts │
├────────────────────────────────────────────────────────┤
│ ui      TextRenderer · DialogWindow · ConfirmDialog       │
│         ShopMenu · RosterMenu · SaveSlotMenu · UnitPanel  │
├────────────────────────────────────────────────────────┤
│ gfx     Screen(VRAM) · Palette · RleDecoder · Sprite24    │
│         IconCache                                        │
├────────────────────────────────────────────────────────┤
│ data    DatArchive/Package · TypedTables · SaveFile       │
├────────────────────────────────────────────────────────┤
│ audio   MusicPlayer · SfxPlayer                           │
├────────────────────────────────────────────────────────┤
│ host    SdlHost · Keyboard · BiosClock      （平台边界）  │
└────────────────────────────────────────────────────────┘
```

依赖规则（单向向下）：

- `core` 可引用一切；`domain` 不依赖 `ui` 具体类，只依赖 `gfx`/`data`/`audio`
  与 `ui` 的**前向声明接口**（战斗内嵌菜单是例外，见 §4.9 的务实处理）。
- `host` 不被任何层 include，只有 `SdlHost` 持有平台回调并把事件喂给
  `Keyboard`/`BiosClock`。
- 禁止 include 环；用前置声明 + 构造注入解决相互引用。

### 3.2 组合根：`Game` 聚合

`main()` 退化为 20 行的组合根，与 IDA 0x25BF4 的装配顺序一一对应：

```cpp
// src/core/game.cpp — 装配顺序 = IDA 0x25BF4 main 的初始化序
struct Game {
    SdlHost     host;        // SDL 窗口/事件泵/呈现
    BiosClock   clock;       // 0x46C tick（RNG 种子源）
    Keyboard    keyboard;    // 扫描码重映射（input_wait_key 0x11AA8）
    DatArchive  archive;     // DAT 目录解析 + dat_load_block
    Resources   resources;   // 8×启动装载（原 resources_load）
    Screen      screen;      // VRAM 320x200 + 模式切换
    MusicPlayer music;       // XMIDI→winmm
    SfxPlayer   sfx;         // DIG 音效
    TextRenderer text;       // FDTXT 字形渲染
    DialogWindow dialog;     // 对白窗/五缓冲/肖像
    EntityTable entities;   // roster 32 + 战场 96
    BattleField field;      // FDFIELD/FDSHAP 当前地图
    // ……其面子系统按依赖序排列
    StateMachine states;    // 最后装配：注入以上全部
};

int main() {
    Game g;                 // 构造序 = 原版 AIL_init → 8×dat_load → malloc → VGA
    g.run();                // 标题循环 + 内层循环（0x25DB5 结构）
}
```

要点：

- **构造顺序即原版初始化顺序**，注释保留 `IDA 0x25BF4` 分步说明。
- 子系统之间用**构造注入**（长生命周期，main 一次装配），不做运行期服务定位器。
- 装配完后 `Game&` 引用沿构造链下传；不出现新的全局单例（迁移期过渡手段见 §5）。

### 3.3 核心决策：逆向 ABI 数据 = 字节存储 + 视图类（不改成真字段）

这是全方案最重要的一次取舍，先论证再定案。

80B 实体记录、2211B battle_ctx、FD2.SAV 布局、roster 2560B，本质是**原版的
磁盘/内存 ABI**：SAV 直接落盘实体字节、battle_ctx 来自 FDFIELD.DAT 块原样装载、
roster 在存档槽里按 80B 原样搬运。若改成"真字段 struct"，就必须为每个布局写
marshal 层，等价性风险从 0 变成 O(字段数)，而收益（可读性）用视图类就能拿全。

**定案**：存储保持 `std::array<std::uint8_t, 80>` 等原始字节；所有访问经由
命名访问器（§4.1/§4.2）。偏移常量集中一处，注释（含 IDA 证据）只写一遍——
直接消灭痛点 P2 的"同一记录两份注释漂移"。

只有一种情况允许真字段化：某结构**从不落盘、从不跨界装载**（如 AI 评分暂存
`g_ai_*` 十余个全局 → 直接变成 `AIPlan` 真字段对象，§4.9）。

### 3.4 目录布局与旧文件映射

```
fd2re/
  include/fd2/            公共头（按模块分子目录）
  src/
    core/                 game.cpp  mainloop.cpp  statemachine.cpp
    core/states/          state00_title.cpp … state29_final_battle.cpp
    domain/               entity.cpp  battlefield.cpp  camera_cursor.cpp
    battle/               turnloop.cpp  movement.cpp  menus.cpp
                          ai.cpp  results.cpp  tilepanel.cpp  magic.cpp
    duel/                 duel_scene.cpp  strike.cpp  fx.cpp
    scene/                scene_controller.cpp  events.cpp  cutscene.cpp
    ui/                   text.cpp  dialog.cpp  confirm.cpp
                          shop_menu.cpp  roster_menu.cpp  save_slot_menu.cpp
    gfx/                  screen.cpp  palette.cpp  rle.cpp  sprite.cpp  icon.cpp
    data/                 package.cpp  tables.cpp  savefile.cpp
    audio/                music.cpp  sfx.cpp
    host/                 sdl_host.cpp  keyboard.cpp  clock.cpp
  tests/                  纯逻辑单元测试（§7.3）
  build.ps1 / .env        不变，仅编译参数升级（§8）
```

旧文件 → 新位置对照（行数为当前 wc -l）：

| 现文件 | 行数 | 新位置 | 主要类 |
|---|---|---|---|
| main.c | 95 | core/game.cpp | `Game`、`main()` |
| startup.c | 258 | core/ | `TitleFlow`（intro 菜单/新游戏/续档分派） |
| statemachine.c | 457 | core/statemachine.cpp + scene/ | `StateMachine`、`StateRegistry`；ADV 场景路径拆给 `SceneController` |
| states.c | 1465 | core/states/ | 各 `XxxState` 子类（§4.7） |
| entities.c | 1547 | domain/entity.cpp + domain/battlefield.cpp | `EntityTable`、`UnitRef`、`FieldMap`、`CameraCursor` |
| battle.c | 4821 | battle/ 六个文件 | `TurnLoop`、`MovementRange`、`BattleMenus`、`AIPlanner`、`BattleResults`、`TileInfoPanel`（§4.9） |
| magic.c | 895 | battle/magic.cpp | `SpellCast` |
| duel.c | 3370 | duel/ | `DuelScene`、`StrikeFlow`、`FxController` |
| scene.c | 2489 | scene/ + ui/ | `SceneController`、`ShopMenu`、`RosterMenu`、`SaveSlotMenu`、`TempleMenu` |
| event.c | 1272 | scene/events.cpp | `EventScripts`（§4.8） |
| text.c | 1269 | ui/text.cpp + ui/dialog.cpp | `TextRenderer`、`DialogWindow`、`ConfirmDialog` |
| script.c | 289 | scene/cutscene.cpp | `CutscenePlayer`（ui_script_exec 0x1366A） |
| tables.c | 851 | data/tables.cpp | 类型化表视图（§4.10） |
| audio.c | 796 | audio/ | `MusicPlayer`、`SfxPlayer` |
| video.c | 457 | gfx/ | `Screen`、`Palette`、`RleDecoder`、`Sprite24` |
| resource.c | 255 | data/package.cpp | `DatArchive`、`Package` |
| save.c | 166 | data/savefile.cpp | `SaveFile` |
| input.c | 94 | host/keyboard.cpp | `Keyboard` |
| timer.c | 53 | host/clock.cpp | `BiosClock` |
| host.c | 235 | host/sdl_host.cpp | `SdlHost` |

## 4. 关键类设计（before / after）

### 4.1 `EntityTable` / `UnitRef` — 80B 实体记录

```cpp
// include/fd2/domain/unit_ref.hpp
namespace fd2 {

/* 实体记录 80B——原版 ABI，字节布局即逆向结论（entities.h 头注释全文迁入，
 * 作为唯一权威版本；fd2.h 的重复段删除）。 */
inline constexpr std::size_t kEntityRecSize = 80;   // IDA: malloc(2560)=32*80

class EntityTable;

/* UnitRef —— 一条实体记录的类型化引用（视图，不拥有存储）。
 * 命名规范：方法名对应逆向语义，实现处保留 IDA 证据注释。 */
class UnitRef {
public:
    // ---- 位置/朝向 ----
    int  grid_x() const;  void set_grid_x(int v);   // +0
    int  grid_y() const;  void set_grid_y(int v);   // +1
    void face(Direction d);  Direction facing() const;  // +3, ent_walk_*

    // ---- flags（+5，位含义唯一权威在此） ----
    bool flag_rostered() const;      // bit0（ent_flag1_test 0x34894）
    bool flag_removed()  const;      // bit2（阵亡移除）
    bool flag_acted()    const;      // bit7（sub_13512 置位）
    void mark_acted();               // IDA 0x13512: ent[5] |= 0x80
    bool is_actionable() const;      // (flags & 0x85)==0 —— anim_pump 环扫条件

    // ---- 身份 ----
    Allegiance allegiance() const;   // +6: Player/AllyNpc/Enemy
    int  class_id() const;           // +7（=FDICON 图标索引）
    int  name_id() const;            // +8（文本 id+1）
    bool is_special() const;         // IDA 0x1F183（特殊单位判定）

    // ---- 物品 ----
    int  item_count() const;         // 0x1B8A6
    int  item_at(int slot) const;    // 0x1B722
    bool inventory_add(int item);    // 0x1BB8C，1=成功/-1=满 → bool + 返回槽位见 find
    int  find_equipped_slot(bool special) const;  // 0x1B83D

    // ---- 战斗数值 ----
    int hp() const;  void set_hp(int v);        // +0x40 u16
    int max_hp() const;                          // +0x42
    int atk() const; int def() const;           // +0x48/+0x4A
    int hit() const; int evade() const;         // +0x4C/+0x4E
    void recompute_derived();                    // IDA 0x1B750
    void apply_equipment();                      // IDA 0x1145A（roster 侧）

    // ---- AI/成长（低频访问器从略：ai_subtype/class_family/level/growth…） ----
private:
    friend class EntityTable;
    UnitRef(EntityTable& t, int idx);
    std::uint8_t*       p_;   // 指向记录首字节
    // 不持 EntityTable 指针亦可（p_ 已够），保留 idx 便于断言
};

class EntityTable {
public:
    UnitRef at(int idx);            // 越界 → abort（RE 不变量，见 §9 错误策略）
    int     count() const;
    int     unit_at_cursor(...) const;   // IDA 0x12C0D
    /* roster / battle 两块存储：
     * roster 固定 32 条（0x53BF7），battle 最多 96 条（0x53A45）。 */
    /* 序列化：SAV 直拷字节（§7.1）——刻意提供裸区访问，仅 SaveFile 可用。 */
    std::span<std::uint8_t>       raw_roster();
    std::span<const std::uint8_t> raw_roster() const;
};

} // namespace fd2
```

改造前后对比（anim_pump 环扫，battle.c:124-137）：

```c
/* before：语义全在注释里 */
uint8_t *rec = g_ent_table[idx];
if ((rec[5] & 0x85) == 0 && rec[6] == 2) { camera_focus_ent(idx); ... }
```
```cpp
/* after：语义在代码里，注释只留证据 */
for (int i = 0; i < field_.units().count(); ++i) {
    auto u = field_.units().at(idx);
    if (u.is_actionable() && u.allegiance() == Allegiance::Player) {
        camera_.focus(idx);   // IDA 0x12D7B
        ...
    }
}
```

收益对照：P2（两份漂移注释）→ 记录布局唯一权威迁入 unit_ref.hpp；P6（sub_13512
等）→ `mark_acted()`，注释保留 `IDA 0x13512`，architecture.md 可继续按地址检索。

### 4.2 `BattleCtx` — 2211B 战场上下文

```cpp
// battle_ctx 布局（FDFIELD[3k+1]，§6.9 定案，注释全文迁入此类）
class BattleCtx {
public:
    static constexpr std::size_t kSize = 2211;

    void load(std::span<const std::uint8_t> block);  // dat_load_block 后接手
    void save_to(std::span<std::uint8_t> out) const; // SAV +0..2211

    int  shape_variant() const;   // [0]
    int  player_slots()  const;   // [1]
    int  enemy_count()   const;   // [2]
    TurnEventSlot turn_event(int i) const;   // [3..50]  16×3B
    void set_turn_event(int i, TurnEventSlot);
    TileEventSlot tile_event(int t) const;   // [51..82] 16×2B
    TreasureSlot  treasure(int t) const;     // [83..124] 14×3B
    std::span<std::uint8_t, 26> sprite_record(int i);  // [126..]，交错区（跨界读语义，
                                               // 返回 span 而非 struct——§6.9 教训）
private:
    std::array<std::uint8_t, kSize> b_{};
};
```

P3 的越界事故在此类永绝：`sprite_record`/`treasure` 内做一次集中边界检查，
替换散落各处的手写守卫。

### 4.3 `Package` / `DatArchive` / `Resources` — 资源所有权（RAII）

```cpp
namespace fd2 {

/* Package —— 一块已解码 DAT 块的独占所有权。替代 void* g_pkg_xxx + _size + free 三件套。 */
class Package {
public:
    Package() = default;
    Package(DatArchive&, const char* dat, int block);  // dat_load_block（IDA 0x…）
    ~Package() { std::free(p_); }
    Package(Package&& o) noexcept;  Package& operator=(Package&&) noexcept;
    Package(const Package&) = delete;                  // 复制即双 free，禁止

    std::size_t        size()  const { return size_; }   // 取代 g_pkg_xxx_size
    std::uint8_t*      data()        { return p_; }
    const std::uint8_t* data() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
};

/* Resources —— main 启动 8×装载的唯一归属（原 resources_load）。
 * 字段名沿用现名（g_pkg_fdother_31 等）以保留与 architecture.md 的对照，
 * 类型从 void* 变 Package：别名共享（g_death_fx_pkg = fdother_5）改为
 * 引用返回，别名永远同源——直接消灭 P1 的别名失步崩溃。 */
class Resources {
public:
    const Package& fdother_31() const;   // 0x53EEC
    const Package& death_fx()   const;   // = fdother_5（0x53A81 别名，注释保留）
    const Package& fdtxt0()     const;   // 0x53A7D
    Package load_temp(const char* dat, int block);  // 临时块（原 NULL 装载后 free 的用法）
};

} // namespace fd2
```

典型改写（statemachine.c:276-283 的 backdrop 装载）：

```cpp
auto backdrop = resources_.load_temp("FDOTHER.DAT", bg_lut[cfg[0]]);
if (backdrop) rle_.decode(backdrop, 0, 0, shadow_.band() + 32904, 456);
// 离开作用域自动 free——不再手写 free(backdrop)
```

### 4.4 `Screen` / `Palette` / `RleDecoder` / `IconCache` — gfx 层

```cpp
class Screen {            // VRAM 0xA0000 等价物 + 模式切换
public:
    std::span<std::uint8_t, FD2_VRAM_SIZE> vram();
    void set_mode(int m);                    // int386(0x10)（SDL 宿主下=呈现模式）
    void blit_rows(dst, w, src, pitch, ...); // IDA 0x11EB0
    void fade_to_black();  void fade_in();   // 0x1F882 / fade_in
};
class Palette {           // 768B + palette_apply_range 0x11D40
public:
    void apply_range(int start, int end, int dim);
    void add_range(int start, int end, int delta);   // cutscene 闪白用
};
class RleDecoder {        // 0x4E98D 帧头 u16 w,h + 2-bit 控制
public:
    void decode_frame(src, x, y, dst, pitch, flag);
    void blit_opaque(frame, dst);            // 0x4E29C 族
    void blit_transparent(frame, dst);       // 0x4E22A（立绘用，2026-09-08 事故注迁入）
};
class IconCache {          // FDICON.B24 0x11019 + FD2.TMP 缓存
public:
    void reset();  bool load_entry(int icon_idx, FILE*);
    bool frame(int cache_idx, int frame, std::span<const std::uint8_t>& out);
};
```

456 宽影子缓冲（`s_scene_shadow/s2`、`battle_shadow_layer`）归入一个
`CompositorSurface` 类（456×336，含 +32904/+107020/+109764 等区域常量），
把"区域常量+边界"收敛到一处——P3 同款收益。

### 4.5 `TextRenderer` / `DialogWindow` / `ConfirmDialog` — ui 层

```cpp
class TextRenderer {      // 0x15F84，296 xrefs 最高频入口
public:
    /* 真实压栈语义注释（fd2re 形参序/两族实参）全文迁入。
     * 参数改具名 struct，调用点从 8 个裸 int 变成语义字段： */
    struct BoxStyle { int typewriter_init; int line_step; int bg; int fg; };
    void render_box(std::span<std::uint8_t> dst, int pitch,
                    int str_id, const Package& glyphs, BoxStyle style);
};

class DialogWindow {      // 五缓冲/肖像/口型/滑入滑出（text.c s_dialog 半边）
public:
    void backdrop_load(int variant);       // 0x1956B
    void backdrop_restore();               // 0x196CB
    int  scroll_text();                    // 0x16E24
    int  dato_frame_stamp(int frame);      // 0x16559
    int  portrait_origin() const;  void set_portrait_origin(int);
    void speaker_reload(int variant);      // 0x29664
private:
    /* work/snap/box 三缓冲作为成员——P8 的"双半边 teardown 守卫"消失：
     * 每半边对象管自己的缓冲，析构即收尾。menu_buffers_teardown 的
     * 选人器半边同样对象化为 RosterMenu 的私有成员。 */
};

class ConfirmDialog {     // 0x19953 是/否 + 0x197E5 收尾动画
public:
    int run();   // 返回 1/0/-1；RAII 保证与 close_anim 成对（原版 12 调用点纪律 → 类型保证）
};
```

### 4.6 `MusicPlayer` / `SfxPlayer` — audio 层

```cpp
class MusicPlayer {   // AIL 序列等价（XMIDI→winmm / ADLMIDI）
public:
    void play(int id, int fade);   // music_play；id=-1 淡出
    void shutdown();
    bool ok() const;
};
class SfxPlayer {
public:
    void play(const Package& pack, int entry, int vol);  // sfx_play
    void set_gate(uint8_t g);  uint8_t gate() const;     // g_sfx_gate（SAV +12500）
};
```

接口不变、实现归位；`g_music_gate/g_sfx_gate` 成为成员，SaveFile 友元读写。

### 4.7 `GameState` 体系与 `StateMachine`

现状：6 张平行表（enter/exit/bgm/bgm_alt/roster_menu/config/duel_bg）+ 30 个
自由函数。数据（BGM 号、配置行）与行为（enter/exit 体）性质不同，**不要**全部
塞进虚函数——数据表保持数据驱动，行为才类化：

```cpp
// 数据面：StateDesc 保持表驱动（对应 dseg03/dseg02 五张静态表）
struct StateDesc {
    std::uint8_t bgm;          // 0x51E63
    std::uint8_t bgm_alt;      // 0x51E81
    std::uint8_t roster_menu;  // 0x523E7
    std::uint8_t duel_bg;      // 0x52470
    std::array<std::uint8_t, 31> config;   // 0x6236E（27 行 + 3 行零，全量迁移）
};

// 行为面：30 个 id → 8~10 个类（多数 ADV 对白态共享一个参数化类——
// 正确粒度是"行为模式"，不是"每个 id 一个类"）
class GameState {
public:
    explicit GameState(Game& g) : g_(g) {}
    virtual ~GameState() = default;
    virtual void on_enter() = 0;                 // g_state_enter[id]
    virtual void on_exit()  = 0;                 // g_state_exit[id]
    virtual int  run_scene();                    // 默认走 SceneController ADV 路径
    virtual int  post_action(int unit);          // g_state_post_action，默认 battle_end_check
protected:
    Game& g_;
};

class AdvDialogueState final : public GameState { /* 覆盖 st1-8/10-20/25-26 的
    "顺序渲染 FDTXT 串 + ++g_state 线性推进"模式，特例（st5/st8/st15 分支）
    用子类或模式参数表达，注释逐一保留 IDA 地址 */ };
class BattleDeployState final : public GameState { /* st9：实体槽 50/51/52 布置 */ };
class BattleState        final : public GameState { /* st21/st22/st29 */ };
class FormationState     final : public GameState { /* st23/24/27/28 编队分支 */ };
class TitleState         final : public GameState { /* st0 + 序章三相位 */ };
class EndingState        final : public GameState { /* st28 结局对白 */ };

class StateMachine {
public:
    void register_state(int id, std::unique_ptr<GameState> s);
    GameState& current();                        // g_state
    void request(int pending);                   // g_state_pending = 1/2
    int  run_inner_loop();                       // 0x25DB5-0x25E91 原样
    int  run_scene_for(int id);                  // state_run_scene 分派
private:
    std::array<std::unique_ptr<GameState>, FD2_STATE_COUNT> table_;
    std::array<StateDesc, FD2_STATE_COUNT>       desc_;
    int current_ = 0;  int pending_ = 0;  bool transition_busy_ = 1;
};
```

必须写进代码的**怪癖保留清单**（迁移时逐条验证）：

- enter 体内 `++g_state` 线性推进 → `on_enter()` 可改 `current_`，
  `run_inner_loop` 在 enter 返回后**重读** current 再调 run/exit（0x25F0B 的
  "重读 g_state" 行为原样保留）。
- boot 驱动新游戏路径调 `g_state_exit[0]` 但内部再改 state →
  `TitleFlow::start_new_game()` 中 `states_.current().on_exit()` 语义等价实现。
- `g_transition_busy` 初值 1（0x51AAC，0x1AD0C 面板门）→ 成员初始化值保留。

### 4.8 `EventScripts` — 90 项事件脚本

90 项函数指针表改为一个类的具名方法 + 显式分派（保留表驱动语义与
`g_pending_event_id` 255=无 的消费时点）：

```cpp
class EventScripts {
public:
    void dispatch(int unit);            // 0x11994-0x119A6 结构：pending!=255 才调
    void raise(uint8_t id);             // 置 g_pending_event_id 等价
    void tile_event_check(int x, int y, int mode);   // 0x13A44
    void turn_event_scan(int phase);                 // 0x1A813
private:
    // 90 个处理器 = 具名小方法。命名 = 事件语义（已在 event.c 注释里的那些
    // 直接起名），仍保留"表项序号"作为方法后缀锚点：
    void ev01_village_meet(int unit);   // IDA 0x34531 …
    void ev02_...(int unit);
    // 表项 [23]/[24] 位于 sub_34B6F 函数体内（IDA 未拆分段内入口）——
    // 该注释随方法迁移，永不再靠口口相传。
    uint8_t pending_ = 255;
};
```

收益：P4 消除；90 个方法天然可单测（注入 mock 的 `Game` 引用即可——不过
本项目的务实做法是纯逻辑才单测，见 §7.3）。

### 4.9 battle.c（4821 行）的域拆分

单文件按既有函数簇切六个类，**跨类调用保持原有调用序不变**（等价性优先）：

| 类 | 吸收的现有函数簇 | 说明 |
|---|---|---|
| `TurnLoop` | anim_pump（0x117E7）、battle_unit_turn、enemy_phase_check（0x13565）、battle_input_phase_restore | 主泵；`g_text_busy` 不变量注释随迁 |
| `MovementRange` | 移动域 BFS/洪泛、move_range_wait_pump（0x18B84）、行走原语四向（0x13185 族） | 候选字节仍存 FDFIELD 运行时字段（ABI） |
| `BattleMenus` | battle_system_menu（0x16F55）、行动菜单、道具/法术子菜单、g_menu_choice | `g_menu_choice` 变成员 |
| `AIPlanner` | g_ai_* 十余个全局 + sub_14237 普攻站位 + 行为 5 | **唯一真字段化**的战斗结构：`struct AIPlan { int score_target, target_x, ... };`（从不落盘） |
| `BattleResults` | battle_show_results（0x1AA1D）、战果汇总（0x24618）、exp/levelup | g_gold/g_exp_gained 成员 |
| `TileInfoPanel` | 0x1ACF3 格子信息区、地图总览（';'/PgUp） | — |

`battle_field_init`/`field_load_for_state`/地形/tile_lookup/宝箱交互 →
`domain/BattleField`（FDFIELD/FDSHAP 块归它所有，`Package` 持有）。
`g_cursor_*`/`g_scroll_*`/`camera_focus_ent` → `CameraCursor`。

### 4.10 duel（3370 行）与 tables（851 行）

- duel → `DuelScene`（背景装载/状态恢复，g_state_duel_bg/g_ending_duel_bg 归入）、
  `StrikeFlow`（决斗演出主流程）、`FxController`（fx 0..9/粒子/多段/大招，
  `g_anim_sfx_data` 注入）。§11 开放项（逐帧像素比对）在类注释中显式承袭。
- tables → 只读类型化视图，字节源不变：
  `WeaponEntry`（23B）、`EnemyBaseEntry`（10B）、`EnemyTemplateEntry`（24B）、
  `GrowthEntry`（11B）、`SpellLearnEntry`（12B）、`ClassCritTable`、
  地形攻防 `TileMods`（s_tile_atk_pct/def_pct 6 项）。全部 `const`，构造时
  校验长度，越界 abort（RE 不变量）。现 `const uint8_t *xxx_table_entry(int)`
  五件套成为对应类的 `entry(i)` 方法。

### 4.11 scene.c（2489 行）与商店/编队 UI

`SceneController`（ADV 常规路径：fade→music(10)→背景→overlay→compose 循环，
即 statemachine.c 里 scene_render_host/state_run_scene 的宿主侧）、
`ShopMenu`（三页货源：+3/+15/+23，shop_stock_build 0x26A0D）、`RosterMenu`
（0x2AF28，含自己的三缓冲成员）、`SaveSlotMenu`（0x26331 picker + 0x25F3F
选档）、`TempleMenu`/`InnMenu`（复活/宿屋计价）。每个菜单类私有持有自己的
work/snap/backup 三缓冲，析构收尾——P8 根治。

## 5. 全局变量迁移策略：两步走，每步可运行

114 个 extern 一次性清零不现实，采用**过渡桥 + 归属清理**两步：

1. **归属即转换**（Phase 2-9 各阶段内）：某子系统转类时，其全局变为该类成员；
   该类提供 `static Sys& instance()` 过渡访问器，供**尚未转换的调用方**继续编译。
   过渡访问器在头文件标注 `// TRANSITIONAL：Phase N 移除`。
2. **清零**（Phase 10）：调用方全部改为注入引用后，删除全部 `instance()` 与
   残留 extern。验收 grep：`^extern` 命中数为 0（fd2/ 头内），`::instance()`
   命中数为 0。

两条纪律：

- 新类代码**禁止**再 new 全局；迁移期允许旧文件经 `instance()` 访问新类，
  不允许新类反向依赖旧全局（依赖方向必须单调收敛）。
- 每个 Phase 结束时统计剩余 extern 数并写进提交说明，保证可见的收敛曲线。

## 6. 分阶段实施计划

原则：**每阶段结束游戏可构建、可运行、可存读档**；一次一个子系统；重构提交
与行为提交严格分离。预计规模按当前 wc -l 计。

---

### Phase 0 — 安全网与 C++ 构建切换（机械改动，0 逻辑变更）

**目标**：全部代码以 C++20 编译通过并运行；等价性验证手段就位。

任务：
1. 基线固化：当前 fd2re.exe 与其 FD2.SAV 存档样本入库（save/golden/），
   作为等价性对照物。
2. `.c`→`.cpp` 批量改名 + 机械修复：`void*`→`T*` 显式 `static_cast`
   （malloc/dat_load_block 赋值点，预计数百处，sed 可覆盖 90%）、
   字符串字面量→`const char*` 形参兼容、enum 隐式转换等编译错清零。
   **本提交禁止任何顺手修改。**
3. build.ps1：`/std:c11` → `/std:c++20 /permissive- /EHsc`；`src -Filter *.c`
   → `*.cpp`。保留 /W4 /utf-8 /Zi 与 .env 固定路径。
4. ASAN 构建脚本确认可跑（asanbuild/ 沿用）。
5. 存档互操作脚本 `tools/savcmp.py`（§7.2）。

验收：构建 OK；标题→序章→一场完整战斗→存档→读档全流程通过；新 exe 能读
Phase 0 前旧 exe 写的存档。
风险：批量 cast 引入错误转换——对策：只允许 `static_cast`，review 时 grep
`reinterpret_cast`（Phase 0 应为 0 处）。

---

### Phase 1 — 命名空间与基础类型（小）

任务：建 `namespace fd2`；新目录骨架；`Vec2i`/`Direction`/`Rect` 等公共值类型；
`fd2::ensure(cond, msg)`（RE 不变量断言，等价现 fprintf+abort 模式）；决定
`Package`/`DatArchive` 并先只引入类型定义不迁移调用。

验收：构建运行无差异；fd2/ 头出现命名空间规范。

---

### Phase 2 — data + gfx + host + audio 底层（~1.9k 行）

顺序：resource.c→`Package/DatArchive`；video.c→`Screen/Palette/Rle/IconCache`；
tables.c→类型化表视图（§4.10）；save.c→`SaveFile`；audio.c→`MusicPlayer/
SfxPlayer`；input/timer/host→`Keyboard/BiosClock/SdlHost`。

任务要点：每个类用 §5 的 `instance()` 过渡；`Resources` 收编 main.c 的
resources_load 与 8 个启动包；`g_death_fx_pkg` 别名改引用返回。

验收：全流程回归；extern 数首次下降并记录。
风险：音频时序敏感（XMIDI 回调）——纯搬移不改调用时序，diff 审核函数体零变化。

---

### Phase 3 — 实体域（~1.5k 行 + fd2.h 瘦身）

任务：`EntityTable`/`UnitRef` 全量访问器落地（§4.1）；roster/ent 两块存储归
`EntityTable`；`CameraCursor`/`FieldMap`/行走原语四向归 domain；fd2.h 的
实体段与 entities.h 合并为唯一权威；battle.c 中的 `rec[...]` 访问**暂不改**
（Phase 6 随调用点转换）。

验收：实体相关全流程（布置/战斗/商店买卖/复活/转职）回归；fd2.h 从 168 行
显著瘦身。

---

### Phase 4 — 文本与对白（~1.3k 行）

任务：`TextRenderer`（BoxStyle 具名化）、`DialogWindow`（五缓冲成员化）、
`ConfirmDialog`（RAII 成对纪律）、`wait_key_anim/wait_key_ticks` 归入
ui/host 边界；`menu_buffers_teardown` 双半边就地对象化（选人器半边随
`RosterMenu` 在 Phase 7 完成收编）。

验收：对白逐字上屏/滚行/肖像/口型全回归；§13.77 五缓冲等价开放项状态不变。

---

### Phase 5 — 场景与过场（~2.8k 行）

任务：`SceneController`（state_run_scene ADV 路径 + compose 循环 +
scene_overlay_stamp 事故注释迁入）、`CutscenePlayer`（ui_script_exec）、
`ShopMenu`/`SaveSlotMenu`/`TempleMenu`。

验收：城镇全场景（酒馆/商店×3/教会/宝箱/神秘商店）+ 序章链 + 存档槽菜单回归。

---

### Phase 6 — 战斗域（~5.7k 行，最大阶段）

任务：§4.9 六类 + `BattleField` + `SpellCast`。battle.c 内所有 `rec[...]`
访问在此阶段改为 `UnitRef` 访问器；`AIPlanner` 真字段化（唯一例外，§3.3）；
`g_state_post_action` 钩子暂由 `StateMachine` 持有函数表，Phase 8 改虚分发。

验收：移动域/行动菜单/攻击决斗/AI 1:1/36 法术/战况板/回合循环/存读档全回归；
伤害公式单测上线（§7.3 第一批）。
风险：本阶段触碰面最大——严格按函数簇小步提交（TurnLoop→Menus→Movement→
AI→Results→Panel→Field→Magic，8 个子提交）。

---

### Phase 7 — 决斗与特效（~3.4k 行）

任务：`DuelScene`/`StrikeFlow`/`FxController`；§11 逐帧比对开放项在类注释
显式标记；`g_state_duel_bg`/`g_ending_duel_bg` 归属 `DuelScene`。

验收：决斗全 fx 0..9 + 大招 + 结局蒙太奇回归。

---

### Phase 8 — 状态机与事件收口（~3.3k 行）

任务：`StateDesc` 数据表迁移（五张静态表全量搬运，含 2026-09-06 的 27 行更正
注释）；30 个状态体归类（§4.7 的 8~10 类）；`g_state_post_action` 改
`GameState::post_action` 虚分发；`EventScripts` 90 方法（§4.8）；startup.c →
`TitleFlow`；main.c → `Game` 组合根。怪癖保留清单（§4.7）逐条验证。

验收：27 态全走查 + 新游戏/续档/中断续战三入口 + 结局链。

---

### Phase 9 — 清理与过渡桥拆除

任务：删除全部 `instance()` 过渡访问器；残留 extern 清零；`sub_XXXX` 命名
收尾（语义仍未知者保留地址名+注释）；死亡变量清点（如 0x51A42 s_byte_51a42
这类原版死变量**保留**并注释，不做"优化"）。

验收：grep 清单（§5）达标；全流程回归 ×2。

---

### Phase 10 — 测试补强与文档

任务：tests/ 单测扩容（§7.3）；README 更新目录说明与新构建参数；本方案文档
标记完成状态；architecture.md 增补一节"代码组织 v2 映射表"（§3.4 的表）。

---

## 7. 行为等价保障体系

### 7.1 不可破坏的等价性清单（每个 Phase 验收逐条过）

1. FD2.SAV 读写：新旧 exe 存档**互相**可读，字节布局零变化
   （0x59CB 记录 + 2600 步长槽区 + 22983 校验和）。
2. RNG：`fd2_rand`（0x4EBE3，16 位零扩展语义）与 `srand(bios_tick())` 热身序
   （main 0x25D90）原样；**禁用 `<random>`**。
3. 帧驱动：anim_pump 每键一轮、4-tick 场景动画钟（0x53F52）、`wait_key_ticks`
   的 tick 差/午夜回绕语义。
4. `g_text_busy` 输入泵不变量（battle.c:99-112 注释全文保留）。
5. 音频触发时序：music_play 调用点/参数、BGM 表、淡出路径。
6. 所有"死变量/死路径"照录不删（byte_51A42、EB 补丁 jmp、0x25E59 折叠等）。

### 7.2 存档互操作（tools/savcmp.py）

- `savcmp a.sav b.sav`：字节 diff，白名单区段（无——布局固定，理论上应全同）。
- 流程：旧 exe 存 → 新 exe 读+再存 → 与旧档 diff；双向都做。
- 每 Phase 验收执行一次；样本覆盖：序章、战斗中（中断续战）、章节间、结局前。

### 7.3 单元测试（tests/，新增 doctest 单头库）

只测**纯逻辑**（无渲染/无时序依赖），首批目标：

- `battle_strike_formula` 伤害/命中/暴击（含地形攻防表、特殊单位豁免）。
- 移动域 BFS 与曼哈顿攻击形状（mode 3/4/5）。
- `shop_stock_build` 三页货源拼装（27 行 config 表驱动）。
- `SaveFile` 编解码往返：真实 FD2.SAV 载入→重编码→逐字节比较。
- RLE 解码 golden 向量（blk10 覆盖层等真实块）。
- `BattleCtx` 访问器偏移 = 文档偏移（静态断言 + 真实块抽查）。

### 7.4 人工回归清单（docs/qa-checklist.md，新建）

按状态 0-29 建一条 3-5 步的走查项（从 architecture.md §3.1 直接派生），
每 Phase 跑受影响子集，Phase 8/9 全量。ASAN 构建至少在 Phase 0/6/9 各完整
跑一遍序章+战斗。

## 8. 构建系统改造

- build.ps1：编译参数 `/std:c++20 /permissive- /EHsc /W4 /utf-8 /Zi`，
  源过滤 `*.cpp`；`.env` 机制、SDL2/ADLMIDI 链接、输出占用回退逻辑全部不动。
- 新增测试构建目标（build.ps1 `-Test` 开关或 tests/build.ps1）：tests/*.cpp
  + 被测纯逻辑编译单元，链接同一运行时。
- ASAN 路径沿用 asanbuild/ 思路，参数追加 `/fsanitize=address`。
- 不引入 CMake/vcpkg——现有 .env 固定路径机制工作良好，重构期不动地基。

## 9. 编码规范（本项目 C++ 子集）

| 项 | 规定 |
|---|---|
| 标准/编译 | C++20，MSVC `/permissive-`；不用模块 |
| 命名 | 类型 `PascalCase`；函数/变量 `snake_case`（**沿用原函数名**，与 architecture.md 可互相 grep——`anim_pump()` 保持叫 `anim_pump()`）；常量 `kName`；成员尾缀无匈牙利 |
| 命名空间 | `fd2`；子命名空间仅限 `fd2::battle` 等大域，不嵌套过深 |
| 注释 | 每个**对应原版函数**的方法，首行注释保留 `IDA 0x…` 地址与既有证据结论；重构**不删不改**证据内容，只随函数搬家 |
| 错误处理 | 预期 IO 失败 → 返回码/`bool`；RE 不变量破坏（state 越界、表越界）→ `fd2::ensure` abort（与现 fprintf+abort 等价）；**不使用异常**做控制流 |
| 所有权 | `Package`/`unique_ptr` 持有；禁止裸 new/delete；禁止 `shared_ptr`（生命周期树状且单线程） |
| 容器 | 存量 ABI 缓冲用 `std::array`/`std::span`；新增逻辑允许 `std::vector`/`std::string`；不用 STL 算法炫技，可读优先 |
| 禁用 | `<random>`、多线程、`constexpr` 重度元编程、运行期反射式注册 |
| 提交纪律 | 重构提交信息带 `refactor:` 前缀并注明"行为零差异 + 验证方式"；行为修正独立提交 |

## 10. 风险登记册

| 风险 | 概率 | 对策 |
|---|---|---|
| Phase 0 批量 cast 改错 | 中 | 只许 `static_cast`；grep `reinterpret_cast`=0；全流程回归 |
| 重构混入行为变化 | 中 | 提交前 `git diff` 逐块审"是否纯搬家"；怪癖清单（§4.7/§7.1）逐条核对 |
| 战斗域大阶段失控 | 中 | 拆 8 个子提交；每子提交可运行 |
| 音频/时序类回归不易察觉 | 低 | Phase 2 只搬不改；BGM 表逐项 diff |
| 五缓冲/决斗帧等开放项被误判为回归 | 中 | §7.1 清单声明"开放项状态冻结"；对照 Phase 0 基线 exe |
| 单人长期工程的阶段疲劳 | — | 每 Phase 独立可玩、可存档；extern 收敛曲线写入提交说明提供进度感 |

## 11. 里程碑总览

| 里程碑 | 内容 | 规模 | 结束时游戏状态 |
|---|---|---|---|
| M0 | C++20 全量编译 + 安全网 | 0 逻辑 | 与现版等价 |
| M1 | 底层四层对象化（P1-P2） | ~2k | 等价，extern 首降 |
| M2 | 实体 + 文本（P3-P4） | ~2.8k | 等价，魔数实体访问器就绪 |
| M3 | 场景 + 战斗（P5-P6） | ~8.5k | 等价，最大 touch 面完成 |
| M4 | 决斗 + 状态机收口（P7-P8） | ~6.7k | 全部子系统类化 |
| M5 | 清零 + 测试 + 文档（P9-P10) | — | extern=0、instance()=0、单测上线 |

每个里程碑都满足：构建绿、全流程可玩、存档双向互操作、ASAN 干净。
