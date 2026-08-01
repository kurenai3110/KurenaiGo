#pragma once

#include <Windows.h>

#include <atomic>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "GoBoard.h"

namespace KurenaiGo
{
    // 対局のコミ。KataGoへの komi コマンドとSGF書き出し(KM プロパティ)の両方で使う唯一の値
    constexpr float kKomi = 6.5f;

    // 一括解析(RequestBatchAnalysis)へ渡す1手ぶんの情報。SgfMove(Sgf.h)と同じ内容だが、
    // Sgf.hがKataGoClient.hをincludeしているため循環参照を避けてこちらに定義する。
    // 呼び出し側でSgfMove/moveHistoryの内容を詰め替えて渡す
    struct KataGoPlayedMove
    {
        Stone Color = Stone::Empty;
        bool IsPass = false;
        int Row = -1; // IsPassがtrueなら未使用
        int Col = -1;
    };

    // kata-analyzeを打ち切る条件。いずれかを満たした時点で停止する
    struct AnalysisBudget
    {
        DWORD TargetMs = 600;   // この時間を過ぎたら(報告を1件でも受けていれば)停止する
        DWORD HardCapMs = 1500; // 報告の有無によらず必ず停止する上限
        int TargetVisits = 250; // 最有力候補(Order==0)のvisitsがこれに達したら停止する
    };

    // 対局中のライブ解析(勝率表示・地合い・着手ヒント)用の予算。従来の固定値をそのまま踏襲する
    constexpr AnalysisBudget kLiveAnalysisBudget { 600, 1500, 250 };
    // 一括解析(対局後の自動解析・棋譜再生前の解析)用の予算。ミス検出・勝率グラフに加え、
    // 棋譜再生では地合い可視化・着手ヒントにも同じ結果を使う(9.4節)ため、ライブ用と同じく
    // ownershipも取得する。1局面ごとの絶対精度までは要らないため、時間/visits数はライブ用より
    // 浅くして所要時間を短縮する
    constexpr AnalysisBudget kBatchAnalysisBudget { 300, 800, 150 };

    // genmove の非同期結果
    struct KataGoMoveResult
    {
        bool Failed = false;   // 通信エラー等、致命的な失敗
        bool IsPass = false;
        bool IsResign = false;
        int Row = -1;
        int Col = -1;
    };

    // 読み筋(PV: Principal Variation)の1手ぶんの座標。KurenaiGoの内部座標(row=0が盤面下端)
    struct AnalysisPvPoint
    {
        int Row = -1;
        int Col = -1;
    };

    // 1つの候補手について保持する読み筋の最大手数。KataGoは数十手先まで読み筋を返すことが
    // あるが、盤上に重ねて表示できる手数には限りがあり、一括解析(RequestBatchAnalysis)は
    // 1局ぶんの全局面ぶんをキャッシュに抱えるため、ここで上限を切っておく
    constexpr int kMaxPvLength = 10;

    // kata-analyzeの候補手1件分(1つの解析報告に複数含まれる)
    struct AnalysisMoveInfo
    {
        int Row = -1;   // pass等、盤上の頂点でない場合は-1
        int Col = -1;
        int Order = 0;      // 0が最有力候補
        float Winrate = 0.5f;  // 解析対象色(ColorToMove)から見た勝率(0〜1)
        int Visits = 0;
        // この候補手を選んだ場合の読み筋(先頭がこの候補手自身。以降は交互に相手・自分の手)。
        // 最大kMaxPvLength手まで。pass・解釈できない頂点表記が現れた時点で打ち切るため、
        // 「先頭から連続する盤上の着手だけ」が入る
        std::vector<AnalysisPvPoint> Pv;
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

    // 一括解析(RequestBatchAnalysis)で1局面ぶんの解析が完了するたびにキューへ積まれる結果
    struct BatchAnalysisItem
    {
        int MoveIndex = 0; // 0手目〜総手数。moves[0..MoveIndex)を再生した局面の解析結果
        KataGoAnalysisResult Result;
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
        // LastError() にメッセージが入る(致命的なのでMain側で表示して終了する想定)。
        // maxVisitsはgtp.cfgのmaxVisitsを-override-configでこの値に上書きする
        // (実機検証済み: genmoveの探索がこの値でおおむね頭打ちになることを確認済み。
        // kata-analyzeの表示用解析はこの値に縛られない場合がある、docs/KurenaiGo_Developer.html参照)。
        // humanSLProfileが非空の場合(例: "rank_15k")、humanModelPathを"-human-model"として
        // 追加起動し、Human SLモデル(11.6節)による人間らしい打ち筋を再現する。空文字列の場合は
        // humanModelPathを無視し、maxVisitsのみによる従来方式で起動する(Human SLモデル未配置時の
        // フォールバック)。
        // すでに起動中のプロセスがある状態で呼んだ場合は、それを終了させてから作り直す
        // (対局ごとに強さを変えるため、同一インスタンスを再起動できるようにしている)
        void StartAsync(const std::filesystem::path& exePath, const std::filesystem::path& modelPath,
            const std::filesystem::path& humanModelPath, const std::string& humanSLProfile,
            const std::filesystem::path& configPath, const std::filesystem::path& stderrLogPath,
            int boardSize, int maxVisits);
        bool IsStartupComplete() const { return m_StartupComplete.load(); }
        bool StartupFailed() const { return m_StartupFailed.load(); }

        // 人間の着手をKataGo側の盤面にも反映する(同期呼び出し。play応答は一瞬で返るため)
        void PlayMove(Stone color, int row, int col);
        void PlayPass(Stone color);

        // 直前の1手をKataGo側の盤面から取り消す(GTPのundo。同期呼び出し)。
        // 「待った」(16章)で、人間の着手とそれに対するAIの応手の2手を戻すために2回呼ぶ。
        // 解析(kata-analyze)の実行中に呼ぶとm_IoMutexの解放待ちでブロックするが、
        // PlayMoveと同じ性質のため呼び出し側の扱いは変わらない。
        // 戻せる手が無い場合はGTPがエラーを返すため、呼び出し側は手数を確認してから呼ぶこと
        void Undo();

        // KataGo側の盤面を空盤面へ戻す(同期呼び出し。clear_board応答は一瞬で返るため)。
        // RequestBatchAnalysisは順方向専用で内部でclear_boardを1回だけ送るため、これは
        // 現状呼ばれていないが、盤面を明示的に空へ戻したい場合のための汎用APIとして残している
        void ResetBoard();

        // genmoveを別スレッドで要求する。結果はTryGetGenMoveResultでポーリングする
        void RequestGenMove(Stone color);
        bool TryGetGenMoveResult(KataGoMoveResult& outResult);

        // final_scoreを別スレッドで要求する。結果はTryGetFinalScoreでポーリングする
        void RequestFinalScore();
        bool TryGetFinalScore(std::string& outResult);

        // final_status_list deadを別スレッドで要求する。結果はTryGetDeadStonesでポーリングする。
        // KataGoが「この局面で死んでいる」と判断した石の座標が返る。詰碁(17章)の正誤判定に使う
        // (ownershipのしきい値と違い、死活の判定がそのまま離散の一覧で得られる)。
        // 通信に失敗した場合はoutFailedがtrueになり、outStonesは空になる
        void RequestDeadStones();
        bool TryGetDeadStones(std::vector<std::pair<int, int>>& outStones, bool& outFailed);

        // kata-analyzeによる局面解析を別スレッドで要求する。結果はTryGetAnalysisResultで
        // ポーリングする。対局進行には必須ではない補助情報のため、失敗してもRequestGenMove等の
        // ように対局を止めることはなく、KataGoAnalysisResult::Failedで呼び出し側に通知するのみ。
        // budgetは対局中のライブ解析を想定した既定値(kLiveAnalysisBudget)
        // allowedMovesを渡すと、候補手をその交点だけに絞って解析させる
        // (kata-analyzeの allow オプション)。詰碁の複数手詰め(17.6節)で、白に
        // 「問題の区域の中で最善の応手」を必ず打たせるために使う。空なら制限しない
        void RequestAnalysis(Stone colorToMove, const AnalysisBudget& budget = kLiveAnalysisBudget,
            const std::vector<std::pair<int, int>>& allowedMoves = {});
        bool TryGetAnalysisResult(KataGoAnalysisResult& outResult);

        // moves全体を0手目から末尾まで順に解析する。順方向にしか進まないためclear_boardは
        // 最初の1回だけで、以降は1手ずつplayして局面を進める(従来のように毎局面で
        // 0手目から全再生し直すO(n²)を避け、played手数nに対してO(n)にする)。
        // 盤面再生もこのワーカースレッド内で行うため描画ループを止めない。
        // moves自体は値渡しでワーカーへ移す(呼び出し側の寿命に依存しない)
        void RequestBatchAnalysis(std::vector<KataGoPlayedMove> moves, const AnalysisBudget& budget);
        // 解析が終わった局面を古い手数から順に1件取り出す(取り出した分はキューから消える)。
        // 毎フレーム呼び、取れるだけ取り出してドレインする想定
        bool TryPopBatchAnalysisItem(BatchAnalysisItem& outItem);
        // RequestBatchAnalysisが実行中(まだ全局面を解析し終えていない)かどうか
        bool IsBatchAnalysisRunning() const { return m_BatchRunning.load(); }
        // 実行中の一括解析を中断する。現在解析中の1局面ぶんはHardCapMsを上限に完了を待ってから
        // 停止するため、呼び出し後すぐにIsBatchAnalysisRunningがfalseになるとは限らない。
        // 新規対局の開始・KataGoClientの再起動(StartAsync)・アプリ終了の前に必ず呼ぶこと
        // (呼ばずに次の要求を送ると、その要求の完了までブロックされる)
        void CancelBatchAnalysis() { m_BatchCancel.store(true); }

        const std::string& LastError() const { return m_LastError; }

    private:
        void LaunchProcess(const std::filesystem::path& exePath, const std::filesystem::path& modelPath,
            const std::filesystem::path& humanModelPath, const std::string& humanSLProfile,
            const std::filesystem::path& configPath, const std::filesystem::path& stderrLogPath, int maxVisits);
        // デストラクタとStartAsync(再起動時)の両方から呼ぶプロセス終了処理
        void ShutdownProcessIfRunning();
        void SendCommand(const std::string& command);
        std::string ReadResponseLine();
        // コマンド送信+応答受信を1つの操作として行い、"= "を取り除いた本文を返す。
        // GTPがエラー("?")を返した場合は例外を投げる
        std::string Exchange(const std::string& command);

        // kata-analyzeを送信し、budgetの条件を満たすまでストリーミング報告を受け取ってから停止し、
        // 最後に受け取った報告をパースして返す(詳細はKataGoClient.cpp冒頭のコメント参照)
        KataGoAnalysisResult ExchangeAnalyze(Stone colorToMove, const AnalysisBudget& budget,
            const std::vector<std::pair<int, int>>& allowedMoves = {});
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

        std::atomic<bool> m_DeadStonesReady { false };
        std::vector<std::pair<int, int>> m_DeadStones;
        bool m_DeadStonesFailed = false;

        std::atomic<bool> m_AnalysisReady { false };
        KataGoAnalysisResult m_AnalysisResult;

        // 一括解析(RequestBatchAnalysis)の結果キュー。m_BatchMutexで保護し、
        // ワーカースレッドがpush_back、TryPopBatchAnalysisItemがpop_frontする
        std::mutex m_BatchMutex;
        std::deque<BatchAnalysisItem> m_BatchQueue;
        std::atomic<bool> m_BatchRunning { false };
        std::atomic<bool> m_BatchCancel { false };

        std::string m_LastError;
    };
}
