#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "GoBoard.h"
#include "Sgf.h"

namespace KurenaiGo
{
    // 詰碁(死活)の問題1問。Assets\Tsumego\ の*.sgf 1ファイルが1問に対応する。
    // 本実装が扱うのは「黒先で、白の一団を取れるか」の形のみ(17章参照)。
    // 正解の手順そのものはファイルに書かない: 正誤はKataGoの読み(白石が実際に盤上から
    // 消えたか/KataGoのownershipが白を生きと判定したか)だけで決める
    struct TsumegoProblem
    {
        std::string FileName;   // 並び順・エラーログ用(拡張子込みのファイル名)
        int BoardSize = 9;
        std::string Title;      // SGFのGN(UTF-8)。無ければファイル名
        std::string Description; // SGFのC(UTF-8)。無ければ空
        std::vector<SgfSetupStone> SetupStones; // AB/AWによる初期配置
        // 生死を判定する対象の白石の座標。SGFのSQ(四角マーク)で指定されていればそれを使い、
        // 指定が無ければAWの白石すべてを対象にする。問題図は盤全体が互角になるよう白の
        // 生きている石(壁など)も置くため、対象をSQで絞れることが必要になる
        std::vector<std::pair<int, int>> TargetStones;
    };

    // Assets\Tsumego\以下の*.sgfをファイル名順に読み込む。
    // 「PL[B](黒先)であり、AWの白石が1つ以上ある」問題のみを採用し、それ以外・読み込みに
    // 失敗したファイルはoutSkipReasonsへ理由を積んで読み飛ばす(1問の不備で機能全体を止めない)。
    // ディレクトリ自体が無い場合は空のリストを返す(例外は投げない)
    std::vector<TsumegoProblem> LoadTsumegoProblems(const std::filesystem::path& directory,
        std::vector<std::string>& outSkipReasons);

    // 初期配置をKataGoへGTPのplayで流し込む順序を決める。playは1手ずつの着手として扱われるため、
    // 置いた瞬間に呼吸点が0になる石があると相手の石を取ってしまい問題図が壊れる。
    // 「置いた時点で自分の連も隣接する相手の連も呼吸点が1つ以上ある」石を選び続ける貪欲法で
    // 並べ替える。どう並べても成立しない場合はfalseを返す(その問題は使わない)
    bool TryOrderSetupStonesForGtp(const std::vector<SgfSetupStone>& stones, int boardSize,
        std::vector<SgfSetupStone>& outOrdered);

    // 対象の石(problem.TargetStones)がすべて「白でなくなった」かどうか。
    // 取られて空点になった場合も、その後どちらかの石が置かれた場合も「白でなくなった」と扱う
    bool AreTargetStonesCaptured(const GoBoard& board, const std::vector<std::pair<int, int>>& targetStones);
}
