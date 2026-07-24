#pragma once

#include <Windows.h>

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "GoBoard.h"

namespace KurenaiGo
{
    // genmove の非同期結果
    struct KataGoMoveResult
    {
        bool Failed = false;   // 通信エラー等、致命的な失敗
        bool IsPass = false;
        bool IsResign = false;
        int Row = -1;
        int Col = -1;
    };

    // kata-analyzeの候補手1件分(1つの解析報告に複数含まれる)
    struct AnalysisMoveInfo
    {
        int Row = -1;   // pass等、盤上の頂点でない場合は-1
        int Col = -1;
        int Order = 0;      // 0が最有力候補
        float Winrate = 0.5f;  // 解析対象色(ColorToMove)から見た勝率(0〜1)
        int Visits = 0;
    };

    // kata-analyzeの解析結果(解析対象色=ColorToMoveの視点)
    struct KataGoAnalysisResult
    {
        bool Failed = false;    // 通信エラー等、致命的な失敗(対局自体は継続する)
        Stone ColorToMove = Stone::Empty;
        float WinrateForColorToMove = 0.5f;
        float ScoreLeadForColorToMove = 0.0f; // ColorToMoveから見た目差(コミ込み、正なら優勢)
        // 19路なら19*19=361要素。盤面左上(row=Size-1, col=0)がindex0で、右→下へ行優先で並ぶ
        // (ownershipIndex = (Size-1-row)*Size+col)。ColorToMoveから見た地の所有率(-1〜+1)。
        // 空なら未取得
        std::vector<float> Ownership;
        std::vector<AnalysisMoveInfo> TopMoves; // Order昇順とは限らないため、利用側でソートする
    };

    // KataGo(katago.exe)を子プロセスとして起動し、GTP(Go Text Protocol)で対局するクライアント。
    // 標準入出力を匿名パイプでリダイレクトしてテキストのコマンド/応答をやり取りする、Win32での
    // 「リダイレクトされた入出力を持つ子プロセスの作成」の標準的な実装。起動処理とgenmoveは
    // OpenCLの初回チューニングや思考時間で数秒〜数十秒かかることがあるため別スレッドで実行し、
    // 描画ループ(PumpEvents/BeginFrame/EndFrame)を止めないようにする
    class KataGoClient
    {
    public:
        KataGoClient() = default;
        ~KataGoClient();

        KataGoClient(const KataGoClient&) = delete;
        KataGoClient& operator=(const KataGoClient&) = delete;

        // katago.exeを起動し、boardsize/clear_board/komi/time_settingsを送って対局開始状態にする。
        // 別スレッドで実行され、完了は IsStartupComplete() でポーリングする。失敗時は
        // LastError() にメッセージが入る(致命的なのでMain側で表示して終了する想定)
        void StartAsync(const std::filesystem::path& exePath, const std::filesystem::path& modelPath,
            const std::filesystem::path& configPath, const std::filesystem::path& stderrLogPath, int boardSize);
        bool IsStartupComplete() const { return m_StartupComplete.load(); }
        bool StartupFailed() const { return m_StartupFailed.load(); }

        // 人間の着手をKataGo側の盤面にも反映する(同期呼び出し。play応答は一瞬で返るため)
        void PlayMove(Stone color, int row, int col);
        void PlayPass(Stone color);

        // genmoveを別スレッドで要求する。結果はTryGetGenMoveResultでポーリングする
        void RequestGenMove(Stone color);
        bool TryGetGenMoveResult(KataGoMoveResult& outResult);

        // final_scoreを別スレッドで要求する。結果はTryGetFinalScoreでポーリングする
        void RequestFinalScore();
        bool TryGetFinalScore(std::string& outResult);

        // kata-analyzeによる局面解析を別スレッドで要求する。結果はTryGetAnalysisResultで
        // ポーリングする。対局進行には必須ではない補助情報のため、失敗してもRequestGenMove等の
        // ように対局を止めることはなく、KataGoAnalysisResult::Failedで呼び出し側に通知するのみ
        void RequestAnalysis(Stone colorToMove);
        bool TryGetAnalysisResult(KataGoAnalysisResult& outResult);

        const std::string& LastError() const { return m_LastError; }

    private:
        void LaunchProcess(const std::filesystem::path& exePath, const std::filesystem::path& modelPath,
            const std::filesystem::path& configPath, const std::filesystem::path& stderrLogPath);
        void SendCommand(const std::string& command);
        std::string ReadResponseLine();
        // コマンド送信+応答受信を1つの操作として行い、"= "を取り除いた本文を返す。
        // GTPがエラー("?")を返した場合は例外を投げる
        std::string Exchange(const std::string& command);

        // kata-analyzeを送信し、一定時間/visits数だけストリーミング報告を受け取ってから停止し、
        // 最後に受け取った報告をパースして返す(詳細はKataGoClient.cpp冒頭のコメント参照)
        KataGoAnalysisResult ExchangeAnalyze(Stone colorToMove);
        // kata-analyzeの1報告行("info move ... info move ... ownership ...")をパースする
        static void ParseAnalysisLine(const std::string& line, KataGoAnalysisResult& outResult);

        static std::string ToVertex(int row, int col);
        // GTPの頂点表記("Q16"等)を(row, col)へ変換する。pass/resignは呼び出し禁止
        static void ParseNormalVertex(const std::string& vertex, int& outRow, int& outCol);
        static void ParseVertex(const std::string& vertex, KataGoMoveResult& outResult);
        static char ToGtpColorChar(Stone color);

        HANDLE m_ProcessHandle = nullptr;
        HANDLE m_ChildStdinWrite = nullptr;
        HANDLE m_ChildStdoutRead = nullptr;

        // GTPのコマンド送信・応答受信は一度に1つのやり取りしか行わない前提のため、
        // パイプの読み書きはこのmutexで直列化する
        std::mutex m_IoMutex;

        std::thread m_WorkerThread;

        std::atomic<bool> m_StartupComplete { false };
        std::atomic<bool> m_StartupFailed { false };

        std::atomic<bool> m_GenMoveReady { false };
        KataGoMoveResult m_GenMoveResult;

        std::atomic<bool> m_FinalScoreReady { false };
        std::string m_FinalScoreResult;

        std::atomic<bool> m_AnalysisReady { false };
        KataGoAnalysisResult m_AnalysisResult;

        std::string m_LastError;
    };
}
