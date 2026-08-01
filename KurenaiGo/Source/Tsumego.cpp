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

    int TsumegoProblem::BlackMoveCount() const
    {
        int count = 0;
        for (const SgfMove& move : Solution)
        {
            if (move.Color == Stone::Black)
            {
                ++count;
            }
        }
        return count;
    }

    bool AreTargetStonesUnconditionallyAlive(const GoBoard& board,
        const std::vector<std::pair<int, int>>& targetStones)
    {
        if (targetStones.empty())
        {
            return false;
        }
        for (const std::pair<int, int>& point : targetStones)
        {
            if (board.At(point.first, point.second) != Stone::White)
            {
                return false; // 1つでも取られていれば「無条件の生き」ではない
            }
        }

        const int size = board.Size();
        const auto index = [size](int row, int col) { return static_cast<size_t>(row) * size + col; };
        const auto onBoard = [size](int row, int col)
        {
            return row >= 0 && row < size && col >= 0 && col < size;
        };

        // 白の連を列挙する(chainIdはその交点が属する連の番号。白以外は-1)
        std::vector<int> chainId(static_cast<size_t>(size) * size, -1);
        std::vector<std::vector<size_t>> chainStones;
        std::vector<std::vector<bool>> chainLiberties; // 連ごとの「呼吸点かどうか」表
        for (int row = 0; row < size; ++row)
        {
            for (int col = 0; col < size; ++col)
            {
                if (board.At(row, col) != Stone::White || chainId[index(row, col)] >= 0)
                {
                    continue;
                }
                const int id = static_cast<int>(chainStones.size());
                std::vector<size_t> stones;
                std::vector<bool> liberties(static_cast<size_t>(size) * size, false);
                std::vector<std::pair<int, int>> stack{ { row, col } };
                chainId[index(row, col)] = id;
                stones.push_back(index(row, col));
                while (!stack.empty())
                {
                    const std::pair<int, int> current = stack.back();
                    stack.pop_back();
                    for (int i = 0; i < 4; ++i)
                    {
                        const int nextRow = current.first + kNeighborDeltaRow[i];
                        const int nextCol = current.second + kNeighborDeltaCol[i];
                        if (!onBoard(nextRow, nextCol))
                        {
                            continue;
                        }
                        const Stone neighbor = board.At(nextRow, nextCol);
                        if (neighbor == Stone::Empty)
                        {
                            liberties[index(nextRow, nextCol)] = true;
                        }
                        else if (neighbor == Stone::White && chainId[index(nextRow, nextCol)] < 0)
                        {
                            chainId[index(nextRow, nextCol)] = id;
                            stones.push_back(index(nextRow, nextCol));
                            stack.push_back({ nextRow, nextCol });
                        }
                    }
                }
                chainStones.push_back(std::move(stones));
                chainLiberties.push_back(std::move(liberties));
            }
        }

        // 白以外の点(空点と黒石)の連結成分=「領域」を列挙し、接している白の連を記録する
        struct Region
        {
            std::vector<size_t> Empties;
            std::vector<int> Neighbors; // 接している白の連の番号(重複なし)
        };
        std::vector<Region> regions;
        std::vector<int> regionId(static_cast<size_t>(size) * size, -1);
        for (int row = 0; row < size; ++row)
        {
            for (int col = 0; col < size; ++col)
            {
                if (board.At(row, col) == Stone::White || regionId[index(row, col)] >= 0)
                {
                    continue;
                }
                const int id = static_cast<int>(regions.size());
                Region region;
                std::vector<std::pair<int, int>> stack{ { row, col } };
                regionId[index(row, col)] = id;
                while (!stack.empty())
                {
                    const std::pair<int, int> current = stack.back();
                    stack.pop_back();
                    if (board.At(current.first, current.second) == Stone::Empty)
                    {
                        region.Empties.push_back(index(current.first, current.second));
                    }
                    for (int i = 0; i < 4; ++i)
                    {
                        const int nextRow = current.first + kNeighborDeltaRow[i];
                        const int nextCol = current.second + kNeighborDeltaCol[i];
                        if (!onBoard(nextRow, nextCol))
                        {
                            continue;
                        }
                        if (board.At(nextRow, nextCol) == Stone::White)
                        {
                            const int neighborChain = chainId[index(nextRow, nextCol)];
                            if (std::find(region.Neighbors.begin(), region.Neighbors.end(), neighborChain) ==
                                region.Neighbors.end())
                            {
                                region.Neighbors.push_back(neighborChain);
                            }
                        }
                        else if (regionId[index(nextRow, nextCol)] < 0)
                        {
                            regionId[index(nextRow, nextCol)] = id;
                            stack.push_back({ nextRow, nextCol });
                        }
                    }
                }
                regions.push_back(std::move(region));
            }
        }

        // Bensonの反復: 「vitalな領域(その領域の空点がすべてその連の呼吸点)」を2つ以上
        // 持たない連を除き、生き残っている連だけに囲まれていない領域を除く。これを変化が
        // なくなるまで繰り返し、残った連が無条件に生きている
        std::vector<bool> chainAlive(chainStones.size(), true);
        std::vector<bool> regionAlive(regions.size(), true);
        bool changed = true;
        while (changed)
        {
            changed = false;
            for (size_t chain = 0; chain < chainStones.size(); ++chain)
            {
                if (!chainAlive[chain])
                {
                    continue;
                }
                int vitalCount = 0;
                for (size_t region = 0; region < regions.size(); ++region)
                {
                    if (!regionAlive[region])
                    {
                        continue;
                    }
                    const std::vector<int>& neighbors = regions[region].Neighbors;
                    if (std::find(neighbors.begin(), neighbors.end(), static_cast<int>(chain)) == neighbors.end())
                    {
                        continue;
                    }
                    bool vital = true;
                    for (size_t empty : regions[region].Empties)
                    {
                        if (!chainLiberties[chain][empty])
                        {
                            vital = false;
                            break;
                        }
                    }
                    if (vital)
                    {
                        ++vitalCount;
                    }
                }
                if (vitalCount < 2)
                {
                    chainAlive[chain] = false;
                    changed = true;
                }
            }
            for (size_t region = 0; region < regions.size(); ++region)
            {
                if (!regionAlive[region])
                {
                    continue;
                }
                for (int neighbor : regions[region].Neighbors)
                {
                    if (!chainAlive[static_cast<size_t>(neighbor)])
                    {
                        regionAlive[region] = false;
                        changed = true;
                        break;
                    }
                }
            }
        }

        for (const std::pair<int, int>& point : targetStones)
        {
            const int chain = chainId[index(point.first, point.second)];
            if (chain < 0 || !chainAlive[static_cast<size_t>(chain)])
            {
                return false;
            }
        }
        return true;
    }

    namespace
    {
        // SGFの1局を詰碁の問題へ変換する。出題できない形なら理由をoutSkipReasonsへ積んでfalseを返す。
        // labelはエラーログ用の名前(1ファイルに複数問入っている場合は何問目かを含む)
        bool TryBuildTsumegoProblem(const SgfGameRecord& record, const std::string& label,
            std::vector<std::string>& outSkipReasons, TsumegoProblem& outProblem)
        {
            if (record.PlayerToMove != Stone::Black)
            {
                outSkipReasons.push_back(label + ": PL[B](黒先)ではないため読み飛ばしました");
                return false;
            }

            TsumegoProblem problem;
            problem.FileName = label;
            problem.BoardSize = record.BoardSize;
            problem.Title = record.GameName.empty() ? label : record.GameName;
            problem.Description = record.Comment;
            problem.Difficulty = (record.Difficulty > 0) ? record.Difficulty : 1;

            // 正解手順(着手ノード)。黒から始まり黒白が交互で、パスを含まないものだけ採用する。
            // 形式が合わない問題図は手順を持たない扱いにし、KataGoの死活判定へ回す
            bool solutionValid = !record.Moves.empty();
            for (size_t i = 0; i < record.Moves.size() && solutionValid; ++i)
            {
                const Stone expected = (i % 2 == 0) ? Stone::Black : Stone::White;
                solutionValid = !record.Moves[i].IsPass && record.Moves[i].Color == expected;
            }
            if (solutionValid)
            {
                problem.Solution = record.Moves;
            }
            else if (!record.Moves.empty())
            {
                outSkipReasons.push_back(label +
                    ": 正解手順(着手ノード)が黒白交互になっていないため手順を使いません");
            }
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
                outSkipReasons.push_back(label + ": SQ(判定対象)に白石以外の交点が含まれているため読み飛ばしました");
                return false;
            }
            if (problem.TargetStones.empty())
            {
                outSkipReasons.push_back(label + ": 生死の判定対象になる白石が無いため読み飛ばしました");
                return false;
            }

            // GTPのplayで再現できない問題図(どの順で置いても呼吸点0になる)は採用しない。
            // ここで弾いておかないと、対局開始時にKataGo側の盤面だけが壊れてしまう
            std::vector<SgfSetupStone> ordered;
            if (!TryOrderSetupStonesForGtp(problem.SetupStones, problem.BoardSize, ordered))
            {
                outSkipReasons.push_back(label + ": 初期配置をGTPのplayで再現できないため読み飛ばしました");
                return false;
            }

            outProblem = std::move(problem);
            return true;
        }
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
            // 1ファイルに複数の問題を並べたSGFコレクションを読む(同梱の問題集は290問を
            // 1ファイルにまとめている)。利用者が1問だけのファイルを追加した場合もそのまま読める
            std::vector<SgfGameRecord> records;
            try
            {
                records = ReadSgfCollectionFile(path);
            }
            catch (const std::exception& e)
            {
                outSkipReasons.push_back(path.filename().string() +
                    ": 読み込みに失敗しました: " + e.what());
                continue;
            }

            for (size_t recordIndex = 0; recordIndex < records.size(); ++recordIndex)
            {
                const std::string label = (records.size() > 1)
                    ? path.filename().string() + "(" + std::to_string(recordIndex + 1) + "問目)"
                    : path.filename().string();
                try
                {
                    TsumegoProblem problem;
                    if (TryBuildTsumegoProblem(records[recordIndex], label, outSkipReasons, problem))
                    {
                        problems.push_back(std::move(problem));
                    }
                }
                catch (const std::exception& e)
                {
                    outSkipReasons.push_back(label + ": 読み込みに失敗しました: " + e.what());
                }
            }
        }

        return problems;
    }
}
