# KurenaiGo
囲碁アプリ

自作の描画エンジン [KurenaiEngine](https://github.com/kurenai3110/KurenaiEngine)(DirectX 11)を使って
実装している囲碁(Go)アプリです。KurenaiEngineは本リポジトリにGit submoduleとして組み込んでいます。

## 現在の実装状況

- [x] 19路盤の表示(木目の盤面・格子線・星)
- [ ] 石を置く操作
- [ ] 対局ルール(着手判定・アゲハマ・地の計算など)

## リポジトリ構成

```
KurenaiGo.sln              ルートソリューション(KurenaiGo本体 + KurenaiEngineをプロジェクト参照)
KurenaiGo/
  KurenaiGo.vcxproj        本体(Application)
  Source/Main.cpp          エントリポイント。KurenaiEngine2Dを使って盤を描画する
KurenaiEngine/              描画エンジン(Git submodule)
docs/
  KurenaiGo.html            本アプリのドキュメント(盤のレイアウト・座標系など)
```

## 必要環境

- Windows 10 / 11
- Visual Studio 2022(「C++によるデスクトップ開発」ワークロード、Windows 10 SDK)
- CMake(KurenaiEngineが使うassimpのビルド用。Visual Studio付属のもので可)

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
KurenaiEngine.dllも含めてビルドされます。生成された `KurenaiGo.exe` は `KurenaiEngine.dll` と同じ
フォルダ(`KurenaiEngine\Build\Bin\x64\Debug\`)に出力されます(DLLは実行ファイルと同じフォルダに
ないと起動時に読み込めないため)。

## 実行

```
KurenaiEngine\Build\Bin\x64\Debug\KurenaiGo.exe
```

Escキーで終了します。

## ドキュメント

本アプリの盤の描画方式・座標系については [docs/KurenaiGo.html](docs/KurenaiGo.html) を参照してください。
KurenaiEngine自体のAPIリファレンスは
[KurenaiEngine/docs/KurenaiEngine.html](KurenaiEngine/docs/KurenaiEngine.html) を参照してください。
