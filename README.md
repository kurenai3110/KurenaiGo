# KurenaiGo
囲碁アプリ

自作の描画エンジン [KurenaiEngine](https://github.com/kurenai3110/KurenaiEngine)(DirectX 11)で
盤・石を描画し、囲碁AI [KataGo](https://github.com/lightvector/katago) と
GTP(Go Text Protocol)経由で通信して対局する囲碁(Go)アプリです。
KurenaiEngineは本リポジトリにGit submoduleとして組み込んでいます。

## 遊び方

`KurenaiGo.exe` を起動し、盤上に表示されるボタンで盤の大きさ(`9路`/`13路`/`19路`)・対局モード
(`レート戦`/`カジュアル`)を選ぶと、黒番(人間) vs KataGoの対局が始まります。操作方法・
勝率表示・棋譜の保存と再生・レーティング・AIの強さ設定・苦手分野の解析など各機能の詳細は
[docs/KurenaiGo.html](docs/KurenaiGo.html) を参照してください。

**注意**: KataGo公式の「Human SLモデル」(`b18c384nbt-humanv0.bin.gz`)が未配置の場合、
対局そのものを開始できません(下記セットアップ「5. Human SLモデルの配置」参照)。

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
    Rating.h/.cpp            棋力の数値化(レーティング)の計算・rating_history.txtの読み書き
    MistakeStats.h/.cpp      苦手分野の解析(局面ごとの悪手率)の集計・mistake_stats.txtの読み書き
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
    rating_history.txt       19路レート戦のレーティング推移(実行時に生成・追記)
    rating_history_13.txt    13路レート戦のレーティング推移(実行時に生成・追記)
    rating_history_9.txt     9路レート戦のレーティング推移(実行時に生成・追記)
    mistake_stats.txt        苦手分野の解析(局面ごとの悪手率)の集計元データ(実行時に生成・追記)
docs/
  KurenaiGo.html            本アプリのユーザー向けドキュメント(遊び方・機能・セットアップ)
  KurenaiGo_Developer.html  本アプリの実装ドキュメント(盤のレイアウト・座標系・対局アーキテクチャ)
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

### 5. Human SLモデルの配置(必須)

AIの強さをアマチュア向けの実際の段級位(20級〜9段)で再現するため、KataGo公式の
「Human SLモデル」を追加で配置する必要があります。**このファイルが無いと対局を開始できません**
(起動直後の画面に配置を促すメッセージが表示され、盤の大きさ・対局モードの選択には進めません)。

1. [KataGo Releases v1.15.0](https://github.com/lightvector/KataGo/releases/tag/v1.15.0) から
   `b18c384nbt-humanv0.bin.gz` をダウンロードし、`model.bin.gz` と同じ
   `Build\Bin\x64\Debug\KataGo\` フォルダにそのままのファイル名で配置する。

`-human-model`起動引数はKataGo v1.15.0以降で使えます(本リポジトリで動作確認している
v1.16.5では利用できます)。

## 実行

```
Build\Bin\x64\Debug\KurenaiGo.exe
```

## ドキュメント

本アプリの遊び方・機能の詳細については
[docs/KurenaiGo.html](docs/KurenaiGo.html) を参照してください。
盤の描画方式・座標系・対局アーキテクチャ(GTP連携・非同期処理)など実装者向けの内容は
[docs/KurenaiGo_Developer.html](docs/KurenaiGo_Developer.html) にまとめています。
KurenaiEngine自体のAPIリファレンスは
[KurenaiEngine/docs/KurenaiEngine.html](KurenaiEngine/docs/KurenaiEngine.html) を参照してください。
