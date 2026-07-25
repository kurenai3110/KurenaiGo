#pragma once

#include <filesystem>
#include <string>

namespace KurenaiGo
{
    // 対局終了時点でのレーティング(棋力の数値化)。rating_history.txtの全行から復元する
    struct RatingData
    {
        double Rating = 1500.0;
        int GamesPlayed = 0;
    };

    // ユーザーの初期レーティング。標準的なElo初期値の慣習を流用した値であり、
    // 実世界の段級位・棋力を表す値ではない。初期レーティング決定(プレースメント)モードの
    // Elo逆算(InvertEloForRating)の基準アンカー値としても使う
    constexpr double kInitialRating = 1500.0;

    // Eloレーティングで広く使われるK係数
    constexpr double kEloK = 32.0;

    // rating_history.txtの全行を読み、パースできた行数をGamesPlayed、最後の行の
    // レーティング値をRatingとして復元する。ファイルが無い・1行もパースできない場合は
    // 既定値(kInitialRating、対局数0)を返す(例外を投げない)
    RatingData LoadRating(const std::filesystem::path& path);

    // レート戦の対局結果を1行追記する。失敗時はstd::runtime_errorを投げる(呼び出し側でcatch)
    void AppendRatingEntry(const std::filesystem::path& path,
        const std::string& timestamp, double ratingAfter, const std::string& result);

    // 対局結果文字列("W+R"/"B+R"/"B+3.5"等)から、黒番(人間)から見た勝敗を
    // 0.0(負け)/0.5(持碁)/1.0(勝ち)として取り出す。解釈できない文字列の場合はfalseを返す
    bool TryParseBlackWinFraction(const std::string& result, double& outScore);

    // 標準的なElo式によるレーティング変化量を計算する
    double ComputeEloDelta(double userRating, double opponentRating, double actualScore, double kFactor);

    // Eloの期待勝率式 expected = 1/(1+10^((opponentRating-rating)/400)) をratingについて
    // 解いた式。「この期待勝率(0〜1)をちょうど出す側のレーティングはいくつか」を逆算する。
    // 初期レーティング決定(プレースメント)モードで、対局中に観測した勝率から現在の実力を
    // 推定するために使う。expectedScoreは0/1に近すぎると発散するため[0.01, 0.99]にクランプする
    double InvertEloForRating(double expectedScore, double opponentRating);
}
