# KurenaiGo
囲碁アプリ

自作の描画エンジン [KurenaiEngine](https://github.com/kurenai3110/KurenaiEngine)(DirectX 11)で
盤・石を描画し、囲碁AI [KataGo](https://github.com/lightvector/katago) と
GTP(Go Text Protocol)経由で通信して対局する囲碁(Go)アプリです。
KurenaiEngineは本リポジトリにGit submoduleとして組み込んでいます。

## 現在の実装状況

- [x] 19路盤の表示(木目の盤面・格子線・星)
- [x] 石を置く操作(マウスクリックで着手)
- [x] 対局ルール(合法手判定・アゲハマ・シンプルコウ)
- [x] KataGoとの対局(人間=黒、KataGo=白。GTP経由でgenmove/final_score等をやり取り)
- [x] 勝率表示・地合い可視化・着手ヒント(KataGoの`kata-analyze`による局面解析)
- [x] 対局終了時の棋譜(SGF)自動保存
- [x] 棋譜の読み込み・再生(直前の対局のみ、解析なし)
- [x] 着手以外の操作(パス・投了・地合い表示・着手ヒント・棋譜再生・終了)のボタンUI化
- [ ] 色の選択・ハンディキャップ・対局のリスタート

## 遊び方

`KurenaiGo.exe` を起動すると自動的にKataGoが子プロセスとして起動し(初回のみOpenCLの
自動チューニングで数十秒かかることがあります)、準備ができ次第、黒番(人間)から対局が始まります。

着手(石を置く)以外の操作は、盤の下に常時表示されるボタン行から行えます。表示されるボタンは
対局の進行状態に応じて変わります(例: 棋譜再生ボタンは対局終了後にのみ表示)。キーボードでも
同じ操作が可能です。

| 操作 | ボタン | キー |
| --- | --- | --- |
| 着手 | (盤の交点をマウスでクリック) | - |
| パス | `パス` (対局中のみ表示) | `P` キー |
| 投了 | `投了` (対局中のみ表示) | `R` キー |
| 地合い表示の切替 | `地合い表示` | `T` キー |
| 着手ヒント表示の切替 | `着手ヒント` | `H` キー |
| 棋譜再生を開始(対局終了後) | `棋譜再生` (対局終了後のみ表示) | `V` キー |
| 棋譜再生中に1手進める・戻す | `次の手` / `前の手` (再生中のみ表示) | `→` / `←` キー |
| 終了 | `終了` | `Esc` |

対局が両者パスで終了すると、KataGoの `final_score` による結果(例: `B+3.5`)をダイアログで表示します。
KataGoが投了した場合、あなたの投了操作をした場合も同様にダイアログで通知されます。

対局が終了すると、棋譜が `Games\` フォルダ(実行ファイルと同じ場所)へSGF形式で自動保存されます
(ファイル名は `game_YYYYMMDD_HHMMSS.sgf`)。保存に失敗しても対局結果の表示は妨げず、
`error.log` に記録するのみです。

対局終了後(結果ダイアログを閉じた後)に `V` キーを押すと、直前に保存した棋譜を読み込んで
再生できます。`→`/`←` キーで1手ずつ進める・戻すと、その時点の盤面が再現されます
(現時点では解析やアゲハマ以外の情報は表示されません)。

盤の下には現在の手番状態・アゲハマ(お互いが取った石の数)を表示するHUDが常時表示されます。

黒番(人間)の手番になるたびに、KataGoの`kata-analyze`による局面解析を使って盤の上に勝率バー
(黒/白の優勢を示す横棒)を表示します。あわせて`T`キーで地合い(地の所有率)の色つきオーバーレイ、
`H`キーで着手ヒント(候補手の上位3件を金・銀・銅で色分け)を表示できます。

## リポジトリ構成

```
KurenaiGo.sln              ルートソリューション(KurenaiGo本体 + KurenaiEngineをプロジェクト参照)
KurenaiGo/
  KurenaiGo.vcxproj        本体(Application)
  Source/
    Main.cpp                エントリポイント。描画・マウス入力・対局の進行状態を管理する
    GoBoard.h/.cpp           囲碁のルール(合法手判定・捕獲・シンプルコウ)
    KataGoClient.h/.cpp      KataGoを子プロセスとして起動しGTPで通信するクライアント
    PathUtil.h/.cpp          exeと同じフォルダにあるファイル/フォルダを解決するヘルパー
    Sgf.h/.cpp               棋譜(SGF)の書き出し・読み込み
  Assets/
    Sounds/                  着手音・対局終了音(Tools/generate_sound_effects.ps1で生成した合成音)
  Tools/
    generate_sound_effects.ps1  効果音(WAV)の再生成スクリプト
KurenaiEngine/              描画エンジン(Git submodule)
Build/
  Bin/x64/Debug/             ビルド成果物の出力先(Git管理対象外)
    KurenaiGo.exe            本体
    KurenaiEngine.dll        ビルド後処理でコピーされる
    Shaders/                 ビルド後処理でコピーされるシェーダ一式(KurenaiEngine.dllが実行時に参照)
    Assets/                  ビルド後処理でコピーされる効果音
    KataGo/                  KataGo本体・ニューラルネット(手動配置。下記セットアップ参照)
    Games/                   対局終了時に自動保存される棋譜(SGF、実行時に生成)
docs/
  KurenaiGo.html            本アプリのドキュメント(盤のレイアウト・座標系・対局アーキテクチャ)
```

## 必要環境

- Windows 10 / 11
- Visual Studio 2022(「C++によるデスクトップ開発」ワークロード、Windows 10 SDK)
- CMake(KurenaiEngineが使うassimpのビルド用。Visual Studio付属のもので可)
- KataGoの動作にはGPU(OpenCL対応。NVIDIA/AMD/Intel問わず)を推奨します

## セットアップ手順

### 1. クローン(submodule込み)

```
git clone --recurse-submodules https://github.com/kurenai3110/KurenaiGo.git
```

既にクローン済みの場合:

```
git submodule update --init --recursive
```

### 2. KurenaiEngineの依存ライブラリをビルド

KurenaiEngineは内部でassimp・DirectXTexを使用するため、事前にビルドが必要です。手順の詳細は
[KurenaiEngine/README.md](KurenaiEngine/README.md)(セットアップ手順)を参照してください。

### 3. ビルド

```
MSBuild KurenaiGo.sln /p:Configuration=Debug /p:Platform=x64
```

`KurenaiGo.vcxproj` は `KurenaiEngine.vcxproj` をプロジェクト参照しているため、上記コマンド一回で
KurenaiEngine.dllも含めてビルドされます。`KurenaiGo.exe` は本リポジトリ直下の
`Build\Bin\x64\Debug\` に出力され、ビルド後処理(PostBuildEvent)で `KurenaiEngine.dll`(と`.pdb`)・
`Shaders\`(KurenaiEngine.dllが実行時に参照するシェーダ一式)・`Assets\`(効果音)が同じフォルダへ
コピーされ、空の `KataGo\` フォルダも作成されます(次の手順で中身を配置する)。実行に必要な
ものはすべて `Build\Bin\x64\Debug\` フォルダ1つに集約されるため、このフォルダごとコピーすれば
他の場所でも実行できます。

### 4. KataGoの配置

KataGo本体・ニューラルネットは容量が大きいためGitに含めていません。ビルドで作成された
`Build\Bin\x64\Debug\KataGo\` フォルダに、以下を配置してください(`Build\` は`.gitignore`で
除外済み)。

1. [KataGo Releases](https://github.com/lightvector/KataGo/releases) から
   Windows向けOpenCL版 (`katago-v<バージョン>-opencl-windows-x64.zip`) をダウンロードし、
   中身(`katago.exe`、各種DLL、`default_gtp.cfg`等)を `Build\Bin\x64\Debug\KataGo\` 直下に展開する。
2. `default_gtp.cfg` を `gtp.cfg` にリネームする。
3. [katagotraining.org](https://katagotraining.org/networks/) からニューラルネット
   (`.bin.gz`)をダウンロードし、`model.bin.gz` として同じフォルダに配置する。
   軽量で実績のある `kata1-b18c384nbt` 系列を推奨します
   (例: `kata1-b18c384nbt-s9996604416-d4316597426.bin.gz`)。

配置後、`Build\Bin\x64\Debug\KataGo\` は以下のようになります。

```
Build\Bin\x64\Debug\KataGo\
  katago.exe
  gtp.cfg          (= default_gtp.cfgをリネームしたもの)
  model.bin.gz
  (その他、zipに同梱のDLL・cfgファイル一式)
```

OpenCLバックエンドは初回起動時にGPU向けの自動チューニングを行い(多くの環境で数十秒程度)、
結果は `KataGo\KataGoData\` 以下にキャッシュされるため、2回目以降の起動は高速になります。

## 実行

```
Build\Bin\x64\Debug\KurenaiGo.exe
```

## ドキュメント

本アプリの盤の描画方式・座標系・対局アーキテクチャ(GTP連携・非同期処理)については
[docs/KurenaiGo.html](docs/KurenaiGo.html) を参照してください。
KurenaiEngine自体のAPIリファレンスは
[KurenaiEngine/docs/KurenaiEngine.html](KurenaiEngine/docs/KurenaiEngine.html) を参照してください。
