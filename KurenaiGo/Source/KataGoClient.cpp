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
        const std::filesystem::path& configPath, const std::filesystem::path& stderrLogPath)
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

        std::wstring commandLine = L"\"" + exePath.wstring() + L"\" gtp -model \"" + modelPath.wstring() +
            L"\" -config \"" + configPath.wstring() + L"\"";
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
        const std::filesystem::path& configPath, const std::filesystem::path& stderrLogPath, int boardSize)
    {
        if (m_WorkerThread.joinable())
        {
            m_WorkerThread.join();
        }

        m_WorkerThread = std::thread([this, exePath, modelPath, configPath, stderrLogPath, boardSize]()
        {
            try
            {
                LaunchProcess(exePath, modelPath, configPath, stderrLogPath);
                Exchange("boardsize " + std::to_string(boardSize));
                Exchange("clear_board");
                Exchange("komi 7.5");
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

        if (trimmed.size() < 2)
        {
            throw std::runtime_error("KataGoの着手座標を解釈できませんでした: " + vertex);
        }

        char columnChar = static_cast<char>(std::toupper(static_cast<unsigned char>(trimmed[0])));
        int columnIndex = columnChar - 'A';
        if (columnChar > 'I')
        {
            columnIndex -= 1; // Iを飛ばしている分を補正
        }
        const int rowNumber = std::stoi(trimmed.substr(1));

        outResult.Col = columnIndex;
        outResult.Row = rowNumber - 1;
    }

    char KataGoClient::ToGtpColorChar(Stone color)
    {
        return color == Stone::Black ? 'B' : 'W';
    }
}
