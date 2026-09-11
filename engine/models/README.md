# 训练权重放这里

本目录**不进版本管理**（见 `.gitignore` 的 `engine/models/` 与 `*.bin`）——
权重是几十到几百 MB，而且可以重新生成，不该塞进仓库。

## 怎么生成

用 CLI 的 `train` 子命令：

```powershell
# mixed 布局（12 个 4-tuple，约 3MB）—— 目前性价比最好的一个
engine\build\ai2048-cli.exe train --net mixed --games 400000 --lr 0.0002 `
    --nstep 1 --decay-games 30000 --eval-every 50000 --eval-games 40 `
    --seed 2024 --out engine\models\mixed-400k.bin
```

`--decay-games` 是 ε 指数衰减到 `epsilon_end` 所需的局数。
**不要用 0**（那是旧的"按总局数线性衰减"行为，实测分数低 61%，
原因见 `docs/results/README.md`）。

## 怎么用

### 命令行

```powershell
ai2048-cli.exe play  --seed 42 --depth 6 --net-file engine\models\mixed-400k.bin
ai2048-cli.exe bench --seeds benchmarks\seeds-v1.txt --depth 6 `
    --net-file engine\models\mixed-400k.bin --tag net-d6
```

不传 `--net-file` 就是手写启发式。

### 前端

设环境变量后双击 `start-ai2048.bat`：

```powershell
set AI2048_NET=engine\models\mixed-400k.bin
start-ai2048.bat
```

或直接起服务端：

```powershell
ai2048-server.exe --port 8765 --depth 6 --net-file engine\models\mixed-400k.bin
```

## ⚠️ 现在它比手写启发式弱，别当默认用

实测（`seeds-v1.txt`，同种子）：

| 叶子评估 | 深度 | 平均分 |
|---|---|---|
| 手写启发式 | 4 | **42,792** |
| 学习权重 | 7 | 17,430 |

学习权重**快约 5.5 倍**（depth 4 时每局 0.38s 对 2.07s），但棋力差 2.5 倍以上。
所以它现在的价值是"便宜、可继续训练"，不是"更强"。

想看它变强的方向（以及为什么"加大网络"是错的方向），
见 `docs/results/README.md` 的 C2 一节。

## 加载失败会直接报错退出

不存在的路径、损坏的文件、布局与权重数不匹配 —— 都会让程序以退出码 1 结束，
**不会静默回退到手写启发式**。这是刻意的：两者分数差 2.5 倍，
静默降级会让人以为在测网络、其实在测启发式。
