#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "GoBoard.h"
#include "KataGoClient.h" // kKomi定数(コミ値をKataGoとの通信・SGF書き出しで共有するため)

namespace KurenaiGo
{
    // 1手ぶんの記録
    struct SgfMove
    {
        Stone Color = Stone::Empty;
        bool IsPass = false;
        int Row = -1; // KurenaiGoの内部座標(row=0が盤面下端)。IsPassがtrueなら未使用
        int Col = -1;
    };

    // 1局ぶんの記録(分岐の無い単一手順のみを扱う、SGFの最小限のサブセット)
    struct SgfGameRecord
    {
        int BoardSize = 19;
        float Komi = kKomi;
        std::string Result; // 例: "B+3.5"、"W+R"。空なら不明
        std::vector<SgfMove> Moves;
    };

    // SgfGameRecordをSGF(Smart Game Format)のテキストへ書き出す
    std::string WriteSgf(const SgfGameRecord& record);

    // SGFのテキストを解析してSgfGameRecordへ変換する。分岐や本実装が対応していない構造を
    // 含む場合、あるいは構文が壊れている場合はstd::runtime_errorを投げる
    SgfGameRecord ReadSgf(const std::string& sgfText);

    // SgfGameRecordをUTF-8のSGFファイルとして書き出す(失敗時はstd::runtime_error)
    void WriteSgfFile(const std::filesystem::path& path, const SgfGameRecord& record);

    // UTF-8のSGFファイルを読み込みSgfGameRecordへ変換する(失敗時はstd::runtime_error)
    SgfGameRecord ReadSgfFile(const std::filesystem::path& path);
}
