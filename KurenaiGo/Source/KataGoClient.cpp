#include "KataGoClient.h"

#include <array>
#include <cctype>
#include <stdexcept>

namespace KurenaiGo
{
    namespace
    {
        // ハンドルを1つ安全に閉じる(nullptrなら何もしない)
        void SafeCloseHandle(HANDLE& handle)
        {
            if (handle)
            {
                CloseHandle(handle);
                handle = nullptr;
            }
        }

        // 空白区切りでトークン分割する(kata-analyzeの報告行のパース用)
        std::vector<std::string> Tokenize(const std::string& line)
        {
            std::vector<std::string> tokens;
            size_t pos = 0;
            while (pos < line.size())
            {
                while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos])))
                {
                    ++pos;
                }
                if (pos >= line.size())
                {
                    break;
                }
                const size_t start = pos;
                while (pos < line.size() && !std::isspace(static_cast<unsigned char>(line[pos])))
                {
                    ++pos;
                }
                tokens.push_back(line.substr(start, pos - start));
            }
            return tokens;
        }
    }

    KataGoClient::~KataGoClient()
    {
        // 進行中のスレッド(起動処理またはgenmove/final_score)が使っているパイプを
        // 閉じてしまわないよう、先にjoinで完了を待つ。genmoveはtime_settingsで
        // 上限秒数が決まっているため、待ちは有限時間で終わる
        if (m_WorkerThread.joinable())
        {
            m_WorkerThread.join();
        }
        ShutdownProcessIfRunning();
    }

    void KataGoClient::ShutdownProcessIfRunning()
    {
        if (m_ChildStdinWrite)
        {
            // quitに対する応答は待たない(プロセス終了待ちで代用する)。
            // 送信自体に失敗してもTerminateProcessで確実に後始末する
            try
            {
                SendCommand("quit");
            }
            catch (...)
            {
            }
        }

        if (m_ProcessHandle)
        {
            const DWORD waitResult = WaitForSingleObject(m_ProcessHandle, 3000);
            if (waitResult != WAIT_OBJECT_0)
            {
                TerminateProcess(m_ProcessHandle, 0);
                WaitForSingleObject(m_ProcessHandle, INFINITE);
            }
        }

        SafeCloseHandle(m_ChildStdinWrite);
        SafeCloseHandle(m_ChildStdoutRead);
        SafeCloseHandle(m_ProcessHandle);
    }

    void KataGoClient::LaunchProcess(const std::filesystem::path& exePath, const std::filesystem::path& modelPath,
        const std::filesystem::path& humanModelPath, const std::string& humanSLProfile,
        const std::filesystem::path& configPath, const std::filesystem::path& stderrLogPath, int maxVisits)
    {
        SECURITY_ATTRIBUTES securityAttributes{};
        securityAttributes.nLength = sizeof(SECURITY_ATTRIBUTES);
        securityAttributes.bInheritHandle = TRUE;
        securityAttributes.lpSecurityDescriptor = nullptr;

        HANDLE childStdoutRead = nullptr;
        HANDLE childStdoutWrite = nullptr;
        if (!CreatePipe(&childStdoutRead, &childStdoutWrite, &securityAttributes, 0))
        {
            throw std::runtime_error("標準出力パイプの作成に失敗しました (GetLastError: " + std::to_string(GetLastError()) + ")");
        }
        // 親側(読み取り用)は子プロセスへ継承させない
        SetHandleInformation(childStdoutRead, HANDLE_FLAG_INHERIT, 0);

        HANDLE childStdinRead = nullptr;
        HANDLE childStdinWrite = nullptr;
        if (!CreatePipe(&childStdinRead, &childStdinWrite, &securityAttributes, 0))
        {
            CloseHandle(childStdoutRead);
            CloseHandle(childStdoutWrite);
            throw std::runtime_error("標準入力パイプの作成に失敗しました (GetLastError: " + std::to_string(GetLastError()) + ")");
        }
        // 親側(書き込み用)は子プロセスへ継承させない
        SetHandleInformation(childStdinWrite, HANDLE_FLAG_INHERIT, 0);

        // KataGoの診断ログ(初回OpenCLチューニングの進捗等)は標準エラーに出るため、
        // ファイルへリダイレクトして残す(GTPのやり取りである標準出力とは混ざらない)
        HANDLE stderrFile = CreateFileW(
            stderrLogPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &securityAttributes,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (stderrFile == INVALID_HANDLE_VALUE)
        {
            CloseHandle(childStdoutRead);
            CloseHandle(childStdoutWrite);
            CloseHandle(childStdinRead);
            CloseHandle(childStdinWrite);
            throw std::runtime_error("KataGoのログファイル作成に失敗しました: " + stderrLogPath.string());
        }

        std::wstring commandLine = L"\"" + exePath.wstring() + L"\" gtp -model \"" + modelPath.wstring() + L"\"";

        // humanSLProfileが非空の場合のみHuman SLモデル(11.6節)を追加起動する。ASCII文字のみの
        // 前提(profileSuffixは"rank_15k"のような英数字・アンダースコアのみの文字列)のため、
        // 文字単位でそのままwstringへ広げてよい
        std::string overrideConfig = "maxVisits=" + std::to_string(maxVisits);
        if (!humanSLProfile.empty())
        {
            commandLine += L" -human-model \"" + humanModelPath.wstring() + L"\"";
            overrideConfig += ",humanSLProfile=" + humanSLProfile +
                ",humanSLChosenMoveProp=1.0,humanSLChosenMoveIgnorePass=true,"
                "humanSLChosenMovePiklLambda=100000000,useLcbForSelection=false,"
                "rootNumSymmetriesToSample=2,chosenMoveTemperatureEarly=0.85,"
                "chosenMoveTemperature=0.70,chosenMoveTemperatureHalflife=80,"
                "chosenMoveTemperatureOnlyBelowProb=0.01";
        }
        commandLine += L" -config \"" + configPath.wstring() + L"\" -override-config " +
            std::wstring(overrideConfig.begin(), overrideConfig.end());

        std::vector<wchar_t> commandLineBuffer(commandLine.begin(), commandLine.end());
        commandLineBuffer.push_back(L'\0');

        STARTUPINFOW startupInfo{};
        startupInfo.cb = sizeof(startupInfo);
        startupInfo.dwFlags = STARTF_USESTDHANDLES;
        startupInfo.hStdInput = childStdinRead;
        startupInfo.hStdOutput = childStdoutWrite;
        startupInfo.hStdError = stderrFile;

        PROCESS_INFORMATION processInfo{};
        const std::wstring workingDirectory = exePath.parent_path().wstring();
        const BOOL created = CreateProcessW(
            exePath.c_str(),
            commandLineBuffer.data(),
            nullptr, nullptr,
            TRUE, // ハンドルを子プロセスへ継承させる
            CREATE_NO_WINDOW,
            nullptr,
            workingDirectory.c_str(),
            &startupInfo,
            &processInfo);

        // 子プロセス側に複製されたので、親はこちら側の役目を終えたハンドルを閉じる
        CloseHandle(childStdinRead);
        CloseHandle(childStdoutWrite);
        CloseHandle(stderrFile);

        if (!created)
        {
            CloseHandle(childStdoutRead);
            CloseHandle(childStdinWrite);
            throw std::runtime_error(
                "KataGo(" + exePath.string() + ")の起動に失敗しました (GetLastError: " + std::to_string(GetLastError()) + ")");
        }

        CloseHandle(processInfo.hThread);
        m_ProcessHandle = processInfo.hProcess;
        m_ChildStdoutRead = childStdoutRead;
        m_ChildStdinWrite = childStdinWrite;
    }

    void KataGoClient::SendCommand(const std::string& command)
    {
        const std::string line = command + "\n";
        DWORD bytesWritten = 0;
        if (!WriteFile(m_ChildStdinWrite, line.data(), static_cast<DWORD>(line.size()), &bytesWritten, nullptr) ||
            bytesWritten != line.size())
        {
            throw std::runtime_error("KataGoへのコマンド送信に失敗しました: " + command);
        }
    }

    std::string KataGoClient::ReadResponseLine()
    {
        // GTPの応答は "=" または "?" で始まる行に続き、空行で終端される。
        // このメソッドは応答本体(先頭の"= "を除いた1つ以上の行)をまとめて1つの文字列で返す
        std::string pending;
        std::string result;
        bool sawStatusLine = false;

        std::array<char, 256> chunk{};
        for (;;)
        {
            size_t newlinePos;
            while ((newlinePos = pending.find('\n')) != std::string::npos)
            {
                std::string line = pending.substr(0, newlinePos);
                pending.erase(0, newlinePos + 1);
                if (!line.empty() && line.back() == '\r')
                {
                    line.pop_back();
                }

                if (!sawStatusLine)
                {
                    if (line.empty())
                    {
                        continue; // 応答本体が始まる前の空行は無視
                    }
                    sawStatusLine = true;
                    result = line;
                }
                else
                {
                    if (line.empty())
                    {
                        return result; // 応答の終端
                    }
                    result += "\n" + line;
                }
            }

            DWORD bytesRead = 0;
            const BOOL ok = ReadFile(m_ChildStdoutRead, chunk.data(), static_cast<DWORD>(chunk.size()), &bytesRead, nullptr);
            if (!ok || bytesRead == 0)
            {
                throw std::runtime_error("KataGoからの応答読み取りに失敗しました(プロセスが終了した可能性があります)");
            }
            pending.append(chunk.data(), bytesRead);
        }
    }

    std::string KataGoClient::Exchange(const std::string& command)
    {
        std::lock_guard<std::mutex> lock(m_IoMutex);
        SendCommand(command);
        const std::string response = ReadResponseLine();

        if (response.empty() || (response[0] != '=' && response[0] != '?'))
        {
            throw std::runtime_error("KataGoから予期しない応答を受け取りました: [" + command + "] -> [" + response + "]");
        }
        if (response[0] == '?')
        {
            throw std::runtime_error("KataGoがエラーを返しました: [" + command + "] -> [" + response + "]");
        }

        std::string content = response.substr(1);
        const size_t start = content.find_first_not_of(" \t");
        return start == std::string::npos ? std::string() : content.substr(start);
    }

    void KataGoClient::StartAsync(const std::filesystem::path& exePath, const std::filesystem::path& modelPath,
        const std::filesystem::path& humanModelPath, const std::string& humanSLProfile,
        const std::filesystem::path& configPath, const std::filesystem::path& stderrLogPath,
        int boardSize, int maxVisits)
    {
        if (m_WorkerThread.joinable())
        {
            m_WorkerThread.join();
        }

        // 対局ごとに強さを変えるため、すでにプロセスが起動していれば終了させてから作り直す。
        // 前回のセッションの状態が残らないよう、フラグ類もすべてリセットする
        ShutdownProcessIfRunning();
        m_StartupComplete.store(false);
        m_StartupFailed.store(false);
        m_GenMoveReady.store(false);
        m_FinalScoreReady.store(false);
        m_AnalysisReady.store(false);

        m_WorkerThread = std::thread([this, exePath, modelPath, humanModelPath, humanSLProfile,
            configPath, stderrLogPath, boardSize, maxVisits]()
        {
            try
            {
                LaunchProcess(exePath, modelPath, humanModelPath, humanSLProfile, configPath, stderrLogPath, maxVisits);
                Exchange("boardsize " + std::to_string(boardSize));
                Exchange("clear_board");
                Exchange("komi " + std::to_string(kKomi));
                Exchange("time_settings 0 5 1");
            }
            catch (const std::exception& e)
            {
                m_LastError = e.what();
                m_StartupFailed.store(true);
            }
            m_StartupComplete.store(true);
        });
    }

    void KataGoClient::PlayMove(Stone color, int row, int col)
    {
        Exchange(std::string("play ") + ToGtpColorChar(color) + " " + ToVertex(row, col));
    }

    void KataGoClient::PlayPass(Stone color)
    {
        Exchange(std::string("play ") + ToGtpColorChar(color) + " pass");
    }

    void KataGoClient::ResetBoard()
    {
        Exchange("clear_board");
    }

    void KataGoClient::RequestGenMove(Stone color)
    {
        if (m_WorkerThread.joinable())
        {
            m_WorkerThread.join();
        }
        m_GenMoveReady.store(false);

        m_WorkerThread = std::thread([this, color]()
        {
            KataGoMoveResult result;
            try
            {
                const std::string vertex = Exchange(std::string("genmove ") + ToGtpColorChar(color));
                ParseVertex(vertex, result);
            }
            catch (const std::exception& e)
            {
                result.Failed = true;
                m_LastError = e.what();
            }
            m_GenMoveResult = result;
            m_GenMoveReady.store(true);
        });
    }

    bool KataGoClient::TryGetGenMoveResult(KataGoMoveResult& outResult)
    {
        if (!m_GenMoveReady.load())
        {
            return false;
        }
        outResult = m_GenMoveResult;
        return true;
    }

    void KataGoClient::RequestFinalScore()
    {
        if (m_WorkerThread.joinable())
        {
            m_WorkerThread.join();
        }
        m_FinalScoreReady.store(false);

        m_WorkerThread = std::thread([this]()
        {
            std::string result;
            try
            {
                result = Exchange("final_score");
            }
            catch (const std::exception& e)
            {
                result = std::string("(取得失敗: ") + e.what() + ")";
            }
            m_FinalScoreResult = result;
            m_FinalScoreReady.store(true);
        });
    }

    bool KataGoClient::TryGetFinalScore(std::string& outResult)
    {
        if (!m_FinalScoreReady.load())
        {
            return false;
        }
        outResult = m_FinalScoreResult;
        return true;
    }

    void KataGoClient::RequestAnalysis(Stone colorToMove, const AnalysisBudget& budget)
    {
        if (m_WorkerThread.joinable())
        {
            m_WorkerThread.join();
        }
        m_AnalysisReady.store(false);

        m_WorkerThread = std::thread([this, colorToMove, budget]()
        {
            KataGoAnalysisResult result;
            result.ColorToMove = colorToMove;
            try
            {
                result = ExchangeAnalyze(colorToMove, budget);
            }
            catch (const std::exception& e)
            {
                result = KataGoAnalysisResult{};
                result.ColorToMove = colorToMove;
                result.Failed = true;
                m_LastError = e.what();
            }
            m_AnalysisResult = result;
            m_AnalysisReady.store(true);
        });
    }

    bool KataGoClient::TryGetAnalysisResult(KataGoAnalysisResult& outResult)
    {
        if (!m_AnalysisReady.load())
        {
            return false;
        }
        outResult = m_AnalysisResult;
        return true;
    }

    void KataGoClient::RequestBatchAnalysis(std::vector<KataGoPlayedMove> moves, const AnalysisBudget& budget)
    {
        if (m_WorkerThread.joinable())
        {
            m_WorkerThread.join();
        }
        {
            std::lock_guard<std::mutex> lock(m_BatchMutex);
            m_BatchQueue.clear();
        }
        m_BatchCancel.store(false);
        m_BatchRunning.store(true);

        m_WorkerThread = std::thread([this, moves = std::move(moves), budget]()
        {
            // 順方向専用: clear_boardは最初の1回だけ送り、以降は1手ずつplayして局面を進める。
            // 従来の「毎局面でclear_board+全再生」に比べ、playの往復が局面数nに対してO(n)になる
            // (詳細はKataGoClient.h RequestBatchAnalysisのコメント参照)
            try
            {
                Exchange("clear_board");

                for (size_t i = 0; i <= moves.size(); ++i)
                {
                    if (m_BatchCancel.load())
                    {
                        break;
                    }

                    if (i > 0)
                    {
                        const KataGoPlayedMove& move = moves[i - 1];
                        if (move.IsPass)
                        {
                            Exchange(std::string("play ") + ToGtpColorChar(move.Color) + " pass");
                        }
                        else
                        {
                            Exchange(std::string("play ") + ToGtpColorChar(move.Color) + " " +
                                ToVertex(move.Row, move.Col));
                        }
                    }

                    if (m_BatchCancel.load())
                    {
                        break;
                    }

                    const Stone colorToMove = (i == 0) ? Stone::Black : Opponent(moves[i - 1].Color);
                    BatchAnalysisItem item;
                    item.MoveIndex = static_cast<int>(i);
                    try
                    {
                        item.Result = ExchangeAnalyze(colorToMove, budget);
                    }
                    catch (const std::exception& e)
                    {
                        // 1局面の解析失敗は致命的ではないため、Failedを立てて次の局面へ進む
                        // (対局後解析・棋譜再生前解析のどちらも補助機能のため、途中の1局面が
                        // 欠けても残りを止めない)
                        item.Result = KataGoAnalysisResult{};
                        item.Result.ColorToMove = colorToMove;
                        item.Result.Failed = true;
                        m_LastError = e.what();
                    }

                    std::lock_guard<std::mutex> lock(m_BatchMutex);
                    m_BatchQueue.push_back(std::move(item));
                }
            }
            catch (const std::exception& e)
            {
                // clear_board/play自体の通信エラー(プロセス終了等)は致命的なため、
                // ここで一括解析全体を打ち切る。呼び出し側はキューに届いた分だけを使う
                m_LastError = e.what();
            }
            m_BatchRunning.store(false);
        });
    }

    bool KataGoClient::TryPopBatchAnalysisItem(BatchAnalysisItem& outItem)
    {
        std::lock_guard<std::mutex> lock(m_BatchMutex);
        if (m_BatchQueue.empty())
        {
            return false;
        }
        outItem = std::move(m_BatchQueue.front());
        m_BatchQueue.pop_front();
        return true;
    }

    void KataGoClient::ParseAnalysisLine(const std::string& line, KataGoAnalysisResult& outResult)
    {
        const std::vector<std::string> tokens = Tokenize(line);
        outResult.TopMoves.clear();
        outResult.Ownership.clear();

        size_t i = 0;
        while (i < tokens.size())
        {
            if (tokens[i] == "info" && i + 1 < tokens.size() && tokens[i + 1] == "move")
            {
                AnalysisMoveInfo move;
                float scoreLead = 0.0f;
                if (i + 2 < tokens.size() && tokens[i + 2] != "pass")
                {
                    try
                    {
                        ParseNormalVertex(tokens[i + 2], move.Row, move.Col);
                    }
                    catch (const std::exception&)
                    {
                        // 解釈できない頂点表記は座標なし(Row/Col=-1)のまま無視する
                    }
                }
                i += 3;

                while (i < tokens.size() && tokens[i] != "info" && tokens[i] != "ownership")
                {
                    const std::string& key = tokens[i];
                    if (key == "pv")
                    {
                        // pvは指し手(GTPの頂点表記)が可変長で続くリストで、次のinfo/ownershipまで
                        // 続く。先頭はこの候補手自身で、以降は交互に相手・自分の手が並ぶ。
                        // 盤上に重ねて表示できる手数には限りがあるためkMaxPvLength手で打ち切り、
                        // pass・解釈できない頂点表記が現れた時点でもそこで止める(その先は色の
                        // 交互性が崩れて盤上に正しく並べられないため)
                        ++i;
                        while (i < tokens.size() && tokens[i] != "info" && tokens[i] != "ownership")
                        {
                            if (static_cast<int>(move.Pv.size()) < kMaxPvLength)
                            {
                                AnalysisPvPoint point;
                                try
                                {
                                    ParseNormalVertex(tokens[i], point.Row, point.Col);
                                    move.Pv.push_back(point);
                                }
                                catch (const std::exception&)
                                {
                                    // pass等、盤上の頂点でない手が現れたらそこで読み筋を打ち切る
                                    // (残りのトークンはこのループで読み飛ばすだけにする)
                                    while (i < tokens.size() && tokens[i] != "info" && tokens[i] != "ownership")
                                    {
                                        ++i;
                                    }
                                    break;
                                }
                            }
                            ++i;
                        }
                        continue;
                    }

                    if (i + 1 >= tokens.size())
                    {
                        break;
                    }
                    const std::string& value = tokens[i + 1];
                    if (key == "visits")
                    {
                        move.Visits = std::stoi(value);
                    }
                    else if (key == "winrate")
                    {
                        move.Winrate = std::stof(value);
                    }
                    else if (key == "order")
                    {
                        move.Order = std::stoi(value);
                    }
                    else if (key == "scoreLead")
                    {
                        scoreLead = std::stof(value);
                    }
                    i += 2;
                }

                if (move.Order == 0)
                {
                    outResult.WinrateForColorToMove = move.Winrate;
                    outResult.ScoreLeadForColorToMove = scoreLead;
                }
                outResult.TopMoves.push_back(move);
            }
            else if (tokens[i] == "ownership")
            {
                ++i;
                outResult.Ownership.reserve(tokens.size() - i);
                while (i < tokens.size())
                {
                    try
                    {
                        outResult.Ownership.push_back(std::stof(tokens[i]));
                    }
                    catch (const std::exception&)
                    {
                        // 数値でないトークンが紛れ込んだ場合は無視する
                    }
                    ++i;
                }
            }
            else
            {
                ++i;
            }
        }
    }

    KataGoAnalysisResult KataGoClient::ExchangeAnalyze(Stone colorToMove, const AnalysisBudget& budget)
    {
        // kata-analyzeはGTPの即時応答を返さず、代わりに"info move ... ownership ..."という
        // 1行の報告を定期的に送り続ける(1行が更新のたびに丸ごと再送される)。
        //
        // 停止方法: 空行を送って止める一般的な方法は、このKataGoビルドでは特定のタイミング
        // (報告を受け取ってから短時間で停止要求すると)で応答が返らなくなる不具合を実機で確認した
        // (kata-analyze開始直後の素の"="応答自体は問題なく、その後の空行停止のみが影響を受ける)。
        // 代わりに、通常のGTPコマンド(ここではprotocol_version)を送ると解析はただちに中断され、
        // そのコマンド自身の正常な応答が届く。この方式は複数回の実機検証で安定して動作したため、
        // 停止には空行ではなく実コマンドを使う。詳細な検証結果はdocs/KurenaiGo_Developer.htmlを参照
        //
        // interval引数の単位はミリ秒ではなくセンチ秒(1/100秒)。実機計測で確認済み
        // (interval 50を指定すると最初の報告が約500ms後に届く。詳細はdocs/KurenaiGo_Developer.html
        // 8.2節参照)。以前はミリ秒のつもりで50を指定しており、下のループはsawReportが立つまで
        // budget.TargetMs等の打ち切り判定に入らないため、実質どの局面も最低500ms待たされていた。
        // 探索自体はもっと速く目標visits数に到達しているため、報告間隔を2(=20ms)まで縮めて
        // 打ち切り判定のポーリング粒度を budget.TargetMs/TargetVisits に対して十分細かくする

        KataGoAnalysisResult result;
        result.ColorToMove = colorToMove;

        // PlayMove/RequestGenMove等、他のGTPやり取りと同時にパイプへ読み書きしないよう、
        // この解析のやり取り全体(送信〜終端行を読むまで)を通してロックを保持する
        std::lock_guard<std::mutex> lock(m_IoMutex);

        const std::string command = std::string("kata-analyze ") + ToGtpColorChar(colorToMove) +
            " interval 2 ownership true";
        SendCommand(command);

        const DWORD startTick = GetTickCount();
        bool stopSent = false;
        bool sawReport = false;
        std::string pending;
        std::array<char, 4096> chunk{};

        for (;;)
        {
            size_t newlinePos;
            while ((newlinePos = pending.find('\n')) != std::string::npos)
            {
                std::string line = pending.substr(0, newlinePos);
                pending.erase(0, newlinePos + 1);
                if (!line.empty() && line.back() == '\r')
                {
                    line.pop_back();
                }

                if (line.rfind("info ", 0) == 0)
                {
                    ParseAnalysisLine(line, result);
                    sawReport = true;
                }
                else if (!line.empty() && line[0] == '?')
                {
                    throw std::runtime_error("KataGoがkata-analyzeでエラーを返しました: " + line);
                }
                else if (!line.empty() && line[0] == '=' && stopSent)
                {
                    // kata-analyze開始直後にも素の"="(空白なし)を一度返すため、停止要求より前に
                    // 届く"="は前置き応答として無視する。本当の終端は停止コマンド送信後に届く
                    // "= "(protocol_versionへの応答)のみ
                    return result;
                }
                // 空行・停止要求前の前置き応答等はそのまま無視して次の行へ
            }

            if (!stopSent)
            {
                const DWORD elapsed = GetTickCount() - startTick;
                int bestVisits = 0;
                for (const AnalysisMoveInfo& move : result.TopMoves)
                {
                    if (move.Order == 0)
                    {
                        bestVisits = move.Visits;
                        break;
                    }
                }

                const bool shouldStop = elapsed >= budget.HardCapMs ||
                    (sawReport && (elapsed >= budget.TargetMs || bestVisits >= budget.TargetVisits));
                if (shouldStop)
                {
                    SendCommand("protocol_version"); // 実コマンドの送信でkata-analyzeを中断させる
                    stopSent = true;
                }
            }

            DWORD bytesRead = 0;
            const BOOL ok = ReadFile(m_ChildStdoutRead, chunk.data(), static_cast<DWORD>(chunk.size()), &bytesRead, nullptr);
            if (!ok || bytesRead == 0)
            {
                throw std::runtime_error("KataGoからの解析応答読み取りに失敗しました(プロセスが終了した可能性があります)");
            }
            pending.append(chunk.data(), bytesRead);
        }
    }

    std::string KataGoClient::ToVertex(int row, int col)
    {
        // GTPの列はA始まりでIを飛ばす(A,B,C,D,E,F,G,H,J,K,...)。行は盤の下端から1始まり
        char columnChar = static_cast<char>('A' + col);
        if (columnChar >= 'I')
        {
            columnChar = static_cast<char>(columnChar + 1);
        }
        return std::string(1, columnChar) + std::to_string(row + 1);
    }

    void KataGoClient::ParseNormalVertex(const std::string& vertex, int& outRow, int& outCol)
    {
        if (vertex.size() < 2)
        {
            throw std::runtime_error("KataGoの座標を解釈できませんでした: " + vertex);
        }

        char columnChar = static_cast<char>(std::toupper(static_cast<unsigned char>(vertex[0])));
        int columnIndex = columnChar - 'A';
        if (columnChar > 'I')
        {
            columnIndex -= 1; // Iを飛ばしている分を補正
        }
        const int rowNumber = std::stoi(vertex.substr(1));

        outCol = columnIndex;
        outRow = rowNumber - 1;
    }

    void KataGoClient::ParseVertex(const std::string& vertex, KataGoMoveResult& outResult)
    {
        std::string trimmed = vertex;
        while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front())))
        {
            trimmed.erase(trimmed.begin());
        }
        while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back())))
        {
            trimmed.pop_back();
        }

        std::string lower = trimmed;
        for (char& c : lower)
        {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }

        if (lower == "pass")
        {
            outResult.IsPass = true;
            return;
        }
        if (lower == "resign")
        {
            outResult.IsResign = true;
            return;
        }

        ParseNormalVertex(trimmed, outResult.Row, outResult.Col);
    }

    char KataGoClient::ToGtpColorChar(Stone color)
    {
        return color == Stone::Black ? 'B' : 'W';
    }
}
