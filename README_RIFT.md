# RIFT 裂 · FM-1 贝斯固件 v0.2

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
python3 tools/fm1_install.py build/rift-0.2.fwsc   # 写入前会先询问确认
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
| 两键同按 | FX+LFO 筑 BUILD · LFO+ENV 碎 SHRED · FX+ARP 空 VOID · ENV+ARP 冻 FREEZE |
| 双击效果键 | 锁定效果，再按一次解锁 |
| 按住效果键时 | K1–K4 调这个效果的参数；OCT+ = 留 KEEP，OCT− = 弃 ABORT |
| HOME | 单击回主屏 · 双击 RETURN（宏回到预设值）· 长按 0.5 秒 PANIC |
| SEQ | 单击进 SEQ 页；再按一次在 GEN / PERF 之间切换 |
| SEQ 按住 + PLAY | HARD 变异（下一小节生效） |
| SEQ 按住 + REC / HOME / OCT− / FX / EDIT | SOFT 微变 / UNDO / DROP / FILL（按住期间）/ A·B 互换 |
| SEQ 按住 + F3 G3 A3 B3 C4 D4 E4 F4 | 单独重生成 RHY ACC SLD PIT OCT MOD CHN TRN |
| SEQ 按住 + G4–G5 白键 | 切换音序槽 1–8 |
| REC 按住 + 弹键（播放中） | TAP-PITCH：只改旋律，节奏不动 |
| EDIT | VOICE / FX 页（FX 页显示两个核的 CPU 占用和峰值） |
| VOICE 页：SELECT 旋钮 | 换引擎 ACID / RUMBLE / REESE / GROWL / SCREAM（旋钮值不变） |
| VOICE 页：SEL 按键 | 音色骰子：按引擎各自的好听范围随机一个音色；长按 0.6 秒撤销 |
| FX 页：SEL 按键 | 按键背光 OFF / LOW / MID / HIGH（SAVE 时保存） |
| GLO | 参考鼓开 / 关 |
| SAVE | 存储音色和 8 个音序槽的 DNA |
| SELECT | PLAY 页调 BPM · SEQ 页选音阶 · FX 页选延迟时值 · VOICE 页换引擎 |
| ALGO / PRESET | 切音色预设（10 个，跨 5 个引擎）/ 切音序槽（下一小节生效） |
| 琴键 | 停止时弹贝斯（按住越久越亮；同键快速双击 = 重音）；播放中按键 = 按音阶级数移调 |

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

## 双核

AC791N 的第二个核负责爆之后的所有处理（降采样、箱体、crush、延迟、参考鼓、总线、tape stop）。输出因此晚一个块（5.8 ms）。开机时按住 REC：本次只用一个核（声音完全一样，只是没有那 5.8 ms）。第二个核出错会自动停掉、退回单核，FX 页右上角会显示原因。双核的启动和防护方式来自 Melodee、fm1-x0x 和 JIANT（GPL-3.0）。

## v0.2 不包含（v0.3）

B 组效果（DIVE / GATE / STACK / REV）、OTT、相位器、动作录制、多台 FM-1 之间同步音序 DNA。
