#pragma once

#include <filesystem>
#include <string>

namespace KurenaiGo
{
    // 自分自身(KurenaiGo.exe)と同じフォルダを基準に、そこに配置されているファイル/フォルダへの
    // 絶対パスを解決する(例: "Assets", "KataGo")。起動時のカレントディレクトリはエクスプローラ
    // 起動/VSデバッガ起動などで変わりうるため、それに依存せずexe自身の場所(GetModuleFileNameW)を
    // 基準にする。実行に必要なデータ(石のテクスチャ・KataGo本体)はすべてビルド後にexeと同じ
    // フォルダへ配置される(KurenaiGo.vcxprojのPostBuildEvent、およびREADMEのセットアップ手順を
    // 参照)ため、Build\Bin\<Platform>\<Config>\ フォルダごとコピーするだけで実行できる。
    std::filesystem::path ResolveAppDataPath(const std::wstring& relativePath);
}
