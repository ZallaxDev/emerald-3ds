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

它们由 `3ds_port/scripts/gen_chinese_font.py` 从**开源 CJK 字体**渲染：

```sh
python 3ds_port/scripts/gen_chinese_font.py \
    --charmap build/upstream/charmap.txt \
    --font /path/to/NotoSansCJKsc-Regular.otf
```

字体本身（Noto Sans CJK SC，SIL OFL 1.1）**不进仓库**：14 MiB，许可是字体
自己的。生成出来的两张 PNG 进仓库，因为它们是这个 port 的作品，`release_audit`
的白名单里有说明。

## 译文

`tools/locales/zh_cn/texts.json` 是 `{源文件: {标签: 中文}}`，标签与 pret 源码
里的完全对应。

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
