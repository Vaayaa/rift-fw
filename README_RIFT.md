# RIFT 裂 · FM-1 贝斯固件 v0.1（核心版）

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
python3 tools/fm1_install.py build/rift.fwsc   # 写入前会先询问确认
```

刷回其他固件或官方 V15：打开 thefirmwarehub.com（Chrome 或 Edge）。
刷坏了：开机时按住 OCT− 进入 USB 救援模式，再重新安装。

## 操作

| 控件 | 作用 |
|---|---|
| PLAY | 播放 / 停止（同时发 MIDI Start/Stop 和时钟） |
| K1–K4 | PLAY 页：CUT / RES / ENV / 爆 |
| FX · LFO · ENV · ARP 按住 | 升 RISE · 滚 ROLL · 哇 WOW · 停 STOP |
| 两键同按 | FX+LFO 筑 BUILD · LFO+ENV 碎 SHRED · FX+ARP 空 VOID（ENV+ARP 冻 FREEZE 留到 v0.2） |
| 双击效果键 | 锁定效果，再按一次解锁 |
| 按住效果键时 | K1–K4 调这个效果的参数；OCT+ = 留 KEEP，OCT− = 弃 ABORT |
| HOME | 单击回主屏 · 双击 RETURN（宏回到预设值）· 长按 0.5 秒 PANIC |
| SEQ | 单击进 SEQ 页；再按一次在 GEN / PERF 之间切换 |
| SEQ 按住 + PLAY | HARD 变异（下一小节生效） |
| SEQ 按住 + REC / HOME / OCT− / FX / EDIT | SOFT 微变 / UNDO / DROP / FILL（按住期间）/ A·B 互换 |
| SEQ 按住 + F3 G3 A3 B3 C4 D4 E4 F4 | 单独重生成 RHY ACC SLD PIT OCT MOD CHN TRN |
| SEQ 按住 + G4–G5 白键 | 切换音序槽 1–8 |
| REC 按住 + 弹键（播放中） | TAP-PITCH：只改旋律，节奏不动 |
| EDIT | VOICE / FX 页（FX 页显示 CPU 占用） |
| GLO | 参考鼓开 / 关 |
| SAVE | 存储音色和 8 个音序槽的 DNA |
| SELECT | PLAY 页调 BPM · SEQ 页选音阶 · FX 页选延迟时值 |
| ALGO / PRESET | 切音色预设 / 切音序槽（下一小节生效） |
| 琴键 | 停止时弹贝斯（按住越久越亮；同键快速双击 = 重音）；播放中按键 = 按音阶级数移调 |

## MIDI 输出（用于 TouchDesigner）

- ch1：音符。
- CC20 cutoff · CC21 爆 · CC22 包络 · CC23 重音电容 · CC26 效果进度 · CC27 裂纹强度。
- ch16：音符 36–43 为效果开关，48 = DROP，49 = 落地，51 = 小节，52 = 拍，55 = PANIC。
- 时钟：24 ppqn，并发送 Start / Stop。

## v0.1 不包含

REESE / RUMBLE / GROWL / SCREAM 引擎、B 组效果、FREEZE、动作录制、音色骰子、OTT、相位器。这些等实测完 CPU 余量后再逐个加回来。
