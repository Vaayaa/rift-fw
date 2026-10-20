# RIFT 裂 · FM-1 贝斯固件 v0.4

Mr.V / PUZAOSI。基于 Dinghy（zvenson）、Felucca（Leo Kuroshita, Hügelton Instruments）和 SLOOP（isod89），GPL-3.0。

## 编译（Mac，需要 Docker 在运行）

```sh
cd rift-fw
pip3 install Pillow mido python-rtmidi
tools/get_toolchain.sh                         # 杰理工具链 -> ~/.jieli/toolchain
git clone --depth 1 --branch AC79NN_SDK_V1.2.1_2023-12-13 \
    https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK.git ~/fw-AC79_AIoT_SDK
./build.sh                                     # -> build/rift.fwsc
```

编译报错时，把终端输出完整贴回对话。构建脚本会检查：内存是否超限、是否有 64 位除法、是否误用了芯片 ROM 地址形式的常量。

## 刷机

1. 先用当前固件的备份功能存好你的音色和音序。
2. USB 接上 FM-1，然后运行：

```sh
python3 tools/fm1_install.py build/rift-0.3.fwsc   # 写入前会先询问确认
```

也可以不用终端：在 Chrome 打开 FiMba 的网页安装器（jadamsowers.github.io/fm1-fimba，Install 一节），选 “Or a firmware file” 选择 .fwsc 安装。
刷回其他固件或官方 V15：打开 thefirmwarehub.com（Chrome 或 Edge）。
刷坏了：开机时按住 OCT− 进入 USB 救援模式，再重新安装。

## 操作

| 控件 | 作用 |
|---|---|
| PLAY | 播放 / 停止（同时发 MIDI Start/Stop 和时钟） |
| K1–K4 | PLAY 页：CUT / RES / ENV / 爆 |
| FX · LFO · ENV · ARP 按住 | 升 RISE · 滚 ROLL · 哇 WOW · 停 STOP |
| 两键同按（A 组） | FX+LFO 筑 BUILD · LFO+ENV 碎 SHRED · FX+ARP 空 VOID · ENV+ARP 冻 FREEZE |
| PLAY / SEQ 页：SEL 按键 | 效果键换组：A 组 升 滚 哇 停 / B 组 俯冲 DIVE · 门 GATE · 叠 STACK · 倒 REV（顶栏显示 B） |
| 双击效果键 | 锁定效果，再按一次解锁 |
| 按住效果键时 | K1–K4 调这个效果的参数；OCT+ = 留 KEEP，OCT− = 弃 ABORT |
| HOME | 单击回主屏 · 双击 RETURN（宏回到预设值）· 长按 0.5 秒 PANIC |
| SEQ | 单击进 SEQ 页；再按一次在 GEN / PERF 之间切换 |
| SEQ 按住 + PLAY | HARD 变异（下一小节生效） |
| SEQ 按住 + REC / HOME / OCT− / FX / EDIT | SOFT 微变 / UNDO / DROP / FILL（按住期间）/ A·B 互换 |
| SEQ 按住 + F3 G3 A3 B3 C4 D4 E4 F4 | 单独重生成 RHY ACC SLD PIT OCT MOD CHN TRN |
| SEQ 按住 + G4–G5 白键 | 切换音序槽 1–8 |
| REC 按住 + 弹键（播放中） | TAP-PITCH：只改旋律，节奏不动 |
| EDIT | VOICE → FX → SYS 页循环（FX 页显示两个核的 CPU 占用和峰值） |
| VOICE 页：SELECT 旋钮 | 换引擎 ACID / RUMBLE / REESE / GROWL / SCREAM（旋钮值不变） |
| VOICE 页：SEL 按键 | 音色骰子：按引擎各自的好听范围随机一个音色；长按 0.6 秒撤销 |
| FX 页：SEL 按键 | 旋钮切第二层：PHASR 相位器量 · RATE 速度 · SQUASH OTT 深度 · HARM 低频谐波 |
| SYS 页 K1–K4 | SYNC 时钟 INT/AUTO/EXT · CH 本机 MIDI 通道 · USB 立体声/8 轨分轨 · LIGHT 按键背光 |
| REC 按住 + 拧 K1–K4（播放中） | 动作录制：每一步记下旋钮值，跟着 pattern 循环回放（旋钮上出现小方块，数值跟着实际播放的值变化）；REC + HOME 清除 |
| SEQ 按住 + SAVE | 把当前 pattern 的 DNA 发给其他 RIFT（USB MIDI SysEx），对方下一小节换上 |
| GLO | 参考鼓开 / 关 |
| SAVE | 存储音色和 8 个音序槽的 DNA |
| SELECT | PLAY 页调 BPM · SEQ 页选音阶 · FX 页选延迟时值 · VOICE 页换引擎 |
| ALGO / PRESET | 切音色预设（10 个，跨 5 个引擎）/ 切音序槽（下一小节生效） |
| 琴键 | 停止时弹贝斯（按住越久越亮；同键快速双击 = 重音）；播放中按键 = 按音阶级数移调 |

## MIDI 输入：让 DAW 来弹（0.5.1）

只收本机通道（SYS 页 CH，默认 ch1）。

- **用法**：SYS > **SYNC = INT**，不要按 PLAY。这样 Ableton 的时钟和 Start 不会启动 RIFT 自己的音序器；
  音序器在走时，收到的音符是「按音阶移调」，不是弹出来。
- **音符**：收到的音符 n 发出的音高 = 标准 MIDI 音符 n − 24（低两个八度）。例：发 MIDI 60（Ableton 叫 C3，
  中央 C）→ 发出 MIDI 36 的音高（65.4 Hz，Ableton 叫 C1）。想让 RIFT 发出某个音高，就发「那个音的 MIDI 号 + 24」。
  面板琴键往外发的音符同样减了 24，录下来回放音高一致。
- **力度**：≥ 100 = 重音（accent），< 100 = 普通。
- **滑音**：前一个音还按着时来的新音 = slide（legato 重叠）。
- **Program Change**：

  | PC | 预设 | 引擎 |
  |---|---|---|
  | 0 | PHUTURE 88 | ACID |
  | 1 | SQUELCH | ACID |
  | 2 | RUBBER | ACID |
  | 3 | DIRT | ACID |
  | 4 | BUNKER | RUMBLE |
  | 5 | PRESSURE | RUMBLE |
  | 6 | BREAK REESE | REESE |
  | 7 | FIRESTORM | REESE |
  | 8 | BLOCK ROCK | GROWL |
  | 9 | ACID SIREN | SCREAM |
  | 100–107 | 音序槽 1–8（播放中下一小节生效） | — |

  和拧 ALGO 到那个预设一样（旋钮值全换成预设值）；正在按着的音不断，直接换成新音色。屏幕顶部显示 PRESET n / PATTERN n。
  其他 PC 号忽略。
- CC：74 cutoff · 71 resonance · 1 爆（同前）。
- USB MIDI 设备名：**RIFT FM-1**（macOS 端口同名；以前连过的机器可能还显示旧名字）。

## MIDI 输出（用于 TouchDesigner）

- ch1：音符。
- CC20 cutoff · CC21 爆 · CC22 包络 · CC23 重音电容 · CC26 效果进度 · CC27 裂纹强度。
- ch16：音符 36–43 为效果开关，48 = DROP，49 = 落地，51 = 小节，52 = 拍，55 = PANIC。
- 时钟：24 ppqn，并发送 Start / Stop。

## 引擎（VOICE 页第三个旋钮 CHR 随引擎变）

| 引擎 | 声音 | CHR | 预设 |
|---|---|---|---|
| ACID | 303 式二极管梯形滤波，重音电容，滑音 | WAVE 锯齿 → 方波 | PHUTURE 88 · SQUELCH · RUBBER · DIRT |
| RUMBLE | 正弦 + 小喇叭频段 + 低频扩散回授环（Techno rumble，踢鼓也送进环里） | RUMBLE 回授量 | BUNKER · PRESSURE |
| REESE | 三把锯齿：中间一把 + 两把去掉低频的失谐锯齿，带缓慢漂移 | DETUNE | BREAK REESE · FIRESTORM |
| GROWL | 两算子 FM + 共振 SVF + 两个随八分音符扫动的共振峰梳状滤波 | FM 调制深度 | BLOCK ROCK |
| SCREAM | 两把失谐锯齿各进一个梯形滤波，第二个高出一段 | SPLIT 两个滤波的距离 | ACID SIREN |

## 冻 FREEZE（ENV+ARP）

按下后在下一步把刚刚 2 / 4 / 8 步的高频段（爆 之后）冻住循环，抬起后按 4 步量化落地并淡出。按住时 K1 LEN 长度 · K2 LOOP 循环音量 · K3 LIVE 现场声部音量（拧到 0 只听循环）· K4 FADE 淡出时间。双击锁定。

## 双核（0.4 起默认关闭）

0.4 起默认单核：这台测试机在双核满负荷时出现过寄存器被改坏的崩溃（与 Melodee #17 / fm1-x0x #10 记录的硬件问题一致）。SYS 页拧 SELECT 选 SET 2，下次上电起用双核。单核时由负载调节器在 CPU 超过 83% 时依次暂停相位器、OTT、HARM，2 秒低于 65% 再恢复（FX 页显示 LT1–3）。


AC791N 的第二个核负责爆之后的所有处理（降采样、箱体、crush、延迟、参考鼓、总线、tape stop）。输出因此晚一个块（5.8 ms）。开机时按住 REC：本次只用一个核（声音完全一样，只是没有那 5.8 ms）。第二个核出错会自动停掉、退回单核，FX 页右上角会显示原因；双核时崩溃过，直到断电前都用单核（1 CORE BARRED）。单核时 OTT、相位器、HARM 自动关闭。SYS 页的 MEM 行是双核内存自检（核心 0 读错 / 乱写、核心 1 读错 / 乱写、检查批数），CRASH 行显示本次上电后最近一次崩溃。双核的启动和防护方式来自 Melodee、fm1-x0x 和 JIANT（GPL-3.0）。

## B 组效果（SEL 换组）

| 键 | 效果 | 按住时旋钮 |
|---|---|---|
| FX 俯冲 DIVE | 每个音从高处俯冲到音高（按键越久越深） | K1 时长 · K2 深度 |
| LFO 门 GATE | 高频段按 16 步节奏门开合 | K2 深度 · K3 门型（4 种） |
| ENV 叠 STACK | 叠上五度和八度的锯齿 | K2 量 |
| ARP 倒 REV | 整个输出倒着放（按 4 步量化落地） | — |

## 同步和多台

- SYNC：INT 自己的速度；AUTO 有外部时钟就跟、没有就用自己的；EXT 只跟外部。跟随时顶栏速度变绿，Start / Stop 也跟着走。
- 时钟和 DNA 从 USB 和 3.5mm MIDI 输入口都能收。FM-1 的 3.5mm 口只能输入，所以 RIFT 只从 USB 发时钟和 DNA；跟随外部时钟时不再往外发时钟，避免环路。
- 几台不经过电脑互连，需要一个 USB MIDI host 盒子把一台的 USB 转给其他几台的 3.5mm 输入；经过电脑就在 TD 或 DAW 里转发。
- CH：每台设一个通道，音符和 CC 走这个通道收发；ch16 的事件音符照旧。

## USB 音频

立体声（44.1 / 48 kHz）或 8 轨分轨（44.1 kHz）：1-2 总输出、3-4 贝斯、5-6 参考鼓、7-8 延迟回声。切换时 FM-1 会在 USB 上断开一下再以新设备出现。设备名改为 RIFT FM-1（macOS 可能还显示旧名字）。

## v0.3 不包含

暂无计划内遗留项。
