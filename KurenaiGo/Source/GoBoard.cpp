#include "GoBoard.h"

#include <algorithm>

namespace KurenaiGo
{
    namespace
    {
        constexpr int kNeighborDeltaRow[4] = { 1, -1, 0, 0 };
        constexpr int kNeighborDeltaCol[4] = { 0, 0, 1, -1 };
    }

    Stone Opponent(Stone color)
    {
        return color == Stone::Black ? Stone::White : Stone::Black;
    }

    GoBoard::GoBoard(int size)
        : m_Size(size)
        , m_Cells(static_cast<size_t>(size) * static_cast<size_t>(size), Stone::Empty)
    {
    }

    bool GoBoard::IsOnBoard(int row, int col) const
    {
        return row >= 0 && row < m_Size && col >= 0 && col < m_Size;
    }

    Stone GoBoard::At(int row, int col) const
    {
        return IsOnBoard(row, col) ? m_Cells[Index(row, col)] : Stone::Empty;
    }

    void GoBoard::PlaceSetupStone(int row, int col, Stone color)
    {
        if (!IsOnBoard(row, col))
        {
            return;
        }
        m_Cells[Index(row, col)] = color;
    }

    void GoBoard::CollectGroup(int row, int col, Stone color, std::vector<int>& outGroup, std::vector<int>& outLiberties) const
    {
        outGroup.clear();
        outLiberties.clear();

        std::vector<int> stack;
        std::vector<bool> visited(m_Cells.size(), false);
        std::vector<bool> libertySeen(m_Cells.size(), false);

        const int startIndex = Index(row, col);
        stack.push_back(startIndex);
        visited[startIndex] = true;

        while (!stack.empty())
        {
            const int index = stack.back();
            stack.pop_back();
            outGroup.push_back(index);

            const int r = index / m_Size;
            const int c = index % m_Size;

            for (int dir = 0; dir < 4; ++dir)
            {
                const int nr = r + kNeighborDeltaRow[dir];
                const int nc = c + kNeighborDeltaCol[dir];
                if (!IsOnBoard(nr, nc))
                {
                    continue;
                }

                const int neighborIndex = Index(nr, nc);
                const Stone neighborStone = m_Cells[neighborIndex];
                if (neighborStone == Stone::Empty)
                {
                    if (!libertySeen[neighborIndex])
                    {
                        libertySeen[neighborIndex] = true;
                        outLiberties.push_back(neighborIndex);
                    }
                }
                else if (neighborStone == color && !visited[neighborIndex])
                {
                    visited[neighborIndex] = true;
                    stack.push_back(neighborIndex);
                }
            }
        }
    }

    bool GoBoard::TryPlay(int row, int col, Stone color)
    {
        if (!IsOnBoard(row, col) || color == Stone::Empty)
        {
            return false;
        }

        const int placeIndex = Index(row, col);
        if (m_Cells[placeIndex] != Stone::Empty)
        {
            return false;
        }

        // シンプルコウ: 直前の手で作られた取り返し禁止点には打てない
        if (placeIndex == m_KoForbiddenIndex)
        {
            return false;
        }

        // 一旦置いてみて、取れる相手グループがあるか調べる
        m_Cells[placeIndex] = color;

        const Stone opponent = Opponent(color);
        std::vector<int> capturedStones;
        std::vector<int> group;
        std::vector<int> liberties;

        for (int dir = 0; dir < 4; ++dir)
        {
            const int nr = row + kNeighborDeltaRow[dir];
            const int nc = col + kNeighborDeltaCol[dir];
            if (!IsOnBoard(nr, nc) || m_Cells[Index(nr, nc)] != opponent)
            {
                continue;
            }

            CollectGroup(nr, nc, opponent, group, liberties);
            if (liberties.empty())
            {
                capturedStones.insert(capturedStones.end(), group.begin(), group.end());
            }
        }

        // 複数方向から同じ相手グループに到達することがあるため重複を除去する
        std::sort(capturedStones.begin(), capturedStones.end());
        capturedStones.erase(std::unique(capturedStones.begin(), capturedStones.end()), capturedStones.end());

        for (int index : capturedStones)
        {
            m_Cells[index] = Stone::Empty;
        }

        // 相手を取り上げた後、自分のグループの呼吸点を確認する
        std::vector<int> ownGroup;
        std::vector<int> ownLiberties;
        CollectGroup(row, col, color, ownGroup, ownLiberties);

        if (ownLiberties.empty())
        {
            // 自殺手: 非合法なので全て元に戻す
            m_Cells[placeIndex] = Stone::Empty;
            for (int index : capturedStones)
            {
                m_Cells[index] = opponent;
            }
            return false;
        }

        if (!capturedStones.empty())
        {
            (color == Stone::Black ? m_BlackCaptures : m_WhiteCaptures) += static_cast<int>(capturedStones.size());
        }

        // シンプルコウの形(相手を1つだけ取り、打った石が呼吸点1つの単独石で、
        // その呼吸点がちょうど今取った地点)なら、その地点への即座の再着手を禁止する
        if (capturedStones.size() == 1 && ownGroup.size() == 1 &&
            ownLiberties.size() == 1 && ownLiberties[0] == capturedStones[0])
        {
            m_KoForbiddenIndex = capturedStones[0];
        }
        else
        {
            m_KoForbiddenIndex = -1;
        }

        m_ConsecutivePasses = 0;
        return true;
    }

    void GoBoard::Pass()
    {
        ++m_ConsecutivePasses;
        m_KoForbiddenIndex = -1;
    }
}
