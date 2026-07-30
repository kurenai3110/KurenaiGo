#pragma once

#include <cstdint>
#include <vector>

namespace KurenaiGo
{
    // 石の状態
    enum class Stone : uint8_t
    {
        Empty,
        Black,
        White,
    };

    // 相手の色を返す(Blackならwhite, WhiteならBlack)。Emptyを渡さないこと
    Stone Opponent(Stone color);

    // 19路盤の状態とルール(合法手判定・捕獲・シンプルコウ)を管理するクラス。
    // 座標はrow=0が盤面の下端・col=0が左端(GTPの座標系(行1=下端、列A=左端)と
    // KurenaiGoの描画座標(GridIndexToCoordinateでインデックス0が下/左になる)の
    // どちらとも一致させている。row*Size+colの1次元配列で盤面を保持する
    class GoBoard
    {
    public:
        explicit GoBoard(int size = 19);

        // (row, col)にcolorの石を打つ。既に石がある/自殺手/シンプルコウ違反のいずれかなら
        // falseを返し盤面は変化しない。合法なら着手を反映し(呼吸点0になった相手グループを
        // 取り上げる)trueを返す
        bool TryPlay(int row, int col, Stone color);

        // パスする(連続パス数をインクリメントし、コウの制約を解除する)
        void Pass();

        // 初期配置の石を置く(詰碁(17章)の問題図をSGFのAB/AWから復元するために使う)。
        // 合法手判定・捕獲・コウの更新を一切行わず、指定した交点の状態をそのまま書き換える。
        // 初期配置は「交互に打った手順」ではないため、TryPlayでは表現できない
        // (自殺手・同色の連続着手になり得る)。盤外の座標は無視する
        void PlaceSetupStone(int row, int col, Stone color);

        Stone At(int row, int col) const;
        int Size() const { return m_Size; }
        int ConsecutivePasses() const { return m_ConsecutivePasses; }

        // アゲハマ(その色が相手から取った石の数)
        int CapturesBy(Stone color) const { return color == Stone::Black ? m_BlackCaptures : m_WhiteCaptures; }

    private:
        int Index(int row, int col) const { return row * m_Size + col; }
        bool IsOnBoard(int row, int col) const;

        // (row, col)を含む同色の連結グループ(石の座標一覧)と、そのグループが接する
        // 呼吸点(空点)の座標一覧を求める(フラッドフィル)
        void CollectGroup(int row, int col, Stone color, std::vector<int>& outGroup, std::vector<int>& outLiberties) const;

        int m_Size;
        std::vector<Stone> m_Cells;
        int m_ConsecutivePasses = 0;
        int m_BlackCaptures = 0;
        int m_WhiteCaptures = 0;

        // シンプルコウ: 直前の手が「相手の石をちょうど1つ取り、打った石自身が呼吸点1つだけの
        // 単独石になる」形だった場合、その取られた地点への即座の再着手を次の1手だけ禁止する
        int m_KoForbiddenIndex = -1;
    };
}
