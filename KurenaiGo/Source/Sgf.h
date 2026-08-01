#pragma once

#include <filesystem>
#include <string>
#include <utility>
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

    // 初期配置の石1個ぶん(SGFのAB/AWプロパティ。詰碁(17章)の問題図で使う)。
    // 「交互に打った手順」ではないためSgfMoveとは別に扱う
    struct SgfSetupStone
    {
        Stone Color = Stone::Empty;
        int Row = -1; // KurenaiGoの内部座標(row=0が盤面下端)
        int Col = -1;
    };

    // 1局ぶんの記録(分岐の無い単一手順のみを扱う、SGFの最小限のサブセット)
    struct SgfGameRecord
    {
        int BoardSize = 19;
        float Komi = kKomi;
        std::string Result; // 例: "B+3.5"、"W+R"。空なら不明
        std::vector<SgfMove> Moves;

        // 以下は詰碁(17章)の問題図を記述するために読み取るプロパティ。
        // KurenaiGo自身が書き出す対局の棋譜では常に空/既定値のままになる
        std::vector<SgfSetupStone> SetupStones; // AB/AWによる初期配置(順序はSGFの記載順)
        Stone PlayerToMove = Stone::Empty;      // PL。記載が無ければEmpty
        std::string GameName;                   // GN(UTF-8)
        std::string Comment;                    // C(UTF-8)
        // SQ(四角マーク)で指定された交点。詰碁では「生死を判定する対象の石」を指す
        std::vector<std::pair<int, int>> MarkedPoints;
        // LV。詰碁の難易度(1が最もやさしい)。SGFの標準プロパティではなくKurenaiGo独自の
        // 拡張で、記載が無ければ0(難易度の指定なし)。他のSGFソフトは未知のプロパティとして
        // 読み飛ばすため、問題図ファイルの互換性は保たれる
        int Difficulty = 0;
    };

    // SgfGameRecordをSGF(Smart Game Format)のテキストへ書き出す
    std::string WriteSgf(const SgfGameRecord& record);

    // SGFのテキストを解析してSgfGameRecordへ変換する。分岐や本実装が対応していない構造を
    // 含む場合、あるいは構文が壊れている場合はstd::runtime_errorを投げる。
    // 複数の局が並んでいる場合は先頭の1局だけを返す
    SgfGameRecord ReadSgf(const std::string& sgfText);

    // SGFコレクション("(;...)(;...)"と局を並べた形式)を解析してすべての局を返す。
    // 詰碁の問題集(17章)は290問を1ファイルにまとめているためこちらで読む。
    // 局が1つだけのテキストでも同じように読める(要素数1の配列を返す)
    std::vector<SgfGameRecord> ReadSgfCollection(const std::string& sgfText);

    // SgfGameRecordをUTF-8のSGFファイルとして書き出す(失敗時はstd::runtime_error)
    void WriteSgfFile(const std::filesystem::path& path, const SgfGameRecord& record);

    // UTF-8のSGFファイルを読み込みSgfGameRecordへ変換する(失敗時はstd::runtime_error)
    SgfGameRecord ReadSgfFile(const std::filesystem::path& path);

    // UTF-8のSGFファイルを読み込み、含まれるすべての局を返す(失敗時はstd::runtime_error)
    std::vector<SgfGameRecord> ReadSgfCollectionFile(const std::filesystem::path& path);
}
