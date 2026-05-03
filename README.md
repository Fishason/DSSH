<h1 align="center">DSSH</h1>

<p align="center">
  <img src="icon.png" alt="DSSH icon" width="96" height="96">
</p>

<p align="center">
  <b>Nintendo 3DS 中文 SSH 客户端</b><br>
  上屏 ANSI 终端 · 下屏自绘软键盘 + 拼音 IME · RSA 公钥认证<br>
  从 3DS 直接连服务器跑 tmux + claude-code，离不开沙发也能 coding
</p>

<p align="center">
  <img alt="platform" src="https://img.shields.io/badge/platform-Nintendo%203DS-FE0016?logo=nintendo3ds&logoColor=white">
  <img alt="license" src="https://img.shields.io/badge/license-MIT-blue">
  <img alt="build" src="https://img.shields.io/badge/build-devkitARM-orange">
</p>

<p align="center">
  <img src="docs/media/preview.gif" alt="DSSH 实机演示" width="540"><br>
  <sub>实机 New 2DS XL · 上屏 ANSI 终端 · 下屏软键盘 + 时钟 + 螃蟹</sub>
</p>

<p align="center">
  <a href="https://github.com/Fishason/DSSH/releases/latest/download/demo.mp4">
    完整 1m42s 实机演示（10 MB MP4）
  </a>
</p>

<p align="center">
  <img src="docs/media/poster.jpg" alt="实机：在 Claude Code 中用拼音 IME 输入中文" width="720"><br>
  <sub>实机 New 2DS XL · 上屏 Claude Code 输入「你好啊！请问您是谁，你可以做什么」 · 下屏字母页 + CHN 模式 + Shift</sub>
</p>

---

## 功能

- **完整 ANSI/VT100 终端**：tmux 状态栏、claude-code spinner、box-drawing
  边框、256-color、TrueColor、Braille — 都正常显示
- **中文渲染**：内置 Zpix 12px 像素字体覆盖 21,000+ 个 CJK 统一汉字 +
  Terminus 6×12 ASCII，中英混排 baseline 对齐
- **自绘软键盘**：iOS 风格 3px 圆角键 + 平滑下沉动画，字母 / 符号双页
- **拼音输入法**：rime-ice 顶部 30 万词典 + 缩写匹配（`nh` → 你好）+
  前缀 fallback（`nihaoz` 自动回退到 `nihao`）+ 候选条游标导航
- **RSA-4096 公钥认证**：libssh2 + mbedTLS，私钥放 SD 卡读
- **物理键全映射**：D-pad 方向键、修饰键 hold-style（L=Shift / Y=Ctrl /
  X=Alt）、Circle Pad scrollback / mouse-wheel
- **Anthropic 红螃蟹吉祥物**：底行左右奔跑，点击会躲开 🦀
- **隐藏 debug 页面**：双击右上 ENG/CHN 进，看 SSH 字节流 + 物理键说明 +
  螃蟹开关

## 目录

- [安装](#安装)
- [服务器侧准备](#服务器侧准备一次性)
- [配置 config.ini](#配置-configini)
- [按键说明](#按键说明)
- [输入法用法](#输入法用法)
- [Debug 页面](#debug-页面)
- [从源码构建](#从源码构建)
- [项目结构](#项目结构)
- [致谢](#致谢)
- [License](#license)

---

## 安装

DSSH 跑在 **破解过的 3DS / 2DS / New 3DS** 上。需要 Homebrew Launcher
（HBL）或 FBI 一类的 CIA 安装器。

### 方案 A — `.cia` 安装（推荐）

1. 下载本仓库 [Releases](../../releases) 里的 `DSSH.cia`（约 14 MB）
2. 拷到 SD 卡，路径任意（比如 `/cias/DSSH.cia`）
3. 启 FBI → SD → 选 `DSSH.cia` → `Install CIA`
4. HOME 菜单里出现橙色 DSSH 图标

### 方案 B — `.3dsx` 直跑

1. Releases 里下 `dssh.3dsx`
2. 拷到 SD 卡 `/3ds/dssh/dssh.3dsx`
3. 启 HBL → 选 DSSH

### 方案 C — `3dslink` WiFi 推送（开发用）

```bash
# 3DS 上 HBL 按 Y 进 "Waiting for 3dslink..."
3dslink -a <3DS-LAN-IP> 3dssh.3dsx
```

---

## 服务器侧准备（一次性）

3DS 上的 libssh2 用的是 mbedTLS 后端，**硬编码不支持 ed25519**。所以要
专门给 3DS 生成一把 RSA-4096 公私钥对，原来 PC 上的 ed25519 不动：

```bash
# 1. 在你的 PC 上生成 3DS 专用 RSA 密钥
ssh-keygen -t rsa -b 4096 -f ~/.ssh/id_rsa_3ds -C "3ds-ssh-client"

# 2. 把公钥复制到服务器的 authorized_keys
ssh-copy-id -i ~/.ssh/id_rsa_3ds.pub user@your-server.example.com

# 3. 验证从 PC 用新 RSA 能连
ssh -i ~/.ssh/id_rsa_3ds user@your-server.example.com 'echo OK'
```

**安全建议**：编辑服务器的 `~/.ssh/authorized_keys`，给这把 RSA 那一行
前面加 `from="<家里公网 IP>"`，这样即使 SD 卡丢了，钥匙也只能从你家网
络登入。

把 **私钥** `~/.ssh/id_rsa_3ds` 通过读卡器复制到 SD 卡的
`/3ds/3dssh/id_rsa`（注意：3DS 装 CIA 后 title-id 路径不同，但**配置 +
密钥**始终读 `sdmc:/3ds/3dssh/`）。

> ⚠️ SD 卡是明文。物理持有 SD 的人就能登服务器。务必加 `from="..."` IP
> 限制或 `command="..."` 命令限制。

---

## 配置 config.ini

把仓库里的 `sd_template/3ds/3dssh/config.ini.example` 拷到 SD 卡的
`/3ds/3dssh/config.ini`，按你的服务器改：

```ini
host       = your-server.example.com
port       = 22
user       = ubuntu
key_path   = sdmc:/3ds/3dssh/id_rsa
passphrase =
```

| 字段 | 说明 |
|------|------|
| `host` | 服务器 IP 或域名 |
| `port` | 端口（默认 22） |
| `user` | 服务器登录用户名 |
| `key_path` | 私钥路径，`sdmc:/...` 是 3DS 标准 SD 路径前缀 |
| `passphrase` | 私钥口令；建议留空（SD 卡上输 passphrase 体验差） |

最终 SD 卡布局：

```
sdmc:/3ds/3dssh/
├── config.ini
└── id_rsa
```

---

## 按键说明

### 物理键

| 键 | 功能 | 备注 |
|---|---|---|
| **A** | Enter（EN）/ **拼音 buffer 当英文直出**（IME） | CN 模式下不小心打了英文？按 A 直接把 buffer 作为 ASCII 发出去并清空，不用切换模式重打 |
| **B** | Backspace / 消拼音 buffer | hold-style 自动重复（最快 60/秒） |
| **X** | Alt 修饰 | hold-style，按住时下次按键加 Alt |
| **Y** | Ctrl 修饰 | hold-style，按住 Y + 点 c → Ctrl-C |
| **L** | Shift 修饰 / **+ Circle Pad → 切右窗格** | 见下面 [tmux 窗格滚动](#tmux-分屏滚动) |
| **R** | 切换中/英输入模式 | 右上 ENG/CHN 显示当前模式 |
| **SELECT** | Esc | 单点立即发 |
| **START** | 退出程序 | |
| **Space**（软键盘）| 普通空格（EN）/ **提交当前高亮候选**（IME） | 跟 sogou/fcitx 一致 |
| **Shift + .** | **。**（中文句号 U+3002） | 不论 EN/CN 模式都生效 |
| **D-pad ↑↓** | 方向键 / IME 翻页 | IME buffer 非空时翻候选页 |
| **D-pad ←→** | 方向键 / IME 选词 | IME buffer 非空时移动候选游标 |
| **Circle Pad ↑↓** | 滚动 scrollback / tmux mouse-wheel | 默认目标左/上窗格，**L 按住 → 右/下窗格** |

> 长按 D-pad 或 B 键：250ms 启动重复，0.5s 后 12/秒，1.5s 后冲到 60/秒。

### tmux 分屏滚动

3DS 没有真正的鼠标光标，tmux 的 mouse-wheel 事件按 (col, row) 路由到对应
窗格。DSSH 默认发 `(1,1)` 命中**左/上窗格**；按住 **L** 时改发 `(60,12)`
命中**右/下窗格**。所以 vertical-split tmux 里：

| 操作 | 效果 |
|---|---|
| Circle Pad ↑↓ | 滚动**左**窗格 |
| L 按住 + Circle Pad ↑↓ | 滚动**右**窗格 |

### 软键盘

下屏占满软键盘，两页：

- **字母页**（默认）：QWERTY 布局，含 `,` `.` 标点 + Tab + 大空格
- **符号页**（左下 `123` 切换）：1234567890、!@#\$%^&\*() 整齐对齐两行 +
  其他常用符号 + 反斜杠

任何键支持 hold-style 修饰组合，例如：**按住 Y + 点 b** = `Ctrl-B`
（tmux prefix）。

### 状态条（顶部 30 px）

```
┌──────────────────────────────────────────────────────┐
│ [SFT]   候选条 / 拼音 buffer / IME 候选词     [CHN]  │
└──────────────────────────────────────────────────────┘
```

- **左槽 [STA]**：3 字母指示当前修饰键（SFT/CTL/ALT 持续亮，
  ENT/BSP/ESC/`R→C` 闪 200ms）
- **中段**：CN 模式拼音 buffer + 候选词；EN 模式空白
- **右槽 [ENG/CHN]**：当前 IME 模式，**双击此处** 进入 debug 页面

---

## 输入法用法

### 全拼

CN 模式下按字母键 → 顶部候选条出现拼音 + 候选：

```
ni       → 年 你 牛奶 娘 念   (page 1/52, total 256)
nihao    → 你好 你好吗 你好啊 拟好 你好呀
shijie   → 世界 世界上 世界杯 世界各地 世界里
```

- **A 键** 或 **空格键** 提交当前高亮候选
- **D-pad ←→** 移动高亮（同一页内）
- **D-pad ↑↓** 翻页
- **触屏** 直接点候选词提交
- **B 键** 删拼音 buffer 一个字母

### 缩写（声母）

每个多音节词都额外有一条声母拼音条目，**输入声母也能召回**（频率打 0.3
折，所以全拼仍排在前）：

```
nh → 你好（在第 8 个候选）
wm → 我们（首个）
sj → 世界
zw → 中文
xx → 谢谢
```

按 D-pad ↓ 翻页或 ←→ 移动游标找到目标后 A 键提交。

### Prefix 回退

输错字母多打了一个？引擎自动用最长有效前缀匹配，红色显示多出的部分：

```
buffer:  niha[oz]    ← 绿色 niha + 红色 oz
候选:    依旧显示 nihao 的结果
```

按 B 键消掉红色尾巴回到正常。

### 修饰键不被 IME 拦截

CN 模式下按住 **Y + c** 仍然发 `Ctrl-C`，按住 **L + a** 仍然发 `A`。修饰
键优先于 IME 路由，跑 vim/tmux/claude-code 不受影响。

### 误打英文逃生：A 键直出

CN 模式 + 拼音 buffer 非空时，按 **A** 把 buffer 当英文 ASCII 发到 SSH
并清空。例如不小心在 CN 模式打了 `cd /etc`：候选条会显示一堆奇怪中文，
此时按 **A** 一次，SSH 直接收到 `cd /etc`，不用退格再切模式重打。

> 区别：**Space** 提交高亮候选（中文上屏），**A** 直接把字母原样发出去。

---

## Debug 页面

**右上 ENG/CHN 双击**（500 ms 内点两次）→ 进入 debug 页面：

- 标题 + 退出提示（再单击 CHN/ENG 即退）
- **recv hex**：最近 32 字节 SSH 接收数据，用来排查 ANSI 协议问题
- **物理键绑定列表**：上述按键说明的速查
- **MASCOT: ON/OFF** 按钮：关掉螃蟹（默认 ON）

---

## 从源码构建

### 依赖

- Linux x86\_64（在 Ubuntu 22.04 测过；其他发行版需要相应调整）
- [devkitPro / devkitARM](https://devkitpro.org/wiki/Getting_Started)
  release 65+，GCC 14.2.0
- Python 3.10+ + Pillow（生成字体 + 词典）
- ImageMagick（可选；图标处理用 Pillow 也可）

### 步骤

```bash
# 1. 装 devkitPro
wget https://apt.devkitpro.org/install-devkitpro-pacman
bash install-devkitpro-pacman
sudo dkp-pacman -S 3ds-dev 3ds-mbedtls 3ds-libpng 3ds-zlib

# 2. clone + 进项目
git clone https://github.com/Fishason/DSSH.git
cd DSSH

# 3. 交叉编译 libssh2（一次性，输出到 $DEVKITPRO/portlibs/3ds/lib/）
bash build-libssh2.sh

# 4. 装系统字体（Terminus 提供 ASCII / box-drawing）
sudo apt install fonts-terminus

# 5. 下载字体源（Zpix）
bash tools/fetch_fonts.sh

# 6. 生成字体 atlas（→ source/font_data.c，~3 MB）
python3 tools/gen_font.py

# 7. 下载 + 生成拼音词典（→ romfs/pinyin_dict.bin，~13 MB）
bash tools/fetch_pinyin_dict.sh
python3 tools/gen_pinyin_dict.py

# 8. 编译 .3dsx
make

# 9. （可选）打 .cia
bash tools/install_cia_tools.sh   # 装 bannertool + makerom 到 ~/bin
make cia                           # 输出 DSSH.cia
```

### 测试 IME 引擎（host 端，不需 3DS）

```bash
make test-ime
```

会编译 `tools/test_ime.c` 链 `source/ime_pinyin.c`，跑 9 个常用查询用例
（`ni→你`，`nihao→你好`，`nh→你好`，等等）。

---

## 项目结构

```
DSSH/
├── 69633.PNG                  # 源图标 (162×102)
├── icon.png                   # .3dsx / SMDH 图标 (48×48，从源图生成)
├── app.rsf                    # makerom CIA spec
├── Makefile                   # 主构建（make / make cia / make test-ime）
├── build-libssh2.sh           # libssh2 + mbedTLS ARM 交叉编译
├── source/
│   ├── main.c                 # 主循环、SSH receive、UTF-8 边界
│   ├── ssh_client.{c,h}       # libssh2 封装
│   ├── config.{c,h}           # SD 卡 config.ini 解析
│   ├── terminal.{c,h}         # ANSI/VT100 解析器（fork skmtrd）
│   ├── renderer.{c,h}         # citro2d 渲染（终端、文本、CJK）
│   ├── keyboard.{c,h}         # 物理按键 + IME 路由
│   ├── softkb.{c,h}           # 软键盘 + 候选条 + debug 页面
│   ├── ime_pinyin.{c,h}       # 拼音引擎
│   ├── mascot.{c,h}           # 螃蟹吉祥物
│   ├── font_atlas.{c,h}       # 字体索引
│   └── font_data.c            # 字体位图（gen_font.py 生成）
├── tools/
│   ├── fetch_fonts.sh         # 下载 Zpix
│   ├── gen_font.py            # 字体 atlas 生成器
│   ├── fetch_pinyin_dict.sh   # 下载 rime-ice
│   ├── gen_pinyin_dict.py     # 词典 → 二进制
│   ├── test_ime.{c,sh}        # host 端 IME 测试
│   ├── gen_cia_assets.py      # 图标/banner 派生
│   └── install_cia_tools.sh   # bannertool + makerom 安装
├── romfs/                     # gitignored — 装载 pinyin_dict.bin
├── data/                      # gitignored — 字体源 + 词典源
└── sd_template/               # SD 卡部署模板
    ├── README.md
    └── 3ds/3dssh/config.ini.example
```

## 架构概览

```
SSH server (somewhere on the internet)
     ▲ libssh2 over mbedTLS-RSA-4096
     │
┌────┴──────────────────────────────────────────────────┐
│  main.c poll loop @60fps                              │
│   ├─ ssh_read → softkb_record_recv → utf8 reassemble  │
│   │                ↓                                  │
│   │   terminal_write_n → ANSI parser → cell grid      │
│   ├─ hidScanInput → keyboard_handle_input             │
│   │   └─ IME mode? → ime_input_letter / page / select │
│   ├─ hidTouchRead → softkb_touch                      │
│   │   ├─ candidate strip hit → ime_select             │
│   │   ├─ key hit → keyboard_emit_for / ime_input      │
│   │   └─ badge double-tap → debug_mode toggle         │
│   └─ render: top = renderer_draw_terminal             │
│              bot = softkb_draw + clock + mascot       │
└───────────────────────────────────────────────────────┘
     │
   citro2d (3DS 2D rendering)
     │
   GPU (top 400×240 + bottom 320×240, 24-bit color)
```

详细 milestone 进度参见 commit history（M0 → M9 全部提交）。

---

## 致谢

- **[skmtrd/3dssh](https://github.com/skmtrd/3dssh)** — 原日文版，复用了
  ANSI/VT100 解析器、UTF-8 边界处理、citro2d 框架基础
- **[rime-ice](https://github.com/iDvel/rime-ice)** — 拼音词典源
  （commit `3f57a6f6` pin）
- **[Zpix Pixel Font](https://github.com/SolidZORO/zpix-pixel-font)** —
  12px CJK 像素字体（OFL 1.1）
- **[Terminus TTF](https://terminus-font.sourceforge.net/)** — ASCII /
  box-drawing 像素字体
- **[libssh2](https://www.libssh2.org/)** + **[mbedTLS](https://www.trustedfirmware.org/projects/mbed-tls/)** —
  SSH/TLS 协议栈
- **[devkitPro](https://devkitpro.org/) libctru / citro2d / citro3d** —
  3DS 用户态运行时 + 渲染
- **[carstene1ns/3ds-bannertool](https://github.com/carstene1ns/3ds-bannertool)**
  + **[3DSGuy/Project_CTR makerom](https://github.com/3DSGuy/Project_CTR)** —
  CIA 打包工具

## License

MIT — 见 [LICENSE](LICENSE)。

字体、词典、上游 SSH/TLS 库各有各的许可证（OFL / GPL / BSD / MIT），
分发二进制时请遵守对应条款。
