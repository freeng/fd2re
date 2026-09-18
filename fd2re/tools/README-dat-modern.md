# fd2dat — FD2 资源 DAT 的现代格式替代读写

`FD2.EXE` 全部游戏资源的现代格式**解包（导出）→ 回包（重建）→ 观察（预览）**
工具链。格式依据 = `docs/architecture.md` 已定案条目（§5/§6.5–6.11/§13.4/
§13.15/§13.28/§13.33/§13.43/§13.47/§13.77），解码器逐条对齐 `fd2re/src/*.c`
的逆向实现；不依赖任何探针/动态设施。

## 一、种类分析（结构普查，2026-09-09 全量扫描）

游戏侧资源文件全集（§13.77 fopen 定案）：`ANI / BG / DATO / FDFIELD / FDMUS /
FDOTHER / FDSHAP / FDTXT / FIGANI / TAI .DAT` + `FDICON.B24`。
`TITLE.DAT` 为发行盘孤儿资产，**字节等同 FDOTHER blk7**（嵌套容器，SHA256
已对拍）——非独立格式。

| 容器/块 | 结构 | 证据 |
|---|---|---|
| LLLLLL 容器 | 6B 魔数 + `u32 starts[]`（含尾哨兵=文件长），块体紧排 | §5、dat_load_block 0x111BA |
| 单帧图 image | `[u16 w][u16 h]` + 逐行 4-op RLE 或字节 RLE 严格耗尽 | BG/TAI 56 块、TITLE 子块、FDOTHER blk10（62×26 场景覆盖层，stamp_frame_opaque 0x4EBFF） |
| 帧表动画包 wh_anim | 头 `[u8 count][u8 flag][u16 played][u32 param]`（字节 1..8 原样保留）+ `u32 off[count]`@8 + 帧；帧 = `[i16 x][i16 y][5B 原样][u16 w][u16 h]` + 逐行 4-op（流@13）。经典 WH 族 = flag=0/played=count/param=0 特例；消费：blit_frame_flat 表@+8、fx_scene_play 读 tpose[0]=count、ending_anim_part2 读 ftab[2]、unit_anim_play 帧[5]=音效/[6]=延时 | §6.6、duel.c 0x2DFC8/0x2E2B0 群（2026-09-09 统一定案，FIGANI 264 块全量自洽） |
| LMI1 帧包 lmi1 | `'LMI1'` + `u16 N` + `u32 off[N+1]`；帧体多格式混合 | §6.5/§6.6（FDOTHER blk5/6/9/13/14/29） |
| LUT 帧 | LMI1 内 256B 定长帧 = 颜色重映射表 | FDOTHER blk3（tile4_stamp_lut 0x4E016） |
| DATO 口型包 dato | `u32 dir[4]@0`（dir[0]=16）+ 4 帧 80×80 字节 RLE | §13.33（136 块） |
| sprite24 包 | `[u16 w][u16 h][u16 cnt]` + `u32 off[cnt+1]` + 24×24 4-op 帧 | FDICON.B24（140×12 帧）、FDOTHER blk1 |
| 原始字面帧包 raw_frames | `u32 off[]@0` + `[u16 w][u16 h]`+裸像素 定长条目 | FDOTHER blk2（78×484B，径向菜单图标，battle.c blk2_frame） |
| 点阵字库 font | 32B/字形 = 16 行 × u16 位阵（MSB 先行） | FDOTHER blk4（glyph_blit 0x4EDC2，58368=1824×32） |
| tile 块 tiles | `[u16 w][u16 h][u16 n]` + `u32 off[n]` + 线性 4-op 流 | FDSHAP 偶块 ×33（§6.7） |
| 属性数组 attrs | 4B/tile 数组（可多于 tile 数） | FDSHAP 奇块（g_shape_map，tile_lookup） |
| 地图 field_map | `[i16 w][i16 h]` + 4B/格：tile=cell[0]\|(cell[1]&3)<<8、flags=cell[2]&0x1F、cell[3]=洪泛候选 | §6.8（33 块 = FDFIELD 3k） |
| 战场上下文 battle_ctx | 131+26F：[0]shape [1]P [2]E、80B 中段、14×3B 宝箱、26B×F 记录、尾 5B | §6.9（FDFIELD 3k+1） |
| 布阵层 deploy | `u16 P+E` + 6B/条 | §6.9（FDFIELD 3k+2） |
| 文本 text | `u16 串偏移表` + u16 token 流（-1 终止） | §13.43（34 块） |
| XMIDI music | `FORM XDIRINFO` + `CAT XMID{FORM XMID{TIMB,RBRN?,EVNT}}` | §7.1（15 曲） |
| 占位轨 music_placeholder | 3 字节 | FDMUS 0/2/5/7/9 |
| 音效包 sfx_pkg | 嵌套 LLLLLL + u32 off[]，条目 = 8bit 无符号 PCM @11025Hz | FDOTHER blk31/77/78（sfx_trigger） |
| 图像包 image_pkg | 嵌套 LLLLLL，子块 = 单帧图 | FDOTHER blk7（≡ TITLE.DAT） |
| ANI 动画 anim | 173B 头（i16 帧数@165）+ `[u16 len][u16 ops][4B]` + 操作码流（op0..9） | §13.4（9 动画） |
| 调色板 palette | 768B VGA 6bit ×256 | FDOTHER blk0/8/57/76/99/101/102 |

## 二、现代格式选型

| 种类 | 现代替代 | 说明 |
|---|---|---|
| 所有像素族 | **TexturePacker JSON（hash 格式）+ 图集 PNG×2** | `<name>.png` = 调色板观看图；`<name>.idx.png` = 索引+流级透明位（回包真值）。WH 族帧头 x/y → `spriteSourceSize`、画布 320×200 → `sourceSize`；mode/fmt 等进 `meta.fd2`/帧 `fd2` 字段 |
| FDMUS | **.mid（标准 MIDI）** + `.events.json` | division=60 + tempo 500000μs ≙ 120 tick/s（§7.1 时基恒定，原 FF51 不进 SMF）；events.json 保留原 delta/VLQ 字节 → pack 无损重建 XMIDI |
| 音效包 | **.wav**（11025Hz 8bit unsigned 单声道） | 可直接试听 |
| FDTXT | `.strings.json`（token 结构化）+ `.txt`（可读） | token 语义见 txtfmt.py 头注 |
| FDFIELD | `.json`（cells 4B 原始 + tiles/flags 便捷字段；ctx/deploy 同） | 可编辑后回包 |
| 调色板 | `.palette.json` + `.pal`（JASC/GIMP） | 6bit 原值保留 |
| ANI | 逐帧 PNG + `.frames.json`（操作码原始字节） | PNG 供观察，ops 供无损回包 |
| 无法结构识别的块 | `.unknown.bin` + JSON | pack 原样回包 |

## 三、回包保证（写路径）

- **像素级**：所有像素类帧从 `*.idx.png` 重编码重建，pack 侧把重建块
  重新解码并与导出 PNG 逐像素校验（`dat_pack.py` 自动执行）——重建 DAT
  与原版**渲染等价**。
- **字节级**：结构类块（容器目录/文本/地图/ctx/deploy/JSON 音频/XMIDI/
  ANI ops/font/RAW/LUT/调色板）回包与原文件**逐字节一致**；RLE 压缩块
  的字节一致率逐帧记录于各块 JSON 的 `byte_equal`/manifest
  `pixel_roundtrip`（像素类帧解码唯一，字节差异 = 等价重编码）。

### 编码器反推（为何 RLE 块难逐字节复刻）

对拍实验（2026-09-09）：4-op RLE 的原版编码器是**行内代价贪婪**，但各族
tie-break 不一致——TAI blk4 平局选「字面+跳过」、blk17 省 1B 才用隔点；
TITLE blk1 平局选「拆分字面+实心」；FIGANI f0 平局选「单条长字面吞掉
2 像素实心」。即不同资产批次用了不同编码器参数，逐字节复刻不经济。
fd2dat 改用**行内贪婪重编码**（跳过/隔点/实心/字面，字面段内 >=3 同色
断给实心；线性时间——曾用行内最优 DP，平坦底图每像素回枚 64 切分 ×3
操作，BG 56 块满屏大色块命中最坏情形需数分钟，贪婪后 BG 全程 1.5s、
FIGANI 2118 帧 26s），像素精确一致由 pack 校验兜底；编码器无法字节
复现的帧/块落 `raw_hex`（原流字节）优先保证字节级无损。dither 空洞位
的索引仅是解码器实现细节（透明位不写目标面），校验按渲染相关像素比较。

> **2026-09-09 idx 真值修复**：pack 重编码与像素校验此前误读 `<name>.png`
> （调色板观看图，R=调色板色），导致打包流写入调色板色而非色号索引，
> 且校验与同一张错图自洽对拍呈假绿。现统一读 `<name>.idx.png`（索引+流级
> 透明真值）；BG 因此由 PIXEL-EQUIVALENT 修正为 BYTE-EXACT，其余像素资源
> 体积变化为修正后的真实重编码差值。导出器 image 种类补齐字节 RLE 变体
> （FDOTHER blk10/blk15）与 `raw_hex` 兜底，manifest/块 JSON 种类恢复一致。
> 同日修 **font 位序**：export_font 曾按 rd_u16 高位在左，而 0x4ED7A 定案
> 为行 u16 字节交换后 MSB 先行（内存序 B0=左半 8px、B1=右半）——此前
> 全字库左右半互换、文字预览花屏；export/pack 对称修正后 blk4 仍字节
> 精确，文字预览复现真实文案（如 str410「要記錄戰況嗎?」）。

## 四、用法

```bash
cd fd2re/tools
# 导出（DAT → 现代格式目录 + manifest.json + 种类普查）
python dat_export.py FDOTHER.DAT          # 单文件；产出 tools/dat-modern/FDOTHER/
python dat_export.py FDICON.B24
# 回包（现代格式目录 → DAT；自动像素校验；--verify 与原版 SHA 对拍）
python dat_pack.py dat-modern/FDOTHER --verify ..\..\FDOTHER.DAT
# 观察（只读现代格式目录，模拟 fd2re 消费路径）
python dat_preview.py map  dat-modern/FDFIELD dat-modern/FDSHAP 3 --fdother dat-modern/FDOTHER --out preview.map3.png
python dat_preview.py anim dat-modern/FIGANI 0 --fdother dat-modern/FDOTHER --out preview.figani0.png
python dat_preview.py portrait dat-modern/DATO 0 --fdother dat-modern/FDOTHER --out preview.dato0.gif
python dat_preview.py icons dat-modern/FDICON.B24 0 --out preview.icon0.png
python dat_preview.py title dat-modern/FDOTHER --out preview.title.png
python dat_preview.py ani   dat-modern/ANI 1 --out preview.ani1.gif
python dat_preview.py music dat-modern/FDMUS 4
python dat_preview.py text  dat-modern/FDTXT 0 410 415 --fdother dat-modern/FDOTHER --out preview.text.png
```

## 五、验证结果（2026-09-09 全量 12 资源终跑）

回包（`dat_pack.py --verify AUTO`，与 yl2 仓库根的原版文件 SHA256 对拍）：

| 资源 | 回包结论 | 像素校验 |
|---|---|---|
| ANI.DAT | **BYTE-EXACT**（SHA256 与源一致） | —（ops 原字节） |
| FDFIELD.DAT | **BYTE-EXACT** | —（结构 JSON） |
| FDMUS.DAT | **BYTE-EXACT**（15 曲 XMIDI 重建全对） | — |
| FDTXT.DAT | **BYTE-EXACT** | — |
| BG.DAT | **BYTE-EXACT**（idx 真值修复 + raw_hex 兜底，2026-09-09） | 56/56 |
| FDICON.B24 | PIXEL-EQUIVALENT（+14B；非等价帧 raw 兜底） | 225/225 重编码帧一致 |
| TAI.DAT | PIXEL-EQUIVALENT（-326B） | 56/56 |
| TITLE.DAT | PIXEL-EQUIVALENT（+1B） | 7/7 |
| DATO.DAT | PIXEL-EQUIVALENT（+20B） | 544/544 |
| FIGANI.DAT | PIXEL-EQUIVALENT（+77165B；2118 帧贪婪重编码） | 2118/2118 |
| FDOTHER.DAT | PIXEL-EQUIVALENT（-9752B；0 unknown） | 1167/1167 |
| FDSHAP.DAT | PIXEL-EQUIVALENT（+356881B，tile 线性贪婪压缩率略差） | 8256/8256 |

- BYTE-EXACT = 回包与原文件逐字节一致，现代格式目录可完全替代原文件。
- PIXEL-EQUIVALENT = 结构块字节全对；RLE 压缩帧为像素等价重编码（体积
  与原版同量级），pack 侧逐帧解码对拍真值图全绿 —— 渲染结果与原版一致。
- 性能（2026-09-09 编码器线性化 + 校验数组化后）：BG 1.5s、FIGANI 26s、
  FDSHAP/FDOTHER 数秒级，12 资源全矩阵 < 1 分钟；`dat_pack.py` 校验阶段
  每 5s 输出心跳进度。

观察（`dat_preview.py`，只消费现代格式目录）：
- `map`：FDFIELD JSON + FDSHAP tileset PNG + FDOTHER 调色板 → 战场地图
  PNG（state3 20×20、state0 24×24 等已出图，草地/树木/宝箱/栅栏齐全）。
- `title`：blk74+75（palette blk76）→ 标题画面；`title.scroll` blk69..73
  → 320×735 卷轴合成。
- `portrait`：DATO 块 → 4 帧口型 GIF（icon39 旁白像已出）。
- `icons`：FDICON 图集 → 12 相位条 + GIF。
- `anim`/`ani`：FIGANI WH 帧 / ANI 全屏动画 → GIF。
- `text`：FDTXT JSON + FDOTHER 字形 PNG → 对话渲染 PNG（16×16 汉字+影）。
- `music`：.mid 时长/通道/program 报告；`sfx`：.wav 时长报告。

### 已知挂号（无损兜底，不影响回包正确性）

- ~~FIGANI 19 块、FDOTHER blk10 未识别~~ → 2026-09-09 全部定案（帧表
  动画包统一格式 + blk10=字节 RLE 单帧图），**12 资源 0 个 unknown 块**。
- 帧表动画包块头字节 1..8（flag/played/param）与帧头字节 4..8 语义
  挂号（消费方只证 count/表@+8），导出原样保留、回包原样还原。
- lmi1/sprite_pkg/图集内非字节等价的 RLE 帧落 `raw_hex`（原流字节），
  图集像素仍为原流解码真值。

