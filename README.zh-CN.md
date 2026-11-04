<p align="center">
  <img src="https://i.postimg.cc/XJRQB9cc/logo2.png" alt="Emerald 3DS" width="480">
</p>

<h1 align="center">Emerald 3DS — 简体中文</h1>

<p align="center">
  <strong>以简体中文重游丰缘，在 3DS 的双屏上。</strong><br>
  宝可梦绿宝石原生 3DS 移植版的简体中文构建。
</p>

<p align="center"><a href="README.md">English</a> · <a href="README.pt-BR.md">Português</a> · 简体中文</p>

---

中文构建把 [Emerald 3DS](https://github.com/ZallaxDev/emerald-3ds) 译成简体
中文：游戏仍是那一份，上屏跑正片，下屏是取代 START 菜单的触摸界面，可选的
体素世界照旧。

游戏的 `GAME_LANGUAGE` 仍是英语——中文是这个移植版的一层，不是另一张卡带。
这样在英文版和中文版之间切换时，存档仍能通用。

## 已翻译的部分

- **游戏正文 8525 条**（全部 10522 条中的 81%）：地图对话、训练家、
  战斗、菜单、背包、宝可梦图鉴、摘要、电脑、商店、宝可梦领航员、
  华丽大赛、对战开拓区与快捷聊天。
- **字形**：6771 个汉字与 9 个中文标点，编码方式与
  [rh-hideout-chinese](https://github.com/rh-hideout-chinese/pokeemerald-expansion)
  的汉化一致（双字节，避开 `é`/`É` 与引擎控制码）。**字形位图直接取自该
  工程**，转成游戏自己的 `.latfont` 格式，所以笔画与它所承载的译文同一手。
- **编译入口**：`PORT_LANG=zh_cn`，与葡语版并列。

## 尚未完成

- **下屏触摸界面仍是英文**：`CTR_TEXT` 的 208 处调用已经可以接受中文，
  但界面文案还没有逐一写入。
- **含文字的图形未重绘**：属性图标、招式窗口、图鉴页标题、标题画面的
  `PRESS START` 等仍是英文美术。
- **名称输入键盘**仍是英数字符表；玩家名上限 7 字节，即 3 个汉字。
- **剩余 1997 条文本保持英文**，原因两类：译文用到了本移植版字形表里没有的
  占位符（如 `{BACKGROUND}`），或上游英文原文已变、需要重新核对。

## 已知问题

- 长句需要重新断行。中文一字全角，消息框一行约 17 字，而译文按 GBA 原版
  的英文行宽排版，个别位置会出现不自然的换行。
- 体素世界与昼夜光照不受影响，但其中的文字仍是英文。

## 怎么玩

你需要**自己的**日版/美版宝可梦绿宝石 ROM（BPEE）。本仓库与任何发布包中
都不含游戏内容。

编译（环境要求见[开发指南](docs/DEVELOPMENT.md)）：

```sh
python tools/bootstrap.py --make --port-lang zh_cn
```

产物在 `build/upstream/3ds_port/emerald3ds.3dsx`。字形、编码与译文的细节见
[docs/CHINESE.md](docs/CHINESE.md)；安装与声音设置见
[docs/INSTALLATION.md](docs/INSTALLATION.md)。

## 文档

| 文档 | 内容 |
| :--- | :--- |
| [中文构建说明](docs/CHINESE.md) | 汉字编码、字模生成、译文注入与测试。 |
| [开发指南](docs/DEVELOPMENT.md) | 编译环境与工作流。 |
| [安装指南](docs/INSTALLATION.md) | 安装、更新与声音设置。 |
| [架构](docs/ARCHITECTURE.md) | 这个移植版怎么组成。 |
| [来源说明](docs/PROVENANCE.md) | 每一部分的出处与许可状态。 |

## 致谢

- **简体中文译文**：来自
  [rh-hideout-chinese/pokeemerald-expansion](https://github.com/rh-hideout-chinese/pokeemerald-expansion)，
  由本移植版整理、校验并适配到自己的字形与占位符体系。若原作者对这份转载
  有异议，请提出，我们会改为由使用者自行获取译文。
- **字形位图**：取自
  [rh-hideout-chinese/pokeemerald-expansion](https://github.com/rh-hideout-chinese/pokeemerald-expansion)
  提交的图集，转载时标注来源；按 `NOTICE.md` 的约定，原作者若有异议即移除。
- **3DS 移植**：Daniel Cazalla
  ([ZallaxDev](https://github.com/ZallaxDev/emerald-3ds))。
- **反编译**：[pret/pokeemerald](https://github.com/pret/pokeemerald)。

宝可梦是任天堂、Creatures 与 GAME FREAK 的商标。本项目是非营利爱好者作品，
与上述公司无关联。许可条款见 [LICENSE-PORT.md](LICENSE-PORT.md) 与
[NOTICE.md](NOTICE.md)。
