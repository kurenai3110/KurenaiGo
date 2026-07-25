#pragma once

#include <filesystem>
#include <set>
#include <string>

namespace KurenaiGo
{
    // 対局の局面。総手数に対する割合で単純に3等分したものであり、囲碁理論上の
    // 厳密な布石・中盤・ヨセの境界を判定しているわけではない
    enum class GamePhase
    {
        Opening,
        Middle,
        Endgame,
    };

    // 着手前後の勝率変化(9.5節、着手の言語化と同じ基準)による4段階の分類
    enum class MoveQuality
    {
        Best,       // 最善手級
        SlightLoss, // やや損な手
        Loose,      // 緩着
        Blunder,    // 悪手
    };

    // GamePhase/MoveQualityの組み合わせごとの手数
    struct MistakeStatsData
    {
        // Counts[phase][quality]。添字はGamePhase/MoveQualityの整数値
        int Counts[3][4] = {};

        // "<SGFファイル名>:<手数>" 形式の集計済みキー。同じ手を重複して数えないための保険
        std::set<std::string> SeenKeys;
    };

    // moveIndex(1始まり)/totalMovesの割合で局面を3等分する(1/3ずつ)
    GamePhase DeterminePhase(int moveIndex, int totalMoves);

    // mistake_stats.txtの全行を読み、パースできた行から集計・重複排除キーを復元する。
    // ファイル不在・パース失敗行は無視する(例外を投げない)
    MistakeStatsData LoadMistakeStats(const std::filesystem::path& path);

    // 1手ぶんの分類結果を1行追記する。失敗時はstd::runtime_errorを投げる(呼び出し側でcatch)
    void AppendMistakeEntry(const std::filesystem::path& path, const std::string& sgfFileName,
        int moveIndex, GamePhase phase, MoveQuality quality);
}
