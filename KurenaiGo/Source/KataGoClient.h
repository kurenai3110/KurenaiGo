#pragma once

#include <Windows.h>

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

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

        const std::string& LastError() const { return m_LastError; }

    private:
        void LaunchProcess(const std::filesystem::path& exePath, const std::filesystem::path& modelPath,
            const std::filesystem::path& configPath, const std::filesystem::path& stderrLogPath);
        void SendCommand(const std::string& command);
        std::string ReadResponseLine();
        // コマンド送信+応答受信を1つの操作として行い、"= "を取り除いた本文を返す。
        // GTPがエラー("?")を返した場合は例外を投げる
        std::string Exchange(const std::string& command);

        static std::string ToVertex(int row, int col);
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

        std::string m_LastError;
    };
}
