# 简体中文（Simplified Chinese）

中文构建（`--port-lang zh_cn`）有两部分：

- **Port 自己的界面**：下屏的标签、选项页和错误信息，通过
  `include/3ds_locale.h` 的 `CTR_TEXT(...)` 第 8 个参数给出。
- **游戏正文**：来自 [rh-hideout-chinese/pokeemerald-expansion](https://github.com/rh-hideout-chinese/pokeemerald-expansion)
  的译文，保存在 `tools/locales/zh_cn/texts.json`，由
  `tools/localize_chinese.py` 在编译前注入源码树。

游戏的 `GAME_LANGUAGE` 仍是英语：中文是 port 的一层，不是另一张卡带。

## 编译

```sh
python tools/bootstrap.py --make --port-lang zh_cn -j8
```

已有的树里只重编时：`make -C 3ds_port PORT_LANG=zh_cn`。切换 `PORT_LANG`
会清空所有对象（Makefile 比较 `build/config` 里的 flags）。

## 字形编码（patch 0083）

汉字是**双字节**：首字节 0x01–0x1E，去掉 0x06 和 0x1B（`é` 和 `É`，
`POKéMON` 要用），尾字节 0x00–0xF6。

```
'啊' = 0100      '中' = 10A1      '冴' = 1E70
```

`src/chinese_text.c` 的 `SheetIndex()` 把这对字节换算成字模行号：

```
slot = ((hi - 1 - (hi > 0x06) - (hi > 0x1B)) << 8) | lo
```

**标点是双字节**，首字节 0x1F（汉字不用），尾字节 0xF7–0xFF。因为这张
charmap 的单字节码位已经全部被占用（假名就占了 0x01–0xAE），标点拿不到
自己的单字节。`。！？` 原本映射到半角字节（0xAD / 0xAB / 0xAC），patch 0083
把它们改指到 0x1F F7 / FA / FB，于是中文构建里画的是全角字形。

标点按这个顺序排在汉字之后，从 slot 7025 开始：

```
。 、 ， ！ ？ ： — ～ ・
```

## 字模

`3ds_port/assets/fonts/chinese_small.png` 和 `chinese_normal.png` 是
`.latfont` 形状：256 px 宽、2bpp、4 色、16×16 一个字模、一行 16 个——和游戏
自己的拉丁字体同一种格式，所以 port 现有的字体加载器（`3ds_compat.c`）
原样就能用。

`3ds_port/scripts/gen_chinese_font.py` 从**上游汉化工程的现成位图**装配：

```sh
git clone https://github.com/rh-hideout-chinese/pokeemerald-expansion /tmp/cn
python 3ds_port/scripts/gen_chinese_font.py --fork /tmp/cn
```

汉字与标点全部拷贝自该仓库提交的四个图集：

| 来源 | 内容 |
| :--- | :--- |
| `graphics/fonts/chinese_normal.png` | 6771 个汉字（槽 0–7024） |
| `graphics/fonts/chinese_small.png` | 同上，小字号 |
| `graphics/fonts/latin_normal.png` | 9 个全角标点（槽 0x30–0x3F） |
| `graphics/fonts/latin_small.png` | 同上，小字号 |

它没有 ttf、没有字库、也没有生成脚本——字形是作者直接提交的位图，无法从
某个字体复现。拷贝它们，本移植版的字才与它承载的译文出自同一手。

脚本要改三件事才装得进来：

- 源图的槽 7025 是杂图（无 charmap 对应），本移植版的 9 个标点因此从 **7026**
  起，不从 `SheetIndex(0x1E71)` 算出的 7025 起；
- **标点不在中文图集里**，在 `latin_normal.png` / `latin_small.png` 的
  0x30–0x3F 上（那是被改成中文标点的假名槽位）。拷贝时要做 2bpp 值重映射：
  拉丁图以 0/3 为背景，中文图以 0 为背景，直接拷贝会得到噪声；
- 输出的图集保持 `.latfont` 几何（一行 16 字模），port 现有字体加载器原样可用。

标点的宽度照上游的 `GetChineseFontWidthFunc`：小字号下 `。` 是 6，`—～` 是 7，
`、，！？：・` 是 5；大字号下除 `・` 是 7 外都是 12。它们画在格子左侧，宽度
截掉右侧空白，所以看起来偏左是设计如此。

## 译文

译文来自
[rh-hideout-chinese/pokeemerald-expansion](https://github.com/rh-hideout-chinese/pokeemerald-expansion)，
由本移植版整理、校验，并适配到自己的字形与占位符体系。那个仓库没有写明
许可，因此这里的转载以标注来源为前提；原作者若有异议，应改为由使用者自行
获取译文，而不是随仓库分发。

`tools/locales/zh_cn/texts.json` 是 `{标签: 中文}`，标签与 pret 源码里的完全
对应——**不是按文件索引**：那个 fork 把部分文本挪到了别的文件（主菜单的在
`src/main_menu.c`，本树的在 `src/strings.c`），只有标签是两边都稳定的。

```sh
python tools/localize_chinese.py --tree build/upstream            # 注入
python tools/localize_chinese.py --tree build/upstream --check    # 只校验
python tools/localize_chinese.py --tree build/upstream --status   # 覆盖率
```

只有当前源码树**真的有**的标签会被注入：fork 为 expansion 内容新增的标签
会被忽略，树里有而译料没有的标签保持英文，所以任何阶段都能编译。

### 注入会跳过的文本

三种情况保持英文，`--status` 会列出：

- 占位符在这棵树里不存在（fork 为 expansion 新增的 `{BACKGROUND}`、
  `{REGION}` 等）；
- 字符不在 charmap 里；
- 英文原文已经变了（标签仍在，但译料对应的是 fork 里的版本）。

要把它们补齐，改 `texts.json` 里对应的条目即可，不用动脚本。

## 测试

```sh
# 字形表与 charmap 是否一一对应（宿主上跑，秒级）
python 3ds_port/tests/chinese_text_test.py --tree build/upstream

python tools/release_audit.py --repo . --strict
python -m compileall -q tools 3ds_port/scripts 3ds_port/tests builder
```
