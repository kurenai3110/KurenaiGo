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
- [x] 棋譜の読み込み・再生(直前の対局のみ)
- [x] 再生中の解析表示(勝率・目差の数値表示)・損失グラフ
- [x] 着手の言語化(再生中、勝率変化の大きさによる悪手判定)
- [x] 着手以外の操作(パス・投了・地合い表示・着手ヒント・棋譜再生・終了)のボタンUI化
- [x] 棋力の数値化(レーティング、レート戦/カジュアルの対局モード分け)
- [x] 対局のリスタート(アプリを再起動せず何度でも新規対局を開始できる)
- [x] 棋力に応じたAIの強さ設定(レート戦は自分のレーティングと互角、カジュアルは段階選択)
- [ ] 苦手分野の解析(複数対局を跨いだ集計)
- [ ] 色の選択・ハンディキャップ

## 遊び方

`KurenaiGo.exe` を起動すると、まず盤下のボタンで対局モードを選びます。「レート戦」を選ぶと、
今のあなたのレーティングと互角になるようKataGoの強さを自動調整して対局します(勝敗がレーティングに
反映されます)。「カジュアル」を選ぶと、続けてAIの強さを5段階(とても弱め/弱め/おすすめ/強め/
とても強め、「おすすめ」は今のレーティング相当)から自分で選べます(レーティングには影響しません)。
強さが決まった時点でKataGoを起動するため(初回のみOpenCLの自動チューニングで数十秒かかることが
あります)、対局開始までにKataGo起動待ちの時間が入ります。準備ができ次第、黒番(人間)から対局が
始まります。

着手(石を置く)以外の操作は、盤の下に常時表示されるボタン行から行えます。表示されるボタンは
対局の進行状態に応じて変わります(例: 棋譜再生ボタンは対局終了後にのみ表示)。キーボードでも
同じ操作が可能です。

| 操作 | ボタン | キー |
| --- | --- | --- |
| 対局モードの選択(起動直後・新規対局時) | `レート戦` / `カジュアル` | - |
| カジュアルの強さ選択(カジュアル選択時のみ) | `とても弱め`/`弱め`/`おすすめ`/`強め`/`とても強め` | - |
| 着手 | (盤の交点をマウスでクリック) | - |
| パス | `パス` (対局中のみ表示) | `P` キー |
| 投了 | `投了` (対局中のみ表示) | `R` キー |
| 地合い表示の切替 | `地合い表示` | `T` キー |
| 着手ヒント表示の切替 | `着手ヒント` | `H` キー |
| 棋譜再生を開始(対局終了後) | `棋譜再生` (対局終了後のみ表示) | `V` キー |
| 棋譜再生中に1手進める・戻す | `次の手` / `前の手` (再生中のみ表示) | `→` / `←` キー |
| 新規対局を開始(対局終了後・棋譜再生中) | `新規対局` (対局終了後・棋譜再生中のみ表示) | `N` キー |
| 終了 | `終了` | `Esc` |

対局が両者パスで終了すると、KataGoの `final_score` による結果(例: `B+3.5`)をダイアログで表示します。
KataGoが投了した場合、あなたの投了操作をした場合も同様にダイアログで通知されます。

対局が終了すると、棋譜が `Games\` フォルダ(実行ファイルと同じ場所)へSGF形式で自動保存されます
(ファイル名は `game_YYYYMMDD_HHMMSS.sgf`)。保存に失敗しても対局結果の表示は妨げず、
`error.log` に記録するのみです。

対局終了後(結果ダイアログを閉じた後)に `V` キーを押すと、直前に保存した棋譜を読み込んで
再生できます。`→`/`←` キーで1手ずつ進める・戻すと、その時点の盤面が再現されます。手を
進めるたびにその局面をKataGoが解析し、勝率バー・数値(勝率%・目差)を表示します(初めて
訪れる手数は解析に数百ms〜数秒かかることがありますが、一度解析した手数は結果を覚えているため
再訪時は即座に表示されます)。盤の上部には対局を通した勝率の推移(損失グラフ)が表示され、
どの手で形勢が動いたかを一目で確認できます。損失グラフの上には、その手が「最善手級」
「やや損な手」「緩着」「悪手」のどれに当たるか、勝率の変化(例: 62.3%→54.1%)とともに
日本語の文章で表示されます(最善手でなかった場合は、KataGoが薦める最善手の座標も添えます)。

対局終了後、または棋譜再生中はいつでも `新規対局` ボタン(`N` キー)で新しい対局を始められます。
アプリを再起動する必要はなく、何度でも打ち直せます。選ぶと対局モードの選択画面に戻り、盤面・
アゲハマ・着手履歴がすべてリセットされたうえで黒番(人間)から対局が始まります(新しい強さで
対局するため、選び直すたびにKataGoを起動し直します)。

盤の下には現在の手番状態・アゲハマ(お互いが取った石の数)・レーティング(棋力の数値化)・
現在の対局モード([レート戦]/[カジュアル])・AIの強さ(目安)を表示するHUDが常時表示されます。
レーティングは標準的なElo式で計算され、初期値は1500です。レート戦は常にあなたの現在の
レーティングと互角になるようAIの強さ(KataGoの`maxVisits`、探索の深さ)を自動調整するため、
勝つと上がり、負けると下がります(カジュアル対局はレーティングに影響しません)。このレーティングは
実世界の段級位を表す値ではなく、「その時点で自分と互角になるよう調整されたAIに対して、
自分がどれだけ勝ち越せているか」を数値化したものです。レート戦の結果は`rating_history.txt`
(実行ファイルと同じ場所)に1局ごと追記され、次回起動時にも引き継がれます。

レーティングからAIの強さ(`maxVisits`)を求める換算、および初期レーティング決定の収束判定基準
(サンプル数・しきい値等)は、いずれも事前に科学的な較正を行ったものではなく、妥当だと考えられる
調整可能な初期値です。

まだ一度もレート戦を対局したことが無い(対局回数0)状態でレート戦を始めると、「初期レーティング
決定」という特別な対局になります。この対局では、あなたの手番ごとの局面の勝率をもとに実力の
推定値を追跡し続け、推定が安定した時点で(1局の途中であっても)その値をレーティングとして
確定します。安定するまで複数局かかることもあります。HUDには通常のレーティング表示の代わりに
「レーティング測定中(推定: ...)」と表示されます。

黒番(人間)の手番になるたびに、KataGoの`kata-analyze`による局面解析を使って盤の上に勝率バー
(黒/白の優勢を示す横棒)と、勝率(%)・目差の数値を表示します。あわせて`T`キーで地合い
(地の所有率)の色つきオーバーレイ、`H`キーで着手ヒント(候補手の上位3件を金・銀・銅で色分け)を
表示できます。

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
    rating_history.txt       レート戦の対局結果ごとのレーティング推移(実行時に生成・追記)
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
