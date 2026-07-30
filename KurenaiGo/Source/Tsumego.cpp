#include "Tsumego.h"

#include <algorithm>
#include <exception>

namespace KurenaiGo
{
    namespace
    {
        constexpr int kNeighborDeltaRow[4] = { 1, -1, 0, 0 };
        constexpr int kNeighborDeltaCol[4] = { 0, 0, 1, -1 };

        // 並べ替えの検証用に使う、GoBoardより単純な盤面(ルール判定を持たない格子)。
        // TryOrderSetupStonesForGtpは「置いた時点で呼吸点が残るか」だけを見たいため、
        // 捕獲・コウを実装したGoBoardではなくこの格子の上で呼吸点を数える
        struct PlainBoard
        {
            int Size = 0;
            std::vector<Stone> Cells;

            explicit PlainBoard(int size)
                : Size(size)
                , Cells(static_cast<size_t>(size) * static_cast<size_t>(size), Stone::Empty)
            {
            }

            bool IsOnBoard(int row, int col) const
            {
                return row >= 0 && row < Size && col >= 0 && col < Size;
            }

            Stone At(int row, int col) const
            {
                return IsOnBoard(row, col) ? Cells[static_cast<size_t>(row) * Size + col] : Stone::Empty;
            }

            void Set(int row, int col, Stone color)
            {
                if (IsOnBoard(row, col))
                {
                    Cells[static_cast<size_t>(row) * Size + col] = color;
                }
            }
        };

        // (row, col)の石を含む同色の連の呼吸点の数を数える(フラッドフィル)。
        // 石が無い交点を渡した場合は0を返す
        int CountLiberties(const PlainBoard& board, int row, int col)
        {
            const Stone color = board.At(row, col);
            if (color == Stone::Empty)
            {
                return 0;
            }

            std::vector<int> stack;
            std::vector<bool> visited(board.Cells.size(), false);
            std::vector<bool> countedLiberty(board.Cells.size(), false);
            const auto index = [&](int r, int c) { return static_cast<size_t>(r) * board.Size + c; };

            stack.push_back(static_cast<int>(index(row, col)));
            visited[index(row, col)] = true;
            int liberties = 0;

            while (!stack.empty())
            {
                const int current = stack.back();
                stack.pop_back();
                const int currentRow = current / board.Size;
                const int currentCol = current % board.Size;

                for (int i = 0; i < 4; ++i)
                {
                    const int neighborRow = currentRow + kNeighborDeltaRow[i];
                    const int neighborCol = currentCol + kNeighborDeltaCol[i];
                    if (!board.IsOnBoard(neighborRow, neighborCol))
                    {
                        continue;
                    }
                    const size_t neighborIndex = index(neighborRow, neighborCol);
                    const Stone neighbor = board.At(neighborRow, neighborCol);
                    if (neighbor == Stone::Empty)
                    {
                        if (!countedLiberty[neighborIndex])
                        {
                            countedLiberty[neighborIndex] = true;
                            ++liberties;
                        }
                    }
                    else if (neighbor == color && !visited[neighborIndex])
                    {
                        visited[neighborIndex] = true;
                        stack.push_back(static_cast<int>(neighborIndex));
                    }
                }
            }
            return liberties;
        }

        // stoneをboardへ置いても、自分の連も隣接する相手の連も呼吸点が1つ以上残るかどうか
        bool IsSafeToPlaceNow(const PlainBoard& board, const SgfSetupStone& stone)
        {
            PlainBoard candidate = board;
            candidate.Set(stone.Row, stone.Col, stone.Color);

            if (CountLiberties(candidate, stone.Row, stone.Col) <= 0)
            {
                return false;
            }

            const Stone opponent = Opponent(stone.Color);
            for (int i = 0; i < 4; ++i)
            {
                const int neighborRow = stone.Row + kNeighborDeltaRow[i];
                const int neighborCol = stone.Col + kNeighborDeltaCol[i];
                if (candidate.At(neighborRow, neighborCol) != opponent)
                {
                    continue;
                }
                if (CountLiberties(candidate, neighborRow, neighborCol) <= 0)
                {
                    return false;
                }
            }
            return true;
        }
    }

    bool TryOrderSetupStonesForGtp(const std::vector<SgfSetupStone>& stones, int boardSize,
        std::vector<SgfSetupStone>& outOrdered)
    {
        outOrdered.clear();
        outOrdered.reserve(stones.size());

        PlainBoard board(boardSize);
        std::vector<bool> placed(stones.size(), false);
        size_t remaining = stones.size();

        while (remaining > 0)
        {
            bool progressed = false;
            for (size_t i = 0; i < stones.size(); ++i)
            {
                if (placed[i] || !IsSafeToPlaceNow(board, stones[i]))
                {
                    continue;
                }
                board.Set(stones[i].Row, stones[i].Col, stones[i].Color);
                outOrdered.push_back(stones[i]);
                placed[i] = true;
                --remaining;
                progressed = true;
            }
            if (!progressed)
            {
                // どの石を置いても呼吸点0になる=GTPのplayでは再現できない問題図
                outOrdered.clear();
                return false;
            }
        }
        return true;
    }

    bool AreTargetStonesCaptured(const GoBoard& board, const std::vector<std::pair<int, int>>& targetStones)
    {
        if (targetStones.empty())
        {
            return false;
        }
        for (const std::pair<int, int>& point : targetStones)
        {
            if (board.At(point.first, point.second) == Stone::White)
            {
                return false;
            }
        }
        return true;
    }

    std::vector<TsumegoProblem> LoadTsumegoProblems(const std::filesystem::path& directory,
        std::vector<std::string>& outSkipReasons)
    {
        std::vector<TsumegoProblem> problems;

        std::vector<std::filesystem::path> files;
        try
        {
            if (!std::filesystem::exists(directory))
            {
                return problems;
            }
            for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directory))
            {
                if (entry.is_regular_file() && entry.path().extension() == L".sgf")
                {
                    files.push_back(entry.path());
                }
            }
        }
        catch (const std::exception& e)
        {
            outSkipReasons.push_back(std::string("詰碁フォルダの走査に失敗しました: ") + e.what());
            return problems;
        }

        std::sort(files.begin(), files.end(),
            [](const std::filesystem::path& a, const std::filesystem::path& b) { return a.filename() < b.filename(); });

        for (const std::filesystem::path& path : files)
        {
            const std::string fileName = path.filename().string();
            try
            {
                const SgfGameRecord record = ReadSgfFile(path);

                if (record.PlayerToMove != Stone::Black)
                {
                    outSkipReasons.push_back(fileName + ": PL[B](黒先)ではないため読み飛ばしました");
                    continue;
                }

                TsumegoProblem problem;
                problem.FileName = fileName;
                problem.BoardSize = record.BoardSize;
                problem.Title = record.GameName.empty() ? fileName : record.GameName;
                problem.Description = record.Comment;
                problem.SetupStones = record.SetupStones;

                // 判定対象はSQ(四角マーク)で指定されていればそれを使う。指定が無ければAWの
                // 白石すべてを対象にする。いずれにしても「白石が置かれている交点」でなければ
                // 生死を判定できないため、そうでない指定は採用しない
                const auto isWhiteSetupPoint = [&record](int row, int col)
                {
                    for (const SgfSetupStone& stone : record.SetupStones)
                    {
                        if (stone.Color == Stone::White && stone.Row == row && stone.Col == col)
                        {
                            return true;
                        }
                    }
                    return false;
                };

                bool markedPointsValid = true;
                if (!record.MarkedPoints.empty())
                {
                    for (const std::pair<int, int>& point : record.MarkedPoints)
                    {
                        if (!isWhiteSetupPoint(point.first, point.second))
                        {
                            markedPointsValid = false;
                            break;
                        }
                        problem.TargetStones.push_back(point);
                    }
                }
                else
                {
                    for (const SgfSetupStone& stone : record.SetupStones)
                    {
                        if (stone.Color == Stone::White)
                        {
                            problem.TargetStones.push_back({ stone.Row, stone.Col });
                        }
                    }
                }

                if (!markedPointsValid)
                {
                    outSkipReasons.push_back(fileName + ": SQ(判定対象)に白石以外の交点が含まれているため読み飛ばしました");
                    continue;
                }
                if (problem.TargetStones.empty())
                {
                    outSkipReasons.push_back(fileName + ": 生死の判定対象になる白石が無いため読み飛ばしました");
                    continue;
                }

                // GTPのplayで再現できない問題図(どの順で置いても呼吸点0になる)は採用しない。
                // ここで弾いておかないと、対局開始時にKataGo側の盤面だけが壊れてしまう
                std::vector<SgfSetupStone> ordered;
                if (!TryOrderSetupStonesForGtp(problem.SetupStones, problem.BoardSize, ordered))
                {
                    outSkipReasons.push_back(fileName + ": 初期配置をGTPのplayで再現できないため読み飛ばしました");
                    continue;
                }

                problems.push_back(std::move(problem));
            }
            catch (const std::exception& e)
            {
                outSkipReasons.push_back(fileName + ": 読み込みに失敗しました: " + e.what());
            }
        }

        return problems;
    }
}
