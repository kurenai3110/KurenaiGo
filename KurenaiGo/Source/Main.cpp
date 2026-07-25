// KurenaiGo - 囲碁アプリ。
// KurenaiEngine2D(公開API)で19路盤・石を描画し、KataGo(https://github.com/lightvector/katago)を
// GTP(Go Text Protocol)で動かして人間(黒)とKataGo(白)の対局を行う。
// 座標系はワールド=ピクセル座標(原点は画面左下、Y-up)。
//
// 操作: 交点クリックで着手。着手以外の操作(パス・投了・地合い表示切替・着手ヒント表示切替・
//       棋譜再生・新規対局・終了)は盤下のボタン行から行う。キーボードでも同じ操作が可能:
//       Pキーでパス / Rキーで投了 / Tキーで地合い表示切替 / Hキーで着手ヒント表示切替 /
//       対局終了後Vキーで棋譜再生 / 再生中は←→キーで手を戻す・進める /
//       対局終了後・棋譜再生中はNキーで新規対局(何度でも打ち直せる) / Escで終了

#include <Windows.h>

#include <objbase.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "GoBoard.h"
#include "KataGoClient.h"
#include "KurenaiEngine2D.h"
#include "KurenaiTypes.h"
#include "MistakeStats.h"
#include "PathUtil.h"
#include "Rating.h"
#include "Sgf.h"

using namespace Kurenai;
using namespace KurenaiGo;

namespace
{
    // ソースは/utf-8オプションでコンパイルしているため、ナロー文字列リテラル("...")はUTF-8で
    // エンコードされる。MessageBoxAにそのまま渡すと、ANSI版はシステムのコードページ(日本語環境では
    // Shift-JIS)で解釈してしまい日本語部分が文字化けするため、UTF-16に変換してMessageBoxWへ渡す
    std::wstring Utf8ToWide(const std::string& utf8)
    {
        if (utf8.empty())
        {
            return std::wstring();
        }
        const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        std::wstring wide(static_cast<size_t>(length), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), length);
        return wide;
    }

    // ownerに実ウィンドウのHWNDを渡すことで、ダイアログを閉じた後にキーボードフォーカスが
    // そのウィンドウへ確実に戻る(owner省略時はダイアログの所有者が無くなり、閉じた後の
    // フォーカスの戻り先が不定になる。ダイアログを閉じた直後にキー入力を受け付ける必要がある
    // 呼び出し箇所では必ずownerを指定すること)
    int ShowMessageBoxUtf8(HWND owner, const std::string& utf8Text, const std::string& utf8Caption, UINT type)
    {
        return MessageBoxW(owner, Utf8ToWide(utf8Text).c_str(), Utf8ToWide(utf8Caption).c_str(), type);
    }

    // 対局開始前(ChoosingBoardSize)の既定の盤の目の数(9路/13路/19路から選び直せる)
    constexpr int kDefaultBoardSize = 19;

    // 盤の星(hoshi)の座標(0-indexed、row/colとも0〜boardSize-1)。9路・13路は4隅+天元の5点、
    // 19路は4隅+辺+天元の9点という、それぞれの盤の目の数で広く使われる標準的な配置
    std::vector<std::pair<int, int>> HoshiPointsForBoardSize(int boardSize)
    {
        switch (boardSize)
        {
        case 9:  return { { 2, 2 }, { 2, 6 }, { 6, 2 }, { 6, 6 }, { 4, 4 } };
        case 13: return { { 3, 3 }, { 3, 9 }, { 9, 3 }, { 9, 9 }, { 6, 6 } };
        default: // 19
        {
            constexpr int kLineIndices[3] = { 3, 9, 15 };
            std::vector<std::pair<int, int>> points;
            for (int r : kLineIndices)
            {
                for (int c : kLineIndices)
                {
                    points.push_back({ r, c });
                }
            }
            return points;
        }
        }
    }

    // 画面右側に確保する操作ボタン列の幅。ComputeBoardLayoutはこの分を除いた領域(左側)に盤を配置する
    constexpr float kButtonColumnWidth = 220.0f;
    // ボタン列の上下端からの余白(上グループ・下グループの開始位置に使う)
    constexpr float kButtonColumnMarginY = 24.0f;
    // ボタン列の左右の余白(ボタンの実幅はkButtonColumnWidthからこれを差し引いた固定幅になる)
    constexpr float kButtonColumnPaddingX = 16.0f;

    // 盤の上に確保する帯の高さ(棋譜再生中のみ、上段に着手の言語化コメント・下段に損失グラフを
    // 描画する。対局中は空のまま)
    constexpr float kGraphAreaHeight = 130.0f;
    // 上部帯の背景パネル(ボタン列と同系統の配色にする)の見た目
    constexpr float kTopPanelColorR = 0.16f, kTopPanelColorG = 0.16f, kTopPanelColorB = 0.19f;
    constexpr float kTopPanelAlpha = 0.85f;
    constexpr float kTopPanelBorderThickness = 1.5f;
    constexpr float kTopPanelBorderColorR = 0.45f, kTopPanelBorderColorG = 0.45f, kTopPanelBorderColorB = 0.48f;
    constexpr float kTopPanelCornerRadius = 10.0f;

    // ウィンドウは16:9に固定する
    constexpr uint32_t kWindowWidth = 1600;
    constexpr uint32_t kWindowHeight = 900;

    // 盤(木目)が画面短辺に対して占める割合。残りは外周の余白
    constexpr float kBoardExtentRatio = 0.88f;
    // 盤の中で格子線が占める範囲の割合。残りは木目の縁
    constexpr float kGridExtentRatio = 0.90f;

    // 木目盤の色
    constexpr float kBoardColorR = 0.80f;
    constexpr float kBoardColorG = 0.62f;
    constexpr float kBoardColorB = 0.36f;

    // 格子線・星の色(濃い木の色)
    constexpr float kLineColorR = 0.15f;
    constexpr float kLineColorG = 0.09f;
    constexpr float kLineColorB = 0.05f;

    // 背景クリア色
    constexpr float kClearColorR = 0.12f;
    constexpr float kClearColorG = 0.12f;
    constexpr float kClearColorB = 0.14f;

    // 黒石・白石の色(DrawCircleで直接描画する。勝率バーの黒/白領域にも同じ色を使い回す)
    constexpr float kBlackStoneR = 0.05f, kBlackStoneG = 0.05f, kBlackStoneB = 0.05f;
    constexpr float kWhiteStoneR = 0.95f, kWhiteStoneG = 0.95f, kWhiteStoneB = 0.92f;

    // 勝率バー(黒番から見た勝率で2色に分割する横棒)の見た目
    constexpr float kWinrateBarHeight = 18.0f;
    constexpr float kWinrateBarMargin = 6.0f; // 盤の上端からバーまでの隙間

    // 勝率バーの下に表示する数値テキスト(勝率%・目差)の見た目
    constexpr float kWinrateTextFontSize = 18.0f;
    constexpr float kWinrateTextMargin = 4.0f; // バーからテキストまでの隙間
    constexpr float kWinrateTextColorR = 0.90f, kWinrateTextColorG = 0.90f, kWinrateTextColorB = 0.88f;

    // 着手の言語化コメント(棋譜再生中のみ、kGraphAreaHeight帯の上段)の見た目
    constexpr float kCommentaryFontSize = 18.0f;
    constexpr float kCommentaryHeight = 34.0f; // 帯の上段としてこの高さ分を確保する
    constexpr float kCommentaryColorR = 0.92f, kCommentaryColorG = 0.90f, kCommentaryColorB = 0.75f;

    // 損失グラフ(棋譜再生中のみ、kGraphAreaHeight帯の下段に描画)の見た目
    constexpr float kGraphMarginX = 24.0f; // 帯の左右の余白
    constexpr float kGraphMarginTop = 12.0f; // kCommentaryHeightの下からグラフ上端までの隙間
    constexpr float kGraphMarginBottom = 8.0f;
    constexpr float kGraphLineThickness = 2.5f;
    constexpr float kGraphLineColorR = 0.95f, kGraphLineColorG = 0.75f, kGraphLineColorB = 0.25f;
    constexpr float kGraphReferenceLineThickness = 1.0f;
    constexpr float kGraphReferenceColorR = 0.45f, kGraphReferenceColorG = 0.45f, kGraphReferenceColorB = 0.48f;
    constexpr float kGraphMarkerRadius = 4.5f;
    constexpr float kGraphMarkerColorR = 1.0f, kGraphMarkerColorG = 1.0f, kGraphMarkerColorB = 1.0f;

    // 地合い可視化(Tキーでトグル)の見た目。黒地=青系、白地=赤系のオーバーレイ
    constexpr float kTerritoryBlackR = 0.25f, kTerritoryBlackG = 0.45f, kTerritoryBlackB = 0.95f;
    constexpr float kTerritoryWhiteR = 0.95f, kTerritoryWhiteG = 0.35f, kTerritoryWhiteB = 0.25f;
    // これ未満のownershipの絶対値は「地としてほぼ確定していない」とみなし表示しない
    constexpr float kTerritoryMinMagnitude = 0.15f;
    constexpr float kTerritoryMaxAlpha = 0.55f;

    // 着手ヒント(Hキーでトグル)の最大表示数と、順位ごとの色(金・銀・銅)・不透明度
    constexpr int kMaxHintMarkers = 3;
    constexpr float kHintColors[kMaxHintMarkers][3] = {
        { 1.00f, 0.85f, 0.20f },
        { 0.75f, 0.78f, 0.82f },
        { 0.80f, 0.50f, 0.30f },
    };
    constexpr float kHintAlphas[kMaxHintMarkers] = { 0.85f, 0.70f, 0.55f };

    // 盤下の余白に表示するHUDテキストの見た目
    constexpr float kHudFontSize = 21.0f;
    constexpr float kHudColorR = 0.92f, kHudColorG = 0.92f, kHudColorB = 0.90f;

    // 操作ボタン(パス・投了・地合い表示切替・着手ヒント表示切替・棋譜再生・終了等)の見た目。
    // 画面右側の縦列に、列幅いっぱいの固定幅で上から下へ積む
    constexpr float kButtonHeight = 40.0f;
    constexpr float kButtonSpacing = 10.0f; // ボタン間の縦の隙間
    constexpr float kButtonFontSize = 19.0f;

    // カジュアル対局の強さ選択(11.6節)用、盤上に表示するタブ+5列2行グリッドの見た目。
    // ButtonSpec/ButtonRect/IsPointInButtonは右側縦列と共通のものを再利用するが、タブ
    // (範囲切替)とグリッド(具体的な強さの決定)は役割が異なるため、DrawButtonにButtonStyleを
    // 渡して形状・配色を変え、見た目でも区別できるようにしている。
    // ボタン(タブ・グリッドとも)の横幅は固定値ではなく、盤の一番外側の線の内側に(左右に
    // kCasualOuterMarginずつ余白を残して)収まるよう、盤の格子の一辺(layout.GridExtent、
    // 盤の大きさによらず常に同じピクセル値)から逆算する(LayoutCasualGroupTabs/
    // LayoutCasualRankGrid参照)。ボタン間の隙間・高さのみ固定値を使う
    constexpr float kCasualOuterMargin = 24.0f; // 盤の一番外側の線とボタン列の間の余白(左右)
    constexpr int kCasualGridColumns = 5;
    constexpr float kCasualGridSpacing = 10.0f; // グリッドのボタン間の隙間(縦横とも)
    constexpr float kCasualGridFontSize = 28.0f;
    constexpr float kCasualGridCornerRadius = 6.0f; // タブより角ばった、キーパッド風の見た目

    constexpr float kCasualTabSpacing = 16.0f; // タブ間の隙間
    constexpr float kCasualTabButtonHeight = 56.0f;
    constexpr float kCasualTabFontSize = 28.0f;
    // タブは範囲切替であることが一目で分かるよう、グリッド(ニュートラルな灰色)とは異なる
    // 青みがかった配色にする
    constexpr float kCasualTabColorR = 0.20f, kCasualTabColorG = 0.30f, kCasualTabColorB = 0.42f;
    constexpr float kCasualTabHoverColorR = 0.28f, kCasualTabHoverColorG = 0.40f, kCasualTabHoverColorB = 0.54f;
    constexpr float kCasualTabBorderColorR = 0.45f, kCasualTabBorderColorG = 0.62f, kCasualTabBorderColorB = 0.80f;
    // 通常時・ホバー時・トグルON時・無効時の背景色
    constexpr float kButtonColorR = 0.30f, kButtonColorG = 0.30f, kButtonColorB = 0.34f;
    constexpr float kButtonHoverColorR = 0.42f, kButtonHoverColorG = 0.42f, kButtonHoverColorB = 0.48f;
    constexpr float kButtonActiveColorR = 0.30f, kButtonActiveColorG = 0.55f, kButtonActiveColorB = 0.35f;
    constexpr float kButtonDisabledColorR = 0.18f, kButtonDisabledColorG = 0.18f, kButtonDisabledColorB = 0.20f;
    // 通常時・無効時の文字色
    constexpr float kButtonTextColorR = 0.95f, kButtonTextColorG = 0.95f, kButtonTextColorB = 0.95f;
    constexpr float kButtonDisabledTextColorR = 0.5f, kButtonDisabledTextColorG = 0.5f, kButtonDisabledTextColorB = 0.5f;
    // ボタンの角丸半径・ドロップシャドウ・枠線・押下状態の見た目(DrawRoundedRect使用)
    constexpr float kButtonCornerRadius = 8.0f;
    constexpr float kButtonShadowOffsetX = 2.0f;
    constexpr float kButtonShadowOffsetY = -3.0f; // Y-up座標系のため下方向は負
    constexpr float kButtonShadowColorR = 0.0f, kButtonShadowColorG = 0.0f, kButtonShadowColorB = 0.0f;
    constexpr float kButtonShadowAlpha = 0.35f;
    constexpr float kButtonBorderThickness = 1.5f;
    constexpr float kButtonBorderColorR = 0.50f, kButtonBorderColorG = 0.50f, kButtonBorderColorB = 0.55f;
    constexpr float kButtonBorderActiveColorR = 0.55f, kButtonBorderActiveColorG = 0.90f, kButtonBorderActiveColorB = 0.55f;
    constexpr float kButtonPressedDarkenFactor = 0.75f; // マウス左ボタン押下中は本体色をこの倍率で暗くする
    constexpr float kButtonPressedOffsetY = -2.0f; // 押下時に本体・文字を少し沈める(Y-up座標系のため負)
    // 中央グループと「終了」ボタンの間の区切り線(誤クリック防止の視覚的な境界)
    constexpr float kButtonSeparatorThickness = 1.0f;
    constexpr float kButtonSeparatorMarginX = 12.0f; // 列の左右端からの余白
    constexpr float kButtonSeparatorColorR = 0.45f, kButtonSeparatorColorG = 0.45f, kButtonSeparatorColorB = 0.48f;

    const wchar_t* kWindowTitle = L"KurenaiGo";

    // 現在のウィンドウサイズから盤のレイアウト(中心・格子の一辺・目の間隔)を求める
    struct BoardLayout
    {
        float CenterX = 0.0f;
        float CenterY = 0.0f;
        float BoardExtent = 0.0f;
        float GridExtent = 0.0f;
        float LineSpacing = 0.0f;
        // 盤を内包する左側の利用可能幅(ウィンドウ幅 - 右側のボタン列幅)。上部帯・HUD等、
        // ボタン列の真上に重ならないよう配置したい要素の水平方向の基準に使う
        float ContentWidth = 0.0f;
    };

    BoardLayout ComputeBoardLayout(uint32_t windowWidth, uint32_t windowHeight, int boardSize)
    {
        // 盤の描画領域は、右端の操作ボタン列(kButtonColumnWidth)を除いた幅・上端の損失グラフの帯
        // (kGraphAreaHeight)を除いた高さの範囲とする(下端はボタン行が無くなったため制約が無い)
        const float contentWidth = (std::max)(1.0f, static_cast<float>(windowWidth) - kButtonColumnWidth);
        const float boardAreaHeight = (std::max)(1.0f, static_cast<float>(windowHeight) - kGraphAreaHeight);
        const float minDimension = (std::min)(contentWidth, boardAreaHeight);

        BoardLayout layout;
        layout.CenterX = contentWidth * 0.5f;
        layout.CenterY = boardAreaHeight * 0.5f;
        layout.BoardExtent = minDimension * kBoardExtentRatio;
        layout.GridExtent = layout.BoardExtent * kGridExtentRatio;
        layout.LineSpacing = layout.GridExtent / static_cast<float>(boardSize - 1);
        layout.ContentWidth = contentWidth;
        return layout;
    }

    // 格子線上のインデックス(0〜boardSize-1)からワールド座標へ変換する
    float GridIndexToCoordinate(const BoardLayout& layout, float center, int index)
    {
        const float origin = center - layout.GridExtent * 0.5f;
        return origin + layout.LineSpacing * static_cast<float>(index);
    }

    // ワールド座標(worldX, worldY)に最も近い交点を求め、スナップ範囲内であれば
    // outRow/outColに書き込みtrueを返す。盤外・範囲外ならfalse
    bool TryGetHoveredIntersection(const BoardLayout& layout, float worldX, float worldY, int boardSize,
        int& outRow, int& outCol)
    {
        const auto nearestIndex = [](const BoardLayout& layoutRef, float center, float coord) -> int
        {
            const float origin = center - layoutRef.GridExtent * 0.5f;
            return static_cast<int>(std::lround((coord - origin) / layoutRef.LineSpacing));
        };

        const int col = nearestIndex(layout, layout.CenterX, worldX);
        const int row = nearestIndex(layout, layout.CenterY, worldY);
        if (col < 0 || col >= boardSize || row < 0 || row >= boardSize)
        {
            return false;
        }

        const float exactX = GridIndexToCoordinate(layout, layout.CenterX, col);
        const float exactY = GridIndexToCoordinate(layout, layout.CenterY, row);
        const float dx = worldX - exactX;
        const float dy = worldY - exactY;
        const float snapRadius = layout.LineSpacing * 0.4f;
        if (dx * dx + dy * dy > snapRadius * snapRadius)
        {
            return false;
        }

        outRow = row;
        outCol = col;
        return true;
    }

    void DrawBoard(KurenaiEngine2D& renderer, TextureHandle whiteTexture, const BoardLayout& layout, int boardSize)
    {
        // 木目の盤面
        renderer.DrawSprite(
            layout.CenterX, layout.CenterY, layout.BoardExtent, layout.BoardExtent, 0.0f,
            whiteTexture, kBoardColorR, kBoardColorG, kBoardColorB, 1.0f);

        const float lineThickness = (std::max)(1.5f, layout.LineSpacing * 0.035f);
        const float halfGrid = layout.GridExtent * 0.5f;

        // 横線(各行ごとに1本、盤の幅いっぱいに伸ばす)
        for (int row = 0; row < boardSize; ++row)
        {
            const float y = GridIndexToCoordinate(layout, layout.CenterY, row);
            renderer.DrawLine(
                layout.CenterX - halfGrid, y, layout.CenterX + halfGrid, y, lineThickness,
                kLineColorR, kLineColorG, kLineColorB, 1.0f);
        }

        // 縦線(各列ごとに1本、盤の高さいっぱいに伸ばす)
        for (int col = 0; col < boardSize; ++col)
        {
            const float x = GridIndexToCoordinate(layout, layout.CenterX, col);
            renderer.DrawLine(
                x, layout.CenterY - halfGrid, x, layout.CenterY + halfGrid, lineThickness,
                kLineColorR, kLineColorG, kLineColorB, 1.0f);
        }

        // 星(hoshi)。盤の目の数に応じた標準的な交点に小さな点を描く
        const float hoshiRadius = layout.LineSpacing * 0.11f;
        for (const auto& [hoshiRow, hoshiCol] : HoshiPointsForBoardSize(boardSize))
        {
            const float y = GridIndexToCoordinate(layout, layout.CenterY, hoshiRow);
            const float x = GridIndexToCoordinate(layout, layout.CenterX, hoshiCol);
            renderer.DrawCircle(x, y, hoshiRadius, kLineColorR, kLineColorG, kLineColorB, 1.0f);
        }
    }

    void DrawStones(KurenaiEngine2D& renderer, const GoBoard& board, const BoardLayout& layout)
    {
        const int boardSize = board.Size();
        const float stoneRadius = layout.LineSpacing * 0.46f;
        for (int row = 0; row < boardSize; ++row)
        {
            for (int col = 0; col < boardSize; ++col)
            {
                const Stone stone = board.At(row, col);
                if (stone == Stone::Empty)
                {
                    continue;
                }

                const float x = GridIndexToCoordinate(layout, layout.CenterX, col);
                const float y = GridIndexToCoordinate(layout, layout.CenterY, row);
                if (stone == Stone::Black)
                {
                    renderer.DrawCircle(x, y, stoneRadius, kBlackStoneR, kBlackStoneG, kBlackStoneB, 1.0f);
                }
                else
                {
                    renderer.DrawCircle(x, y, stoneRadius, kWhiteStoneR, kWhiteStoneG, kWhiteStoneB, 1.0f);
                }
            }
        }
    }

    // kata-analyzeのownership配列(row-major、盤面左上=index0)における(row, col)のインデックスを
    // 求める。KurenaiGoの内部座標はrow=0が盤面下端・col=0が左端(GTPと同じ)なので変換が必要。
    // 実機での検証結果(黒石をA19に置いてindex0付近が反応することを確認)はdocs/KurenaiGo.html参照
    int OwnershipIndex(int row, int col, int boardSize)
    {
        const int rowFromTop = (boardSize - 1) - row;
        return rowFromTop * boardSize + col;
    }

    // kata-analyzeの結果は解析対象色(ColorToMove)から見た値のため、常に黒視点に変換して扱う
    float ToBlackWinrate(const KataGoAnalysisResult& analysis)
    {
        return analysis.ColorToMove == Stone::Black ? analysis.WinrateForColorToMove : 1.0f - analysis.WinrateForColorToMove;
    }

    float ToBlackOwnership(const KataGoAnalysisResult& analysis, int ownershipIndex)
    {
        const float value = analysis.Ownership[ownershipIndex];
        return analysis.ColorToMove == Stone::Black ? value : -value;
    }

    float ToBlackScoreLead(const KataGoAnalysisResult& analysis)
    {
        return analysis.ColorToMove == Stone::Black ? analysis.ScoreLeadForColorToMove : -analysis.ScoreLeadForColorToMove;
    }

    // GTPの頂点表記(列はA〜T、Iを飛ばす。行は盤面下端から1始まり)と同じ慣習で座標を文字列化する。
    // KataGoClient::ToVertexと同じ変換則(row=0が盤面下端)
    std::wstring FormatVertex(int row, int col)
    {
        wchar_t columnChar = static_cast<wchar_t>(L'A' + col);
        if (columnChar >= L'I')
        {
            columnChar = static_cast<wchar_t>(columnChar + 1);
        }
        return std::wstring(1, columnChar) + std::to_wstring(row + 1);
    }

    // 盤の上マージンに、黒番から見た勝率で2分割した横棒を描く
    void DrawWinrateBar(KurenaiEngine2D& renderer, TextureHandle whiteTexture, const BoardLayout& layout, float blackWinrate)
    {
        const float clampedWinrate = (std::max)(0.0f, (std::min)(1.0f, blackWinrate));
        const float barWidth = layout.GridExtent;
        const float barLeft = layout.CenterX - barWidth * 0.5f;
        const float barY = layout.CenterY + layout.BoardExtent * 0.5f + kWinrateBarMargin + kWinrateBarHeight * 0.5f;

        const float blackWidth = barWidth * clampedWinrate;
        const float whiteWidth = barWidth - blackWidth;

        if (blackWidth > 0.0f)
        {
            renderer.DrawSprite(
                barLeft + blackWidth * 0.5f, barY, blackWidth, kWinrateBarHeight, 0.0f,
                whiteTexture, kBlackStoneR, kBlackStoneG, kBlackStoneB, 1.0f);
        }
        if (whiteWidth > 0.0f)
        {
            renderer.DrawSprite(
                barLeft + blackWidth + whiteWidth * 0.5f, barY, whiteWidth, kWinrateBarHeight, 0.0f,
                whiteTexture, kWhiteStoneR, kWhiteStoneG, kWhiteStoneB, 1.0f);
        }
    }

    // 勝率バーのすぐ下に、黒視点の勝率(%)と目差を数値で表示する
    void DrawWinrateText(KurenaiEngine2D& renderer, const BoardLayout& layout, float blackWinrate, float blackScoreLead)
    {
        const float textX = layout.CenterX - layout.GridExtent * 0.5f;
        const float textY = layout.CenterY + layout.BoardExtent * 0.5f + kWinrateBarMargin + kWinrateBarHeight +
            kWinrateTextMargin;

        std::wostringstream text;
        text << L"黒 " << std::fixed << std::setprecision(1) << (blackWinrate * 100.0f) << L"%"
             << L"   目差 " << (blackScoreLead >= 0.0f ? L"+" : L"") << std::setprecision(1) << blackScoreLead;

        renderer.DrawText(textX, textY, text.str(), kWinrateTextFontSize,
            kWinrateTextColorR, kWinrateTextColorG, kWinrateTextColorB, 1.0f);
    }

    // 棋譜再生中、その局面の解析がまだキャッシュされていない間に表示する代わりの文言
    void DrawWinratePending(KurenaiEngine2D& renderer, const BoardLayout& layout)
    {
        const float textX = layout.CenterX - layout.GridExtent * 0.5f;
        const float textY = layout.CenterY + layout.BoardExtent * 0.5f + kWinrateBarMargin + kWinrateBarHeight +
            kWinrateTextMargin;
        renderer.DrawText(textX, textY, L"解析中...", kWinrateTextFontSize,
            kWinrateTextColorR, kWinrateTextColorG, kWinrateTextColorB, 1.0f);
    }

    // 着手の言語化: moveIndex手目の局面に至った手(record.Moves[moveIndex-1])について、
    // 着手者から見た勝率の変化と最善手だったかを日本語の文章にする。moveIndex==0(まだ1手も
    // 進めていない)なら空文字列、前後どちらかの手数が未解析なら「解析中...」を返す
    // 勝率の下げ幅による4段階分類(9.5節「着手の言語化」・13章「苦手分野の解析」で共用)。
    // 一般的なGo解析ツールで使われる目安の一例であり、局面によって適切な閾値は変わり得る
    // (絶対的な基準ではない)
    MoveQuality ClassifyMoveQuality(float deltaPercent, bool isBestMove)
    {
        if (isBestMove || deltaPercent >= -2.0f)
        {
            return MoveQuality::Best;
        }
        if (deltaPercent >= -5.0f)
        {
            return MoveQuality::SlightLoss;
        }
        if (deltaPercent >= -15.0f)
        {
            return MoveQuality::Loose;
        }
        return MoveQuality::Blunder;
    }

    std::wstring BuildMoveCommentary(const SgfGameRecord& record, int moveIndex,
        const std::vector<float>& winrateCache, const std::vector<int>& bestMoveRowCache,
        const std::vector<int>& bestMoveColCache, const std::vector<bool>& hasCached)
    {
        if (moveIndex <= 0 || moveIndex > static_cast<int>(record.Moves.size()))
        {
            return std::wstring();
        }

        const size_t beforeIndex = static_cast<size_t>(moveIndex - 1);
        const size_t afterIndex = static_cast<size_t>(moveIndex);
        if (!hasCached[beforeIndex] || !hasCached[afterIndex])
        {
            return L"解析中...";
        }

        const SgfMove& playedMove = record.Moves[beforeIndex];
        const Stone mover = playedMove.Color;

        // 着手者視点の勝率(黒視点キャッシュを着手者視点へ変換)
        const float winrateBefore = (mover == Stone::Black) ? winrateCache[beforeIndex] : 1.0f - winrateCache[beforeIndex];
        const float winrateAfter = (mover == Stone::Black) ? winrateCache[afterIndex] : 1.0f - winrateCache[afterIndex];
        const float deltaPercent = (winrateAfter - winrateBefore) * 100.0f;

        const bool isBestMove = !playedMove.IsPass &&
            bestMoveRowCache[beforeIndex] == playedMove.Row && bestMoveColCache[beforeIndex] == playedMove.Col;

        std::wostringstream text;
        text << moveIndex << L"手目: ";

        switch (ClassifyMoveQuality(deltaPercent, isBestMove))
        {
        case MoveQuality::Best:       text << L"最善手級です"; break;
        case MoveQuality::SlightLoss: text << L"やや損な手です"; break;
        case MoveQuality::Loose:      text << L"緩着です"; break;
        case MoveQuality::Blunder:    text << L"悪手です"; break;
        }

        text << L"(勝率 " << std::fixed << std::setprecision(1) << (winrateBefore * 100.0f) << L"%→"
             << (winrateAfter * 100.0f) << L"%)";

        if (!isBestMove && bestMoveRowCache[beforeIndex] >= 0 && bestMoveColCache[beforeIndex] >= 0)
        {
            text << L"  最善手は " << FormatVertex(bestMoveRowCache[beforeIndex], bestMoveColCache[beforeIndex]) << L" でした";
        }

        return text.str();
    }

    // 石のない交点に、地の所有率(黒視点)に応じた色つきオーバーレイを描く(Tキーでトグル)
    void DrawTerritoryOverlay(KurenaiEngine2D& renderer, const GoBoard& board, const BoardLayout& layout,
        TextureHandle whiteTexture, const KataGoAnalysisResult& analysis)
    {
        const int boardSize = board.Size();
        if (analysis.Ownership.size() != static_cast<size_t>(boardSize) * static_cast<size_t>(boardSize))
        {
            return;
        }

        const float overlaySize = layout.LineSpacing * 0.82f;
        for (int row = 0; row < boardSize; ++row)
        {
            for (int col = 0; col < boardSize; ++col)
            {
                if (board.At(row, col) != Stone::Empty)
                {
                    continue;
                }

                const float blackOwnership = ToBlackOwnership(analysis, OwnershipIndex(row, col, boardSize));
                const float magnitude = std::fabs(blackOwnership);
                if (magnitude < kTerritoryMinMagnitude)
                {
                    continue;
                }

                const float x = GridIndexToCoordinate(layout, layout.CenterX, col);
                const float y = GridIndexToCoordinate(layout, layout.CenterY, row);
                const float alpha = (std::min)(magnitude, 1.0f) * kTerritoryMaxAlpha;
                if (blackOwnership > 0.0f)
                {
                    renderer.DrawSprite(x, y, overlaySize, overlaySize, 0.0f, whiteTexture,
                        kTerritoryBlackR, kTerritoryBlackG, kTerritoryBlackB, alpha);
                }
                else
                {
                    renderer.DrawSprite(x, y, overlaySize, overlaySize, 0.0f, whiteTexture,
                        kTerritoryWhiteR, kTerritoryWhiteG, kTerritoryWhiteB, alpha);
                }
            }
        }
    }

    // 候補手の上位(最大kMaxHintMarkers件)に順位つきマーカーを描く(Hキーでトグル)
    void DrawMoveHints(KurenaiEngine2D& renderer, const BoardLayout& layout, TextureHandle whiteTexture,
        const KataGoAnalysisResult& analysis)
    {
        std::vector<AnalysisMoveInfo> sortedMoves = analysis.TopMoves;
        std::sort(sortedMoves.begin(), sortedMoves.end(),
            [](const AnalysisMoveInfo& a, const AnalysisMoveInfo& b) { return a.Order < b.Order; });

        const float markerSize = layout.LineSpacing * 0.6f;
        int shown = 0;
        for (const AnalysisMoveInfo& move : sortedMoves)
        {
            if (shown >= kMaxHintMarkers)
            {
                break;
            }
            if (move.Row < 0 || move.Col < 0)
            {
                continue; // passなど盤上の座標を持たない候補は表示しない
            }

            const float x = GridIndexToCoordinate(layout, layout.CenterX, move.Col);
            const float y = GridIndexToCoordinate(layout, layout.CenterY, move.Row);
            renderer.DrawSprite(x, y, markerSize, markerSize, 0.0f, whiteTexture,
                kHintColors[shown][0], kHintColors[shown][1], kHintColors[shown][2], kHintAlphas[shown]);
            ++shown;
        }
    }

    // 対局の進行状態
    enum class TurnState
    {
        HumanModelMissing, // Human SLモデル(b18c384nbt-humanv0.bin.gz)が未配置のため対局不可。起動時のみ判定する
        ChoosingBoardSize,      // 盤の大きさ(9路/13路/19路)のボタン選択待ち(対局開始前、最初の選択)
        ChoosingGameMode,       // レート戦/カジュアルのボタン選択待ち(対局開始前)
        ChoosingCasualStrength, // (カジュアル選択時のみ)AIの強さの段階選択待ち
        EngineStarting, // KataGo起動中(強さ確定後に起動するため、対局開始のたびに発生する)
        HumanToMove,     // 黒(人間)の手番
        AIThinking,      // 白(KataGo)がgenmove応答待ち
        WaitingForScore, // 両者パス後、final_score応答待ち
        GameOver,        // 対局終了。Vキーで直前の対局の棋譜再生(Reviewing)へ移れる
        Reviewing,       // 棋譜再生中。矢印キーで手を進め戻しする
        ViewingMistakeStats, // 苦手分野の解析結果を表示中(GameOver/Reviewingから遷移)
    };

    // 対局モード。レート戦のみレーティング(棋力の数値化、9.6節参照)を更新する
    enum class GameMode
    {
        Ranked, // レート戦。対局結果に応じてレーティングを更新し履歴に記録する
        Casual, // カジュアル。SGF保存は行うがレーティングには影響しない
    };

    // レーティング(0〜上限なし)↔段級位インデックスの変換(11.6節参照)。
    // 段級位インデックスは30級=0、1級=29、1段=30、9段=38、以降10段・11段…も上限なく続く
    // 整数の目安。1段のレーティングがちょうど1000になるよう、レーティングを
    // インデックスの3乗に比例させる(rating = index^3 / 27、index = 3 * cbrt(rating))。
    // 段位が上がるほど1段上がるのに必要なレーティング差が大きくなるという直感に合わせた
    // 調整可能な目安であり、科学的な較正ではない
    constexpr int kRankIndexForOneDan = 30; // 30級(index0)〜1級(index29)の30段階の直後が1段
    constexpr double kRatingForOneDan = 1000.0;

    double RankIndexForRating(double rating)
    {
        const double clamped = (std::max)(0.0, rating);
        return kRankIndexForOneDan * std::cbrt(clamped / kRatingForOneDan);
    }

    constexpr double RatingForRankIndex(double rankIndex)
    {
        return (rankIndex * rankIndex * rankIndex) / 27.0;
    }

    // 段級位インデックス→表示用の日本語文字列("30級"〜"1級","1段"〜)。上限なく続ける
    std::wstring DisplayRankTextForRankIndex(int rankIndex)
    {
        if (rankIndex < kRankIndexForOneDan)
        {
            return std::to_wstring(kRankIndexForOneDan - rankIndex) + L"級";
        }
        return std::to_wstring(rankIndex - kRankIndexForOneDan + 1) + L"段";
    }

    // KataGoのhumanSLProfileが実際にサポートするのは20級〜9段のみ(同梱gtp_human5k_example.cfgの
    // コメント記載の範囲、"RANK from 20k to 9d")。表示用の段級位(DisplayRankTextForRankIndex)は
    // この範囲外もそのまま見せるが、実際にKataGoへ渡すプロファイル文字列だけはこの範囲へ
    // クランプしてから作る
    constexpr int kHumanSLMinRankIndex = kRankIndexForOneDan - 20; // 20級
    constexpr int kHumanSLMaxRankIndex = kRankIndexForOneDan + 9 - 1; // 9段

    // Human SLモデル使用時のmaxVisits(同梱gtp_human5k_example.cfgの既定値を踏襲)。
    // この値は着手選択そのものには使われず(humanSLChosenMoveProp=1.0のため)、
    // パス/投了判定用の探索にのみ使う設計のため、レーティングによる可変スケーリングは行わない
    constexpr int kHumanSLMaxVisits = 40;

    std::string HumanSLProfileSuffixForRankIndex(int rankIndex)
    {
        const int clamped = (std::max)(kHumanSLMinRankIndex, (std::min)(kHumanSLMaxRankIndex, rankIndex));
        if (clamped < kRankIndexForOneDan)
        {
            return std::to_string(kRankIndexForOneDan - clamped) + "k";
        }
        return std::to_string(clamped - kRankIndexForOneDan + 1) + "d";
    }

    // カジュアル対局の強さ選択(盤上のタブ+5列2行グリッド、11.6節)用。タブ番号
    // (0=20〜11級, 1=10〜1級, 2=1〜9段)とグリッド内スロット番号(0〜9)から段級位
    // インデックスを求める。タブ2(段)はスロット9が存在しない(1〜9段の9個のみ)
    int RankIndexForCasualSlot(int group, int slot)
    {
        switch (group)
        {
        case 0:  return kHumanSLMinRankIndex + slot;               // 20級(idx10)〜11級(idx19)
        case 1:  return kHumanSLMinRankIndex + 10 + slot;          // 10級(idx20)〜1級(idx29)
        default: return kRankIndexForOneDan + slot;                // 1段(idx30)〜9段(idx38)
        }
    }

    // 現在のレーティングが属するカジュアル強さタブ(0/1/2)を求める(範囲外は端のタブに寄せる)
    int CasualGroupForRating(double rating)
    {
        const int rankIndex = static_cast<int>(std::lround(RankIndexForRating(rating)));
        if (rankIndex < kHumanSLMinRankIndex + 10)
        {
            return 0;
        }
        if (rankIndex < kRankIndexForOneDan)
        {
            return 1;
        }
        return 2;
    }

    // 初期レーティング決定(プレースメント)モードはいったん無効化している。
    // 基準点をkInitialRating=1500(標準的なElo初期値)から0(30級相当、11.6節の段級位換算に
    // 合わせた値)へ変更したことに伴い、EMA・収束判定まわりの初期値も合わせて再検討する
    // 必要があるため、再設計が済むまでfalseにしておく(falseの間はisCurrentGamePlacementが
    // 常にfalseになり、対局回数0のユーザーも通常のレート戦としてレーティング0から即座に
    // 対局を始める。PlacementTracker本体・以下のコメント・finalizePlacementRating等の
    // コードは再有効化に備えて削除せず残している)
    constexpr bool kPlacementModeEnabled = false;

    // 初期レーティング決定(プレースメント)モード: 対局回数0のままレート戦を始めた場合のみ
    // 発動する。黒(人間)の手番ごとに得られるkata-analyzeの勝率(WinrateForColorToMove)を
    // Eloの期待勝率式の逆算(InvertEloForRating)に通し、「この勝率を出す人間側のレーティングは
    // いくつか」を指数移動平均で追跡する。ConvergenceRate()(0.0〜1.0)がkConvergenceRateThreshold
    // (80%)を超えたら「ある程度収束した」とみなし、対局の結果を待たずにその時点で
    // rating_history.txtへ確定値を記録した上で対局自体もそこで終了する(Main.cppの
    // finalizePlacementRating呼び出し箇所を参照)。収束(80%)に達する前に投了・終局してしまった
    // 場合は、この対局ではレーティングを確定させない(GamesPlayedを増やさない)ため、
    // 次回のレート戦も引き続きプレースメントになる(複数局にまたがって収束させる想定どおりの
    // 挙動)。このとき、同じ盤の大きさで続けて始めた場合はEMA・サンプルを前の対局から引き継ぎ、
    // 収束率が対局のたびに0%へ戻らないようにする(盤の大きさを変えた場合や、間にカジュアル対局
    // を挟んだ場合はレーティングが別物になるためリセットする。Main.cppのbeginGameWithTargetRating
    // 参照)。この収束基準(EMAの重み・ウィンドウ幅・しきい値・上限サンプル数)は事前の
    // 科学的較正を行ったものではなく、妥当だと考えられる初期値である
    struct PlacementTracker
    {
        static constexpr double kEmaAlpha = 0.15;
        static constexpr int kConvergenceWindowSize = 8;
        static constexpr double kConvergenceThreshold = 15.0; // レーティング換算でこの幅未満なら収束
        static constexpr int kMinSamples = 10;
        static constexpr int kMaxSamples = 60; // 収束しなくても打ち切る上限(無限に終わらないため)
        // 収束率(ConvergenceRate)がこの値以上になったら収束とみなし対局を終了する
        static constexpr double kConvergenceRateThreshold = 0.8;
        // 収束率表示(ConvergenceRate)で「ここまで開いていればまだ0%とみなす」という
        // 基準となる幅。厳密な較正値ではなく、HUD表示用の目安
        static constexpr double kConvergenceReferenceSpread = 200.0;

        bool Active = false;
        int SampleCount = 0;
        double RunningWinrateEma = 0.5;
        std::vector<double> RecentEstimates; // 直近kConvergenceWindowSize件の推定レーティング
        // 現在追跡中の盤の大きさ(9/13/19)。収束(80%)に達する前に対局が終わって次の対局も
        // プレースメントになる場合、同じ盤の大きさが続く限りはこのトラッカーの状態(EMA・
        // サンプル)を引き継ぎ、Resetしない(Main.cppのbeginGameWithTargetRating参照)。
        // 盤の大きさを変えた場合はレーティングが別物(10.5節)になるため、引き継がずResetする
        int BoardSize = -1;

        void Reset(bool active, int boardSize)
        {
            Active = active;
            SampleCount = 0;
            RunningWinrateEma = 0.5;
            RecentEstimates.clear();
            BoardSize = boardSize;
        }

        // 黒視点の勝率を1サンプル取り込む。収束したらtrueを返す(その場合CurrentEstimate()が確定値)
        bool Update(float blackWinrate)
        {
            ++SampleCount;
            RunningWinrateEma = (SampleCount == 1)
                ? static_cast<double>(blackWinrate)
                : RunningWinrateEma * (1.0 - kEmaAlpha) + static_cast<double>(blackWinrate) * kEmaAlpha;

            const double estimate = InvertEloForRating(RunningWinrateEma, kInitialRating);
            RecentEstimates.push_back(estimate);
            if (static_cast<int>(RecentEstimates.size()) > kConvergenceWindowSize)
            {
                RecentEstimates.erase(RecentEstimates.begin());
            }

            return ConvergenceRate() >= kConvergenceRateThreshold;
        }

        // 収束時点の確定推定値(直近ウィンドウの平均)
        double CurrentEstimate() const
        {
            if (RecentEstimates.empty())
            {
                return kInitialRating;
            }
            double sum = 0.0;
            for (double v : RecentEstimates)
            {
                sum += v;
            }
            return sum / static_cast<double>(RecentEstimates.size());
        }

        // 収束率(0.0〜1.0)。HUD表示にも、Update()の収束判定(kConvergenceRateThreshold以上で
        // 収束)にも使う共通の値。サンプル数がkMinSamples未満、またはウィンドウが
        // kConvergenceWindowSize件埋まっていない間は、まだ推定値の幅(spread)を評価する
        // だけのデータが揃っていない。この段階でspreadを使うと、たまたま数手分の推定値が
        // 近い値になっただけで見かけ上の収束率が跳ね上がってしまう(対局開始直後の数手で
        // 収束率が異常に高く出ていた不具合の原因)ため、この間はサンプル数の到達度
        // (kMaxSamples分の何%集まったか)のみを収束率として使う。データが揃った後は、
        // 「幅(spread)がkConvergenceThreshold未満に近づいた度合い」と「サンプル数が
        // kMaxSamplesに近づいた度合い」の大きい方を採用する(どちらか一方の基準を満たせば
        // 収束とみなすため)
        double ConvergenceRate() const
        {
            const double sampleRate = (std::min)(1.0, static_cast<double>(SampleCount) / kMaxSamples);
            if (SampleCount < kMinSamples || static_cast<int>(RecentEstimates.size()) < kConvergenceWindowSize)
            {
                return sampleRate;
            }
            const double maxV = *std::max_element(RecentEstimates.begin(), RecentEstimates.end());
            const double minV = *std::min_element(RecentEstimates.begin(), RecentEstimates.end());
            const double spread = maxV - minV;
            const double spreadRate = 1.0 - (spread - kConvergenceThreshold) /
                (kConvergenceReferenceSpread - kConvergenceThreshold);
            return (std::max)(0.0, (std::min)(1.0, (std::max)(spreadRate, sampleRate)));
        }
    };

    // 盤下のHUDに表示する手番状態・アゲハマ数のテキストを組み立てる。reviewMoveIndex/
    // reviewTotalMoves/reviewResultはturnState==Reviewingの場合のみ使う。aiTargetRatingは
    // 今回の対局でAIが狙っている強さ(目安レーティング)。isPlacementActiveがtrueの間は
    // userRatingの代わりにplacementEstimateを「測定中」の値として、あわせて
    // placementConvergenceRate(0.0〜1.0、PlacementTracker::ConvergenceRate参照)を
    // 収束率(%)として表示する。
    // postGameAnalysisActiveの間はGameOverの表示に解析の進捗を追記する(11章参照)
    std::wstring BuildStatusText(TurnState turnState, const GoBoard& board,
        int reviewMoveIndex, int reviewTotalMoves, const std::string& reviewResult,
        double userRating, GameMode gameMode, double aiTargetRating,
        bool isPlacementActive, double placementEstimate, double placementConvergenceRate,
        bool postGameAnalysisActive, int postGameAnalysisIndex, int postGameAnalysisTotalMoves)
    {
        if (turnState == TurnState::HumanModelMissing)
        {
            return L"Human SLモデル(b18c384nbt-humanv0.bin.gz)が見つからないため対局できません。"
                L"KataGoフォルダに配置してからアプリを起動し直してください(README参照)";
        }
        if (turnState == TurnState::ChoosingBoardSize)
        {
            return L"盤の大きさを選んでください(9路/13路/19路。レーティングは大きさごとに別々に記録されます)";
        }
        if (turnState == TurnState::ChoosingGameMode)
        {
            return L"対局モードを選んでください(レート戦: 今のレーティングと互角のAIと対局/"
                L"カジュアル: 強さを自分で選べます)";
        }
        if (turnState == TurnState::ChoosingCasualStrength)
        {
            return L"カジュアル対局の強さを選んでください"
                L"(上のタブで級/段の範囲を切り替え、グリッドから具体的な強さを選べます)";
        }

        std::wstring text;
        switch (turnState)
        {
        case TurnState::EngineStarting:  text = L"KataGo起動中..."; break;
        case TurnState::HumanToMove:     text = L"あなたの番です(黒)"; break;
        case TurnState::AIThinking:      text = L"KataGo思考中..."; break;
        case TurnState::WaitingForScore: text = L"終局判定中..."; break;
        case TurnState::GameOver:
            text = L"対局終了(Vキーで棋譜再生)";
            if (postGameAnalysisActive)
            {
                text += L"  対局後の解析中 (" + std::to_wstring((std::min)(postGameAnalysisIndex, postGameAnalysisTotalMoves)) +
                    L"/" + std::to_wstring(postGameAnalysisTotalMoves) + L"手)";
            }
            break;
        case TurnState::Reviewing:
            text = L"棋譜再生中 (手 " + std::to_wstring(reviewMoveIndex) + L"/" +
                std::to_wstring(reviewTotalMoves) + L")";
            if (reviewMoveIndex == reviewTotalMoves && !reviewResult.empty())
            {
                text += L"  結果: " + Utf8ToWide(reviewResult);
            }
            break;
        default: break;
        }
        text += L"   アゲハマ 黒:" + std::to_wstring(board.CapturesBy(Stone::Black)) +
            L" 白:" + std::to_wstring(board.CapturesBy(Stone::White));
        if (isPlacementActive)
        {
            text += L"   レーティング測定中(推定:" + std::to_wstring(std::lround(placementEstimate)) +
                L" 収束率:" + std::to_wstring(std::lround(placementConvergenceRate * 100.0)) + L"%)";
        }
        else
        {
            text += L"   レーティング:" + std::to_wstring(std::lround(userRating)) +
                L" [" + (gameMode == GameMode::Ranked ? L"レート戦" : L"カジュアル") + L"]";
        }
        text += L"   AI強さ(目安):" + std::to_wstring(std::lround(aiTargetRating)) +
            L" [" + DisplayRankTextForRankIndex(static_cast<int>(std::lround(RankIndexForRating(aiTargetRating)))) +
            L"相当]";
        return text;
    }

    // 盤の下マージンにHUDテキストを描画する
    void DrawHud(KurenaiEngine2D& renderer, const BoardLayout& layout, TurnState turnState, const GoBoard& board,
        int reviewMoveIndex, int reviewTotalMoves, const std::string& reviewResult,
        double userRating, GameMode gameMode, double aiTargetRating,
        bool isPlacementActive, double placementEstimate, double placementConvergenceRate,
        bool postGameAnalysisActive, int postGameAnalysisIndex, int postGameAnalysisTotalMoves)
    {
        const float hudX = layout.CenterX - layout.GridExtent * 0.5f;
        const float bottomMarginCenterY = (layout.CenterY - layout.BoardExtent * 0.5f) * 0.5f;
        const float hudY = bottomMarginCenterY - kHudFontSize * 0.5f;
        renderer.DrawText(hudX, hudY,
            BuildStatusText(turnState, board, reviewMoveIndex, reviewTotalMoves, reviewResult,
                userRating, gameMode, aiTargetRating, isPlacementActive, placementEstimate, placementConvergenceRate,
                postGameAnalysisActive, postGameAnalysisIndex, postGameAnalysisTotalMoves),
            kHudFontSize, kHudColorR, kHudColorG, kHudColorB, 1.0f);
    }

    // ボタン1個の識別子。着手以外の操作(パス・投了・地合い表示切替・着手ヒント表示切替・
    // 棋譜再生・新規対局・終了・対局モード/強さ選択)にそれぞれ対応する
    enum class ButtonId
    {
        ToggleTerritory,
        ToggleHint,
        Pass,
        Resign,
        StartReview,
        ReviewPrev,
        ReviewNext,
        NewGame,
        Quit,
        ChooseBoardSize9,
        ChooseBoardSize13,
        ChooseBoardSize19,
        ChooseRanked,
        ChooseCasual,
        // カジュアル対局の強さ選択(盤上のタブ+5列2行グリッド、11.6節)。CasualGroup*は
        // タブ切替(0=20〜11級/1=10〜1級/2=1〜9段)、CasualRankSlot*はグリッド内の
        // 10マス分(1〜9段タブはスロット9を使わない)
        CasualGroupKyu20To11,
        CasualGroupKyu10To1,
        CasualGroupDan1To9,
        CasualRankSlot0,
        CasualRankSlot1,
        CasualRankSlot2,
        CasualRankSlot3,
        CasualRankSlot4,
        CasualRankSlot5,
        CasualRankSlot6,
        CasualRankSlot7,
        CasualRankSlot8,
        CasualRankSlot9,
        ShowMistakeStats,
        BackFromStats,
    };

    // ボタン列(画面右側の縦列)内での配置グループ。対局状態(turnState)によって中央グループの
    // 内容・個数が変わっても列全体の重心が安定するよう、役割ごとに配置基準を分ける
    enum class ButtonGroup
    {
        Top,    // 常時表示・表示切替系(地合い表示・着手ヒント)。列の上端を起点に上詰め
        Center, // 対局状態に応じた操作。列の垂直中央を基準に中央揃え
        Bottom, // 終了。列の下端を起点に下詰め(頻繁に押す操作から離し誤クリックを防ぐ)
    };

    // 1フレーム分のボタン行を組み立てる際の仕様(ラベル・有効/無効・トグルON状態・配置グループ)
    struct ButtonSpec
    {
        ButtonId Id;
        std::wstring Label;
        bool Enabled = true;
        bool Active = false; // トグル系ボタンがON状態かどうか(背景色に反映)
        ButtonGroup Group = ButtonGroup::Center;
    };

    // ButtonSpecから求めたヒット領域(中心x, y基準)
    struct ButtonRect
    {
        ButtonId Id = ButtonId::Quit;
        float CenterX = 0.0f;
        float CenterY = 0.0f;
        float Width = 0.0f;
        float Height = 0.0f;
        bool Enabled = true;
        bool Active = false;
    };

    // DrawButtonの見た目を役割ごとに変えるための設定(フォントサイズ・角丸・通常時/ホバー時の
    // 配色)。既定値は右側縦列の通常ボタンの見た目と一致させてあるため、既存の呼び出し側は
    // このstyleを省略すればこれまでどおりの見た目になる(カジュアル対局の強さ選択、11.6節の
    // タブ・グリッドのみ専用のstyleを渡す)。無効時・トグルON時・押下時の配色は役割によらず
    // 共通(kButtonDisabledColor*・kButtonActiveColor*・kButtonPressedDarkenFactor)のまま
    struct ButtonStyle
    {
        float FontSize = kButtonFontSize;
        float CornerRadius = kButtonCornerRadius;
        float ColorR = kButtonColorR, ColorG = kButtonColorG, ColorB = kButtonColorB;
        float HoverColorR = kButtonHoverColorR, HoverColorG = kButtonHoverColorG, HoverColorB = kButtonHoverColorB;
        float BorderColorR = kButtonBorderColorR, BorderColorG = kButtonBorderColorG, BorderColorB = kButtonBorderColorB;
    };

    // ラベル文字列のおおよその描画幅を見積もる。KurenaiEngine2Dは実測用のAPIを公開していないため、
    // ASCII(半角)はfontSizeの約0.55倍、それ以外(かな漢字などの全角文字)は約1.0倍として概算する
    float EstimateTextWidth(const std::wstring& text, float fontSize)
    {
        float width = 0.0f;
        for (wchar_t ch : text)
        {
            const bool isHalfWidth = ch < 0x100;
            width += fontSize * (isHalfWidth ? 0.55f : 1.0f);
        }
        return width;
    }

    // ボタン仕様のリストを、画面右側のボタン列内でグループ別(Top=上詰め/Center=中央揃え/
    // Bottom=下詰め)に縦積みしたヒット領域のリストへ変換する。列幅いっぱいの固定幅ボタンにする
    // (ラベル文字数に応じた可変幅は縦列だと不自然なため)
    std::vector<ButtonRect> LayoutButtonColumn(const std::vector<ButtonSpec>& specs, const BoardLayout& layout,
        uint32_t windowHeight)
    {
        std::vector<ButtonRect> rects(specs.size());

        const float columnCenterX = layout.ContentWidth + kButtonColumnWidth * 0.5f;
        const float buttonWidth = kButtonColumnWidth - kButtonColumnPaddingX * 2.0f;

        size_t centerCount = 0;
        for (const ButtonSpec& spec : specs)
        {
            if (spec.Group == ButtonGroup::Center)
            {
                ++centerCount;
            }
        }
        const float centerTotalHeight = centerCount > 0
            ? static_cast<float>(centerCount) * kButtonHeight + static_cast<float>(centerCount - 1) * kButtonSpacing
            : 0.0f;

        float topCursorY = static_cast<float>(windowHeight) - kButtonColumnMarginY - kButtonHeight * 0.5f;
        float centerCursorY = static_cast<float>(windowHeight) * 0.5f + centerTotalHeight * 0.5f - kButtonHeight * 0.5f;
        float bottomCursorY = kButtonColumnMarginY + kButtonHeight * 0.5f;

        for (size_t i = 0; i < specs.size(); ++i)
        {
            const ButtonSpec& spec = specs[i];
            ButtonRect rect;
            rect.Id = spec.Id;
            rect.Width = buttonWidth;
            rect.Height = kButtonHeight;
            rect.CenterX = columnCenterX;
            rect.Enabled = spec.Enabled;
            rect.Active = spec.Active;

            switch (spec.Group)
            {
            case ButtonGroup::Top:
                rect.CenterY = topCursorY;
                topCursorY -= (kButtonHeight + kButtonSpacing);
                break;
            case ButtonGroup::Center:
                rect.CenterY = centerCursorY;
                centerCursorY -= (kButtonHeight + kButtonSpacing);
                break;
            case ButtonGroup::Bottom:
                rect.CenterY = bottomCursorY;
                bottomCursorY += (kButtonHeight + kButtonSpacing);
                break;
            }
            rects[i] = rect;
        }
        return rects;
    }

    // カジュアル対局の強さ選択(11.6節)の範囲タブ3個を、盤の上部に横一列・中央揃えで配置する
    // (グリッドとは役割が異なることを示すため、位置も盤上部に離して置く)
    //
    // タブの合計横幅(3個分+隙間)が盤の格子の一辺(layout.GridExtent、盤の一番外側の線から
    // 線までの幅。盤の大きさによらず常に同じピクセル値)から左右にkCasualOuterMarginずつ
    // 余白を残した幅にちょうど収まるよう、1個あたりの幅を逆算する(ボタンが盤の一番外側の
    // 線の内側に収まるようにするための設計。BoardExtent/GridExtent自体はboardSizeに依存せず
    // 常に同じピクセルサイズで描かれ、変わるのはLineSpacingだけのため、逆にLineSpacingに
    // 比例させて拡大すると9路・13路で盤の外へあふれてしまう。実機検証で確認済み)
    std::vector<ButtonRect> LayoutCasualGroupTabs(const std::vector<ButtonSpec>& specs, const BoardLayout& layout)
    {
        const float availableWidth = layout.GridExtent - kCasualOuterMargin * 2.0f;
        const size_t count = specs.size();
        const float gapTotal = count > 0 ? static_cast<float>(count - 1) * kCasualTabSpacing : 0.0f;
        const float buttonWidth = count > 0 ? (availableWidth - gapTotal) / static_cast<float>(count) : 0.0f;
        const float buttonHeight = kCasualTabButtonHeight;

        std::vector<ButtonRect> rects(count);
        const float originX = layout.CenterX - availableWidth * 0.5f + buttonWidth * 0.5f;
        // 盤の一番外側の線(CenterY + GridExtent/2)からkCasualOuterMargin分下げた位置に
        // タブの上端が来るように中心を置く(左右の余白と同じ考え方)
        const float centerY = layout.CenterY + layout.GridExtent * 0.5f - kCasualOuterMargin - buttonHeight * 0.5f;

        for (size_t i = 0; i < count; ++i)
        {
            ButtonRect rect;
            rect.Id = specs[i].Id;
            rect.Width = buttonWidth;
            rect.Height = buttonHeight;
            rect.CenterX = originX + static_cast<float>(i) * (buttonWidth + kCasualTabSpacing);
            rect.CenterY = centerY;
            rect.Enabled = specs[i].Enabled;
            rect.Active = specs[i].Active;
            rects[i] = rect;
        }
        return rects;
    }

    // カジュアル対局の強さ選択(11.6節)のランクボタンを、盤中央基準でkCasualGridColumns列の
    // 正方形(1:1)グリッドに配置する(1〜9段タブは9個のため最後の1マスは空けてボタンを
    // 置かない=specsの要素数がそのままボタン数になる)。ボタン1個の一辺は、5個分+隙間が
    // 盤の格子の一辺から左右にkCasualOuterMarginずつ余白を残した幅にちょうど収まるよう
    // 逆算する(理由はLayoutCasualGroupTabsのコメントを参照)
    std::vector<ButtonRect> LayoutCasualRankGrid(const std::vector<ButtonSpec>& specs, const BoardLayout& layout)
    {
        const float availableWidth = layout.GridExtent - kCasualOuterMargin * 2.0f;
        const float gapTotal = static_cast<float>(kCasualGridColumns - 1) * kCasualGridSpacing;
        const float buttonSize = (availableWidth - gapTotal) / static_cast<float>(kCasualGridColumns);
        const float spacingX = kCasualGridSpacing;
        const float spacingY = kCasualGridSpacing;

        std::vector<ButtonRect> rects(specs.size());
        const int rows = (static_cast<int>(specs.size()) + kCasualGridColumns - 1) / kCasualGridColumns;
        const float totalWidth = availableWidth;
        const float totalHeight = static_cast<float>(rows) * buttonSize +
            static_cast<float>(rows > 0 ? rows - 1 : 0) * spacingY;
        const float originX = layout.CenterX - totalWidth * 0.5f + buttonSize * 0.5f;
        const float originY = layout.CenterY + totalHeight * 0.5f - buttonSize * 0.5f; // 上段から並べる(Y-up)

        for (size_t i = 0; i < specs.size(); ++i)
        {
            const int row = static_cast<int>(i) / kCasualGridColumns;
            const int col = static_cast<int>(i) % kCasualGridColumns;
            ButtonRect rect;
            rect.Id = specs[i].Id;
            rect.Width = buttonSize;
            rect.Height = buttonSize;
            rect.CenterX = originX + static_cast<float>(col) * (buttonSize + spacingX);
            rect.CenterY = originY - static_cast<float>(row) * (buttonSize + spacingY);
            rect.Enabled = specs[i].Enabled;
            rect.Active = specs[i].Active;
            rects[i] = rect;
        }
        return rects;
    }

    // タブ番号(0/1/2)→そのタブのButtonId
    ButtonId CasualTabButtonId(int group)
    {
        switch (group)
        {
        case 0:  return ButtonId::CasualGroupKyu20To11;
        case 1:  return ButtonId::CasualGroupKyu10To1;
        default: return ButtonId::CasualGroupDan1To9;
        }
    }

    // グリッド内スロット番号(0〜9)→そのマスのButtonId
    ButtonId CasualSlotButtonId(int slot)
    {
        static constexpr ButtonId kSlotIds[] = {
            ButtonId::CasualRankSlot0, ButtonId::CasualRankSlot1, ButtonId::CasualRankSlot2,
            ButtonId::CasualRankSlot3, ButtonId::CasualRankSlot4, ButtonId::CasualRankSlot5,
            ButtonId::CasualRankSlot6, ButtonId::CasualRankSlot7, ButtonId::CasualRankSlot8,
            ButtonId::CasualRankSlot9,
        };
        return kSlotIds[slot];
    }

    bool IsPointInButton(const ButtonRect& button, float worldX, float worldY)
    {
        return worldX >= button.CenterX - button.Width * 0.5f && worldX <= button.CenterX + button.Width * 0.5f &&
            worldY >= button.CenterY - button.Height * 0.5f && worldY <= button.CenterY + button.Height * 0.5f;
    }

    // ボタン1個の背景と文字を描画する。ホバー/トグルON/無効/押下状態に応じて見た目を変える。
    // 角丸矩形+内側枠線+ドロップシャドウはKurenaiEngine2D::DrawRoundedRectで1回の描画コールずつ描く。
    // styleを省略すると右側縦列の通常ボタンの見た目になる(ButtonStyleの既定値参照)
    void DrawButton(KurenaiEngine2D& renderer, const ButtonRect& button,
        const std::wstring& label, bool isHovered, bool isPressed, const ButtonStyle& style = ButtonStyle{})
    {
        float r = style.ColorR, g = style.ColorG, b = style.ColorB;
        float borderR = style.BorderColorR, borderG = style.BorderColorG, borderB = style.BorderColorB;
        if (!button.Enabled)
        {
            r = kButtonDisabledColorR; g = kButtonDisabledColorG; b = kButtonDisabledColorB;
        }
        else if (button.Active)
        {
            r = kButtonActiveColorR; g = kButtonActiveColorG; b = kButtonActiveColorB;
            borderR = kButtonBorderActiveColorR; borderG = kButtonBorderActiveColorG; borderB = kButtonBorderActiveColorB;
        }
        else if (isHovered)
        {
            r = style.HoverColorR; g = style.HoverColorG; b = style.HoverColorB;
        }

        float centerX = button.CenterX;
        float centerY = button.CenterY;
        if (button.Enabled && isPressed)
        {
            r *= kButtonPressedDarkenFactor; g *= kButtonPressedDarkenFactor; b *= kButtonPressedDarkenFactor;
            centerY += kButtonPressedOffsetY;
        }

        // ドロップシャドウ
        renderer.DrawRoundedRect(
            button.CenterX + kButtonShadowOffsetX, button.CenterY + kButtonShadowOffsetY,
            button.Width, button.Height, style.CornerRadius,
            kButtonShadowColorR, kButtonShadowColorG, kButtonShadowColorB, kButtonShadowAlpha);

        // 本体+内側枠線
        renderer.DrawRoundedRect(
            centerX, centerY, button.Width, button.Height, style.CornerRadius,
            r, g, b, 1.0f,
            kButtonBorderThickness, borderR, borderG, borderB, 1.0f);

        const float textWidth = EstimateTextWidth(label, style.FontSize);
        const float textX = centerX - textWidth * 0.5f;
        const float textY = centerY - style.FontSize * 0.5f;
        if (button.Enabled)
        {
            renderer.DrawText(textX, textY, label, style.FontSize, kButtonTextColorR, kButtonTextColorG, kButtonTextColorB, 1.0f);
        }
        else
        {
            renderer.DrawText(textX, textY, label, style.FontSize,
                kButtonDisabledTextColorR, kButtonDisabledTextColorG, kButtonDisabledTextColorB, 1.0f);
        }
    }

    // 現在時刻から"game_YYYYMMDD_HHMMSS.sgf"形式のファイル名を組み立てる
    std::wstring BuildGameFileName(std::chrono::system_clock::time_point when)
    {
        const std::time_t time = std::chrono::system_clock::to_time_t(when);
        std::tm localTime{};
        localtime_s(&localTime, &time);

        std::wostringstream name;
        name << L"game_" << std::put_time(&localTime, L"%Y%m%d_%H%M%S") << L".sgf";
        return name.str();
    }

    // 現在時刻から"YYYYMMDD_HHMMSS"形式のタイムスタンプを組み立てる(rating_history.txtの
    // 各行の先頭に使う。BuildGameFileNameと同じput_time呼び出しを再利用し、新しい日時表記を
    // 作らない)
    std::string BuildTimestamp(std::chrono::system_clock::time_point when)
    {
        const std::time_t time = std::chrono::system_clock::to_time_t(when);
        std::tm localTime{};
        localtime_s(&localTime, &time);

        std::ostringstream name;
        name << std::put_time(&localTime, "%Y%m%d_%H%M%S");
        return name.str();
    }

    // 対局の記録をSGFへ保存する。棋譜保存は対局結果の表示を妨げない補助機能のため、
    // 失敗しても例外は投げずerror.logに記録するのみとする。戻り値は保存先パス
    // (棋譜再生で読み直すために使う)。失敗時は空のパスを返す
    std::filesystem::path SaveGameRecordSafely(const std::filesystem::path& gamesDir,
        const std::vector<SgfMove>& moves, const std::string& result, int boardSize)
    {
        try
        {
            std::filesystem::create_directories(gamesDir);

            SgfGameRecord record;
            record.BoardSize = boardSize;
            record.Komi = kKomi;
            record.Result = result;
            record.Moves = moves;

            const std::filesystem::path path = gamesDir / BuildGameFileName(std::chrono::system_clock::now());
            WriteSgfFile(path, record);
            return path;
        }
        catch (const std::exception& e)
        {
            std::ofstream log("error.log", std::ios::app);
            log << "SGFの保存に失敗しました: " << e.what() << std::endl;
            return {};
        }
    }

    // 対局終了処理をまとめる: SGF保存(モードに関わらず常に行う)に加え、レート戦の場合のみ
    // 標準Elo式(9.6節参照)でuserRatingを更新しrating_history.txtへ1行追記する。カジュアルの
    // 場合はレーティング関連の処理を丸ごとスキップする。結果文字列が未知の形式で勝敗を
    // 判定できない場合もレーティングは更新せず、error.logに記録するのみとする(存在しない
    // データを捏造しない)。opponentRatingForThisGameは今回のAIの目標強さ(=対局開始時点の
    // userRating.Rating)で、Elo更新の相手レーティングとして使う(AIは常に自分と互角の相手に
    // 調整しているため、固定の1500ではなくこの値を使う、10章参照)。isPlacementGameがtrueの
    // 場合、この関数では通常のElo更新を行わない。収束(80%)に達していれば呼び出し側が
    // この関数より前にfinalizePlacementRating()ですでにレーティングを確定させており、
    // 未収束のまま対局が終わった場合はそもそも今回はレーティングを確定させない
    // (次回もプレースメントとして再開するため)。いずれにしてもここでの二重計上・
    // 誤った確定は起きない
    std::filesystem::path FinalizeGameResult(const std::filesystem::path& gamesDir,
        const std::filesystem::path& ratingPath, const std::vector<SgfMove>& moves,
        const std::string& result, GameMode gameMode, double opponentRatingForThisGame,
        bool isPlacementGame, RatingData& userRating, int boardSize)
    {
        const std::filesystem::path savedPath = SaveGameRecordSafely(gamesDir, moves, result, boardSize);

        if (gameMode != GameMode::Ranked || isPlacementGame)
        {
            return savedPath;
        }

        double actualScore = 0.0;
        if (!TryParseBlackWinFraction(result, actualScore))
        {
            std::ofstream log("error.log", std::ios::app);
            log << "未知の対局結果文字列のためレーティングを更新しませんでした: " << result << std::endl;
            return savedPath;
        }

        userRating.Rating += ComputeEloDelta(userRating.Rating, opponentRatingForThisGame, actualScore, kEloK);
        userRating.GamesPlayed += 1;

        try
        {
            AppendRatingEntry(ratingPath, BuildTimestamp(std::chrono::system_clock::now()), userRating.Rating, result);
        }
        catch (const std::exception& e)
        {
            std::ofstream log("error.log", std::ios::app);
            log << "レーティング履歴の保存に失敗しました: " << e.what() << std::endl;
        }

        return savedPath;
    }

    // reviewMoveIndex手目までを空盤面から再生し、再生用盤面を作り直す
    GoBoard ReplayMoves(const std::vector<SgfMove>& moves, int upToIndex, int boardSize)
    {
        GoBoard replayBoard(boardSize);
        for (int i = 0; i < upToIndex; ++i)
        {
            const SgfMove& move = moves[static_cast<size_t>(i)];
            if (move.IsPass)
            {
                replayBoard.Pass();
            }
            else
            {
                replayBoard.TryPlay(move.Row, move.Col, move.Color);
            }
        }
        return replayBoard;
    }

    // 盤上部の帯の上段に、着手の言語化コメントを描く(棋譜再生中のみ)
    void DrawMoveCommentary(KurenaiEngine2D& renderer, uint32_t windowHeight, const std::wstring& commentary)
    {
        if (commentary.empty())
        {
            return;
        }
        const float textX = kGraphMarginX;
        const float textY = static_cast<float>(windowHeight) - kCommentaryHeight * 0.5f - kCommentaryFontSize * 0.5f;
        renderer.DrawText(textX, textY, commentary, kCommentaryFontSize,
            kCommentaryColorR, kCommentaryColorG, kCommentaryColorB, 1.0f);
    }

    // 損失グラフ(棋譜再生中のみ、盤上部のkGraphAreaHeight帯の下段に黒視点勝率の推移を描く)。
    // hasCached[i]がtrueの手数のみ値が有効(未解析の区間は線が途切れる)。
    // winrateCache/hasCachedのサイズは総手数+1(0手目〜総手数)
    void DrawLossGraph(KurenaiEngine2D& renderer, uint32_t windowWidth, uint32_t windowHeight,
        const std::vector<float>& winrateCache, const std::vector<bool>& hasCached, int currentIndex)
    {
        const int totalMoves = static_cast<int>(hasCached.size()) - 1;
        if (totalMoves <= 0)
        {
            return;
        }

        const float graphLeft = kGraphMarginX;
        const float graphRight = static_cast<float>(windowWidth) - kGraphMarginX;
        const float graphBottom = static_cast<float>(windowHeight) - kGraphAreaHeight + kGraphMarginBottom;
        const float graphTop = static_cast<float>(windowHeight) - kCommentaryHeight - kGraphMarginTop;
        const float graphWidth = (std::max)(1.0f, graphRight - graphLeft);
        const float graphHeight = (std::max)(1.0f, graphTop - graphBottom);

        const auto xForIndex = [&](int index) -> float
        {
            return graphLeft + graphWidth * (static_cast<float>(index) / static_cast<float>(totalMoves));
        };
        const auto yForWinrate = [&](float winrate) -> float
        {
            return graphBottom + graphHeight * (std::max)(0.0f, (std::min)(1.0f, winrate));
        };

        // 50%の目安線
        const float midY = yForWinrate(0.5f);
        renderer.DrawLine(graphLeft, midY, graphRight, midY, kGraphReferenceLineThickness,
            kGraphReferenceColorR, kGraphReferenceColorG, kGraphReferenceColorB, 1.0f);

        // キャッシュ済みの区間のみ線分で結ぶ
        for (int i = 0; i < totalMoves; ++i)
        {
            if (!hasCached[i] || !hasCached[i + 1])
            {
                continue;
            }
            renderer.DrawLine(
                xForIndex(i), yForWinrate(winrateCache[i]),
                xForIndex(i + 1), yForWinrate(winrateCache[i + 1]),
                kGraphLineThickness, kGraphLineColorR, kGraphLineColorG, kGraphLineColorB, 1.0f);
        }

        // 現在の手数の位置にマーカーを描く
        if (currentIndex >= 0 && currentIndex <= totalMoves && hasCached[static_cast<size_t>(currentIndex)])
        {
            renderer.DrawCircle(
                xForIndex(currentIndex), yForWinrate(winrateCache[static_cast<size_t>(currentIndex)]),
                kGraphMarkerRadius, kGraphMarkerColorR, kGraphMarkerColorG, kGraphMarkerColorB, 1.0f);
        }
    }

    // 苦手分野の解析結果(局面ごとの悪手率)を表示用の行に組み立てる。局面の3分割は単純な
    // 手数の割合によるものであり、囲碁理論上の厳密な布石・中盤・ヨセの境界ではない(11章参照)
    std::vector<std::wstring> BuildMistakeStatsLines(const MistakeStatsData& stats)
    {
        std::vector<std::wstring> lines;
        lines.push_back(L"苦手分野の解析(レート戦終了時に自動解析した手を集計)");
        lines.push_back(L"");

        const wchar_t* phaseLabels[3] = { L"序盤(1〜33%)", L"中盤(34〜66%)", L"終盤(67〜100%)" };
        double looseBlunderRate[3] = { -1.0, -1.0, -1.0 };

        for (int phase = 0; phase < 3; ++phase)
        {
            const int* counts = stats.Counts[phase];
            const int total = counts[0] + counts[1] + counts[2] + counts[3];

            std::wostringstream line;
            line << phaseLabels[phase] << L": ";
            if (total == 0)
            {
                line << L"データがありません";
                lines.push_back(line.str());
                continue;
            }

            const double best = 100.0 * counts[static_cast<int>(MoveQuality::Best)] / total;
            const double slight = 100.0 * counts[static_cast<int>(MoveQuality::SlightLoss)] / total;
            const double loose = 100.0 * counts[static_cast<int>(MoveQuality::Loose)] / total;
            const double blunder = 100.0 * counts[static_cast<int>(MoveQuality::Blunder)] / total;
            looseBlunderRate[phase] = loose + blunder;

            line << L"最善手級 " << std::fixed << std::setprecision(0) << best << L"% "
                 << L"やや損 " << slight << L"% "
                 << L"緩着 " << loose << L"% "
                 << L"悪手 " << blunder << L"%  (合計" << total << L"手)";
            if (total < 10)
            {
                line << L" ※データが少なめです";
            }
            lines.push_back(line.str());
        }

        lines.push_back(L"");
        int worstPhase = -1;
        for (int phase = 0; phase < 3; ++phase)
        {
            if (looseBlunderRate[phase] >= 0.0 &&
                (worstPhase < 0 || looseBlunderRate[phase] > looseBlunderRate[worstPhase]))
            {
                worstPhase = phase;
            }
        }
        if (worstPhase >= 0)
        {
            lines.push_back(std::wstring(L"もっとも「緩着+悪手」の割合が高いのは") +
                phaseLabels[worstPhase] + L"です");
        }
        else
        {
            lines.push_back(L"まだ十分なデータがありません(レート戦を対局すると自動的に集計されます)");
        }
        return lines;
    }

    // 苦手分野の解析結果を盤の代わりに表示する
    void DrawMistakeStatsScreen(KurenaiEngine2D& renderer, uint32_t windowWidth, uint32_t windowHeight,
        const MistakeStatsData& stats)
    {
        const std::vector<std::wstring> lines = BuildMistakeStatsLines(stats);
        constexpr float fontSize = 18.0f;
        constexpr float lineHeight = 30.0f;
        const float startX = static_cast<float>(windowWidth) * 0.08f;
        float y = static_cast<float>(windowHeight) - lineHeight * 2.0f;
        for (const std::wstring& line : lines)
        {
            renderer.DrawText(startX, y, line, fontSize, 0.9f, 0.9f, 0.9f, 1.0f);
            y -= lineHeight;
        }
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    // DirectXTexのWICテクスチャ読み込みがCOMに依存しているため必須(docs/KurenaiEngine.html 7章参照)
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    int exitCode = 0;
    try
    {
        const std::filesystem::path kataGoDir = ResolveAppDataPath(L"KataGo");
        // Human SLモデル(11.6節)。未配置の場合は意図した強さのアマチュア向け対局にならないため、
        // 起動直後にstd::filesystem::existsで確認し、無ければ対局自体を許可しない
        // (TurnState::HumanModelMissing、下記turnStateの初期化を参照)
        const std::filesystem::path humanModelPath = kataGoDir / L"b18c384nbt-humanv0.bin.gz";
        const std::filesystem::path soundsDir = ResolveAppDataPath(L"Assets/Sounds");
        const std::filesystem::path gamesDir = ResolveAppDataPath(L"Games");
        // レーティングは盤の大きさ(9路/13路/19路)ごとに互いに独立して記録する(強さの基準が
        // 盤の大きさで全く異なるため)。19路は既存ユーザーのrating_history.txtをそのまま使い続け、
        // 9路・13路は新規にrating_history_9.txt/rating_history_13.txtへ記録する
        const std::filesystem::path ratingPath19 = ResolveAppDataPath(L"rating_history.txt");
        const std::filesystem::path ratingPath13 = ResolveAppDataPath(L"rating_history_13.txt");
        const std::filesystem::path ratingPath9 = ResolveAppDataPath(L"rating_history_9.txt");
        const std::filesystem::path mistakeStatsPath = ResolveAppDataPath(L"mistake_stats.txt");

        KurenaiEngine2D renderer(kWindowTitle, kWindowWidth, kWindowHeight, GraphicsAPI::DX11);

        const TextureHandle whiteTexture = renderer.CreateSolidColorTexture(255, 255, 255, 255);
        const SoundHandle stonePlaceSound = renderer.LoadSound((soundsDir / L"stone_place.wav").wstring());
        const SoundHandle gameEndSound = renderer.LoadSound((soundsDir / L"game_end.wav").wstring());

        // 対局開始前(ChoosingBoardSize)に選ぶ盤の大きさ。既定値はkDefaultBoardSize(19路)で、
        // 選び直すたびにboard/reviewBoardをこの大きさで作り直す
        int currentBoardSize = kDefaultBoardSize;
        GoBoard board(currentBoardSize);
        KataGoClient kataGo;
        // Human SLモデル(11.6節)が未配置の場合、maxVisitsのみのフォールバックでは
        // 意図した強さのアマチュア向け対局にならないため、対局自体を許可しない
        const bool humanModelAvailable = std::filesystem::exists(humanModelPath);
        TurnState turnState = humanModelAvailable ? TurnState::ChoosingBoardSize : TurnState::HumanModelMissing;

        // 対局中の着手・パスの記録。対局終了時にSGFへ保存する
        std::vector<SgfMove> moveHistory;
        // 直近の対局終了時にSGFを保存したパス。GameOver中にVキーを押すとこれを読み込んで再生する
        std::filesystem::path lastSavedGamePath;

        // 棋力の数値化(レーティング)。盤の大きさごとにrating_history*.txtから現在値を復元する。
        // 対局モード(レート戦/カジュアル)・AIの強さは対局開始前(ChoosingGameMode/
        // ChoosingCasualStrength)にそのつど選ぶ(下記)
        RatingData userRating9 = LoadRating(ratingPath9);
        RatingData userRating13 = LoadRating(ratingPath13);
        RatingData userRating19 = LoadRating(ratingPath19);

        // 現在選択中の盤の大きさ(currentBoardSize)に対応するレーティングデータ/履歴ファイルを返す。
        // 対局中は盤の大きさを変えない(選び直しはChoosingBoardSizeでのみ行う)ため、
        // 呼び出しのたびに参照先を解決しても矛盾は生じない
        const auto CurrentUserRating = [&]() -> RatingData&
        {
            switch (currentBoardSize)
            {
            case 9:  return userRating9;
            case 13: return userRating13;
            default: return userRating19;
            }
        };
        const auto CurrentRatingPath = [&]() -> const std::filesystem::path&
        {
            switch (currentBoardSize)
            {
            case 9:  return ratingPath9;
            case 13: return ratingPath13;
            default: return ratingPath19;
            }
        };

        GameMode currentGameMode = GameMode::Casual;
        // カジュアル対局の強さ選択(11.6節)で現在表示中のタブ(0=20〜11級/1=10〜1級/2=1〜9段)。
        // ChoosingCasualStrengthに入るたびに現在のレーティングに応じた既定タブへ設定し直す
        int casualStrengthGroup = 0;
        // 今回の対局でAIが狙っている強さ(目安レーティング)。レート戦なら常にuserRating.Rating
        // (五分の相手)、カジュアルなら段階選択で選んだ値
        double currentAiTargetRating = kInitialRating;
        // 対局回数0のままレート戦を始めた場合のみtrue(対局開始時に固定し、対局終了まで変えない)。
        // trueの間はFinalizeGameResultで通常のElo更新をスキップする(PlacementTrackerが対局中に
        // 収束した時点ですでにレーティングを確定させているため)
        bool isCurrentGamePlacement = false;
        // 初期レーティング決定(プレースメント)の追跡状態。isCurrentGamePlacementの対局中のみ
        // Active。収束するとActiveがfalseになりHUD表示が通常のレーティング表示へ切り替わる
        PlacementTracker placementTracker;

        // プレースメント対局が収束率80%(kConvergenceRateThreshold)に到達した時点で呼ぶ。
        // 直近ウィンドウの推定値の平均を、その対局の確定レーティングとして記録する
        // (GamesPlayedが0から1になり、以降は通常のレート戦になる)。収束(80%)に達する前に
        // 対局が投了・終局した場合はこの関数を呼ばない。その場合Activeがtrueのまま残り、
        // 次回のレート戦もGamesPlayed==0によりプレースメントとして再開される(複数局に
        // またがって収束させる想定どおりの挙動)。すでに確定済み(Active==false)の場合は
        // 何もしない(念のためのガード。現状は収束経路からのみ呼ばれる)
        const auto finalizePlacementRating = [&]()
        {
            if (!placementTracker.Active)
            {
                return;
            }
            RatingData& userRating = CurrentUserRating();
            userRating.Rating = placementTracker.CurrentEstimate();
            userRating.GamesPlayed += 1;
            placementTracker.Active = false;
            try
            {
                AppendRatingEntry(CurrentRatingPath(), BuildTimestamp(std::chrono::system_clock::now()),
                    userRating.Rating, "PLACEMENT");
            }
            catch (const std::exception& e)
            {
                std::ofstream log("error.log", std::ios::app);
                log << "レーティング履歴の保存に失敗しました: " << e.what() << std::endl;
            }
        };

        // 苦手分野の解析(局面ごとの悪手率)。mistake_stats.txtから現在の集計を復元する。
        // レート戦の対局が終了するたびに、その対局の全手を自動解析して集計する(下記)
        MistakeStatsData mistakeStats = LoadMistakeStats(mistakeStatsPath);
        // 対局終了後の自動解析の状態。postGameAnalysisActiveの間は新規対局・棋譜再生を
        // ブロックする(KataGoの解析チャンネルを他の経路と競合させないため)
        bool postGameAnalysisActive = false;
        std::vector<SgfMove> postGameAnalysisMoves; // 解析対象(終了した対局のmoveHistoryのコピー)
        std::string postGameAnalysisSgfFileName;    // 集計の重複排除キーに使うSGFファイル名
        int postGameAnalysisIndex = 0;              // 次に要求する手数
        std::vector<float> postGameWinrateCache;    // サイズ = 総手数+1(黒視点)
        std::vector<bool> postGameHasCached;
        std::vector<int> postGameBestMoveRowCache;  // その局面での最善候補手(着手の言語化と同様)
        std::vector<int> postGameBestMoveColCache;
        bool postGameAnalysisRequestPending = false; // 現在解析要求中かどうか
        // 「苦手分野」ボタンで ViewingMistakeStats に入る前の状態(GameOver/Reviewing)。
        // 「戻る」ボタンでここへ戻す
        TurnState statsReturnState = TurnState::GameOver;

        // 棋譜再生(Reviewing)の状態
        SgfGameRecord reviewRecord;
        int reviewMoveIndex = 0;
        GoBoard reviewBoard(kDefaultBoardSize);

        // 棋譜再生中の局面ごとの解析結果キャッシュ(黒視点に変換済み)。要素数は総手数+1
        // (0手目〜総手数)。reviewHasCached[i]がtrueの手数のみ有効な値を持つ。
        // reviewBestMoveRow/ColCacheはその局面での最善候補手(Order==0)の座標(未取得時は-1)で、
        // 着手の言語化(実際の着手と比較する)に使う
        std::vector<float> reviewWinrateCache;
        std::vector<float> reviewScoreLeadCache;
        std::vector<int> reviewBestMoveRowCache;
        std::vector<int> reviewBestMoveColCache;
        std::vector<bool> reviewHasCached;
        // 解析要求中のreviewMoveIndex(-1なら要求なし)。KataGoClientは1件ずつしか処理しない
        // 設計のため、前の解析が終わるまで次のreset/replayは送らない(描画ループを止めないため)
        int reviewAnalysisPendingIndex = -1;

        // 解析(kata-analyze)の最新結果。対局中の勝率表示・地合い可視化・着手ヒントが共通で使う
        KataGoAnalysisResult latestAnalysis;
        bool hasAnalysis = false;
        // TryGetAnalysisResultは同じ結果を読み出すたびtrueを返し続ける(呼び出し側で状態を
        // 消費する設計ではない)ため、PlacementTrackerへは1手番につき1回だけサンプルを渡すよう
        // この手番でサンプル済みかどうかをここで別途追跡する(enterHumanToMoveでfalseに戻す)
        bool placementSampledForCurrentTurn = false;

        // HumanToMoveへ遷移すると同時に、その局面の解析(黒=人間視点)を要求する
        const auto enterHumanToMove = [&]()
        {
            turnState = TurnState::HumanToMove;
            hasAnalysis = false;
            placementSampledForCurrentTurn = false;
            kataGo.RequestAnalysis(Stone::Black);
        };

        // targetIndex手目の局面をKataGoに解析させる(未キャッシュかつ現在解析要求中でない場合のみ)。
        // KataGoの盤面をclear_boardしてから0手目〜targetIndex手目の直前まで再生し直し、
        // その局面の解析を要求する。要求を送った場合はtrueを返す
        const auto requestReviewAnalysisFor = [&](int targetIndex) -> bool
        {
            if (reviewAnalysisPendingIndex != -1)
            {
                return false;
            }
            if (targetIndex < 0 || targetIndex >= static_cast<int>(reviewHasCached.size()) ||
                reviewHasCached[static_cast<size_t>(targetIndex)])
            {
                return false;
            }

            kataGo.ResetBoard();
            for (int i = 0; i < targetIndex; ++i)
            {
                const SgfMove& move = reviewRecord.Moves[static_cast<size_t>(i)];
                if (move.IsPass)
                {
                    kataGo.PlayPass(move.Color);
                }
                else
                {
                    kataGo.PlayMove(move.Color, move.Row, move.Col);
                }
            }

            const Stone colorToMove = (targetIndex == 0)
                ? Stone::Black
                : Opponent(reviewRecord.Moves[static_cast<size_t>(targetIndex - 1)].Color);
            kataGo.RequestAnalysis(colorToMove);
            reviewAnalysisPendingIndex = targetIndex;
            return true;
        };

        // 現在の局面の解析を優先し、手が空いたら着手の言語化に使う1手前の局面も解析する
        const auto triggerReviewAnalysisIfNeeded = [&]()
        {
            if (!requestReviewAnalysisFor(reviewMoveIndex) && reviewMoveIndex > 0)
            {
                requestReviewAnalysisFor(reviewMoveIndex - 1);
            }
        };

        // 対局終了後(GameOver)または棋譜再生中(Reviewing)、あるいはアプリ起動直後に呼ぶ。
        // 盤面・着手履歴をすべて空の状態に戻し、モード選択(ChoosingGameMode)からやり直す
        // (何度でも打ち直せるようにする。初回起動時と新規対局時のセットアップを統合している)
        const auto startNewGame = [&]()
        {
            moveHistory.clear();
            // 新規対局のたびに盤の大きさから選び直す(前回選んだ大きさに応じてboardは
            // ChooseBoardSize9/13/19のボタン処理で作り直される)
            turnState = TurnState::ChoosingBoardSize;
        };

        // レート戦/カジュアルの強さがすべて決まった後に呼ぶ。目標レーティングからHuman SLの
        // 段級位プロファイルを求め、KataGoを起動する(対局ごとに強さを変えるため、対局開始の
        // たびにStartAsyncを呼び直す。KataGoClient::StartAsyncは実行中のプロセスがあれば
        // 終了させてから作り直すため、同一インスタンスを安全に再利用できる)。
        // Human SLモデル未配置時はturnState初期化時点でHumanModelMissingへ止めており
        // このラムダ自体が呼ばれないため、ここではhumanModelAvailable==true前提で良い
        const auto beginGameWithTargetRating = [&](double targetRating)
        {
            // 上限なしのレーティングをそのまま表示・保存に使う(11.6節。段級位表示・
            // Human SLプロファイルの範囲クランプはそれぞれの変換関数側で行うため、ここでは
            // クランプしない)
            currentAiTargetRating = targetRating;
            isCurrentGamePlacement = kPlacementModeEnabled &&
                (currentGameMode == GameMode::Ranked && CurrentUserRating().GamesPlayed == 0);
            // 前回の対局が収束(80%)に達しないまま終わり、今回も同じ盤の大きさでプレースメントを
            // 続ける場合は、PlacementTrackerの状態(EMA・サンプル)をリセットせず引き継ぐ
            // (でないと収束率が対局のたびに0%へ戻ってしまう)。それ以外(プレースメントで
            // なくなった/盤の大きさが変わった/今回が新規にプレースメントを始める場合)はリセットする
            const bool resumePlacement = isCurrentGamePlacement &&
                placementTracker.Active && placementTracker.BoardSize == currentBoardSize;
            if (!resumePlacement)
            {
                placementTracker.Reset(isCurrentGamePlacement, currentBoardSize);
            }

            const int rankIndex = static_cast<int>(std::lround(RankIndexForRating(currentAiTargetRating)));
            const std::string humanSLProfile = "rank_" + HumanSLProfileSuffixForRankIndex(rankIndex);
            const int maxVisits = kHumanSLMaxVisits;

            kataGo.StartAsync(
                kataGoDir / L"katago.exe",
                kataGoDir / L"model.bin.gz",
                humanModelPath, humanSLProfile,
                kataGoDir / L"gtp.cfg",
                kataGoDir / L"katago_stderr.log",
                currentBoardSize, maxVisits);
            turnState = TurnState::EngineStarting;
        };

        // レート戦の対局が終了した直後に呼ぶ。終了した対局の全手を自動解析するため、
        // moveHistoryのコピーを保持して対局後解析の状態を初期化する(実際の解析要求は
        // 毎フレームのadvancePostGameAnalysisIfNeededが行う)。手が無い対局(開始直後の投了等)
        // では何もしない
        const auto beginPostGameAnalysis = [&](const std::filesystem::path& savedPath)
        {
            if (moveHistory.empty())
            {
                return;
            }
            postGameAnalysisMoves = moveHistory;
            postGameAnalysisSgfFileName = savedPath.filename().string();
            postGameAnalysisIndex = 0;
            postGameWinrateCache.assign(postGameAnalysisMoves.size() + 1, 0.5f);
            postGameHasCached.assign(postGameAnalysisMoves.size() + 1, false);
            postGameBestMoveRowCache.assign(postGameAnalysisMoves.size() + 1, -1);
            postGameBestMoveColCache.assign(postGameAnalysisMoves.size() + 1, -1);
            postGameAnalysisRequestPending = false;
            postGameAnalysisActive = true;
        };

        // postGameAnalysisActiveの間、解析要求中でなければ次の手数の解析を要求する
        // (requestReviewAnalysisForと同じ手順: ResetBoardしてから0手目から再生し直す)
        const auto advancePostGameAnalysisIfNeeded = [&]()
        {
            if (!postGameAnalysisActive || postGameAnalysisRequestPending)
            {
                return;
            }
            if (postGameAnalysisIndex > static_cast<int>(postGameAnalysisMoves.size()))
            {
                postGameAnalysisActive = false;
                return;
            }

            // 対局後の自動解析は補助機能のため、KataGoとの通信で何か問題が起きても
            // (新規対局・棋譜再生をブロックしたまま止まってしまわないよう)ここで打ち切り、
            // error.logに記録するのみとする
            try
            {
                kataGo.ResetBoard();
                for (int i = 0; i < postGameAnalysisIndex; ++i)
                {
                    const SgfMove& move = postGameAnalysisMoves[static_cast<size_t>(i)];
                    if (move.IsPass)
                    {
                        kataGo.PlayPass(move.Color);
                    }
                    else
                    {
                        kataGo.PlayMove(move.Color, move.Row, move.Col);
                    }
                }
                const Stone colorToMove = (postGameAnalysisIndex == 0)
                    ? Stone::Black
                    : Opponent(postGameAnalysisMoves[static_cast<size_t>(postGameAnalysisIndex - 1)].Color);
                kataGo.RequestAnalysis(colorToMove);
                postGameAnalysisRequestPending = true;
            }
            catch (const std::exception& e)
            {
                std::ofstream log("error.log", std::ios::app);
                log << "対局後の自動解析を中断しました: " << e.what() << std::endl;
                postGameAnalysisActive = false;
            }
        };

        bool territoryOverlayEnabled = false;
        bool hintOverlayEnabled = false;

        while (!renderer.ShouldClose())
        {
            renderer.PumpEvents();

            if (renderer.WasKeyPressed(VK_ESCAPE))
            {
                renderer.Close();
            }

            const uint32_t width = renderer.GetWidth();
            const uint32_t height = renderer.GetHeight();
            if (width == 0 || height == 0)
            {
                // 最小化中などはスキップ
                continue;
            }

            // 描画・当たり判定の基準となる盤の目の数。棋譜再生中はその棋譜の記録上の大きさ
            // (reviewRecord.BoardSize)、それ以外は現在の対局の盤(board.Size())を使う
            const int activeBoardSize = (turnState == TurnState::Reviewing) ? reviewRecord.BoardSize : board.Size();
            const BoardLayout layout = ComputeBoardLayout(width, height, activeBoardSize);

            // マウス位置をワールド座標(原点は画面左下、Y-up)へ変換する。GetClientMousePosition()は
            // Win32標準のクライアント座標(原点は左上、Y-down)を返すため、Yを反転する
            const bool mouseInWindow = renderer.IsMouseOverWindow();
            float mouseWorldX = 0.0f;
            float mouseWorldY = 0.0f;
            if (mouseInWindow)
            {
                const POINT cursor = renderer.GetClientMousePosition();
                mouseWorldX = static_cast<float>(cursor.x);
                mouseWorldY = static_cast<float>(height) - static_cast<float>(cursor.y);
            }

            int hoverRow = -1;
            int hoverCol = -1;
            const bool isHovering = mouseInWindow &&
                TryGetHoveredIntersection(layout, mouseWorldX, mouseWorldY, activeBoardSize, hoverRow, hoverCol) &&
                board.At(hoverRow, hoverCol) == Stone::Empty;

            const bool clicked = renderer.WasMouseButtonPressed(MouseButton::Left);
            bool passPressed = renderer.WasKeyPressed('P');
            bool resignPressed = renderer.WasKeyPressed('R');
            bool reviewStartPressed = renderer.WasKeyPressed('V');
            bool reviewPrevPressed = renderer.WasKeyPressed(VK_LEFT);
            bool reviewNextPressed = renderer.WasKeyPressed(VK_RIGHT);
            bool newGamePressed = renderer.WasKeyPressed('N');

            if (renderer.WasKeyPressed('T'))
            {
                territoryOverlayEnabled = !territoryOverlayEnabled;
            }

            if (renderer.WasKeyPressed('H'))
            {
                hintOverlayEnabled = !hintOverlayEnabled;
            }

            // 着手以外の操作(パス・投了・地合い表示切替・着手ヒント表示切替・棋譜再生・新規対局・
            // 終了・対局モード/強さ選択)を行うボタン行。表示内容は対局の進行状態(turnState)に
            // 応じて変える
            std::vector<ButtonSpec> buttonSpecs;
            // Human SLモデル未配置時は対局できないため、「終了」以外のボタンを一切出さない
            if (turnState == TurnState::HumanModelMissing)
            {
            }
            else if (turnState == TurnState::ChoosingBoardSize)
            {
                buttonSpecs.push_back({ ButtonId::ChooseBoardSize9, L"9路", true, false });
                buttonSpecs.push_back({ ButtonId::ChooseBoardSize13, L"13路", true, false });
                buttonSpecs.push_back({ ButtonId::ChooseBoardSize19, L"19路", true, false });
            }
            else if (turnState == TurnState::ChoosingGameMode)
            {
                buttonSpecs.push_back({ ButtonId::ChooseRanked, L"レート戦", true, false });
                buttonSpecs.push_back({ ButtonId::ChooseCasual, L"カジュアル", true, false });
            }
            // ChoosingCasualStrengthの間は右側縦列にボタンを出さない(強さ選択は下記の
            // 盤中央のタブ+グリッドで行う、11.6節参照)。Quitボタンのみ以下で共通追加される
            else if (turnState == TurnState::ChoosingCasualStrength)
            {
            }
            else if (turnState == TurnState::ViewingMistakeStats)
            {
                buttonSpecs.push_back({ ButtonId::BackFromStats, L"戻る", true, false });
            }
            else
            {
                buttonSpecs.push_back({ ButtonId::ToggleTerritory, L"地合い表示", true, territoryOverlayEnabled, ButtonGroup::Top });
                buttonSpecs.push_back({ ButtonId::ToggleHint, L"着手ヒント", true, hintOverlayEnabled, ButtonGroup::Top });
                if (turnState == TurnState::HumanToMove)
                {
                    buttonSpecs.push_back({ ButtonId::Pass, L"パス", true, false });
                    buttonSpecs.push_back({ ButtonId::Resign, L"投了", true, false });
                }
                if (turnState == TurnState::GameOver && !lastSavedGamePath.empty())
                {
                    // 対局後の自動解析中(11章参照)は棋譜再生を開始できない
                    // (KataGoの解析チャンネルを取り合わないため)
                    buttonSpecs.push_back({ ButtonId::StartReview, L"棋譜再生", !postGameAnalysisActive, false });
                }
                if (turnState == TurnState::Reviewing)
                {
                    const bool canGoPrev = reviewMoveIndex > 0;
                    const bool canGoNext = reviewMoveIndex < static_cast<int>(reviewRecord.Moves.size());
                    buttonSpecs.push_back({ ButtonId::ReviewPrev, L"前の手", canGoPrev, false });
                    buttonSpecs.push_back({ ButtonId::ReviewNext, L"次の手", canGoNext, false });
                }
                if (turnState == TurnState::GameOver || turnState == TurnState::Reviewing)
                {
                    // 対局終了後・棋譜再生中はいつでも新規対局を開始できる(何度でも打ち直せるようにする)。
                    // 対局後の自動解析中は同じ理由でブロックする
                    buttonSpecs.push_back({ ButtonId::NewGame, L"新規対局", !postGameAnalysisActive, false });
                    buttonSpecs.push_back({ ButtonId::ShowMistakeStats, L"苦手分野", true, false });
                }
            }
            buttonSpecs.push_back({ ButtonId::Quit, L"終了", true, false, ButtonGroup::Bottom });

            const std::vector<ButtonRect> buttonRects = LayoutButtonColumn(buttonSpecs, layout, height);

            // カジュアル対局の強さ選択(11.6節)。盤中央に3個の範囲タブと、現在のタブに応じた
            // 段級位ボタンのグリッド(20〜11級・10〜1級は10個、1〜9段は9個)を表示する
            std::vector<ButtonSpec> casualTabSpecs;
            std::vector<ButtonSpec> casualSlotSpecs;
            if (turnState == TurnState::ChoosingCasualStrength)
            {
                static const wchar_t* kTabLabels[3] = { L"20〜11級", L"10〜1級", L"1〜9段" };
                for (int group = 0; group < 3; ++group)
                {
                    casualTabSpecs.push_back({ CasualTabButtonId(group), kTabLabels[group],
                        true, casualStrengthGroup == group });
                }

                const int slotCount = (casualStrengthGroup == 2) ? 9 : 10;
                for (int slot = 0; slot < slotCount; ++slot)
                {
                    const int rankIndex = RankIndexForCasualSlot(casualStrengthGroup, slot);
                    casualSlotSpecs.push_back({ CasualSlotButtonId(slot),
                        DisplayRankTextForRankIndex(rankIndex), true, false });
                }
            }
            const std::vector<ButtonRect> casualTabRects = LayoutCasualGroupTabs(casualTabSpecs, layout);
            const std::vector<ButtonRect> casualSlotRects = LayoutCasualRankGrid(casualSlotSpecs, layout);
            const ButtonStyle casualTabStyle{
                kCasualTabFontSize, kCasualTabButtonHeight * 0.5f,
                kCasualTabColorR, kCasualTabColorG, kCasualTabColorB,
                kCasualTabHoverColorR, kCasualTabHoverColorG, kCasualTabHoverColorB,
                kCasualTabBorderColorR, kCasualTabBorderColorG, kCasualTabBorderColorB
            };
            const ButtonStyle casualGridStyle{ kCasualGridFontSize, kCasualGridCornerRadius };

            if (mouseInWindow && clicked && turnState == TurnState::ChoosingCasualStrength)
            {
                for (const ButtonRect& button : casualTabRects)
                {
                    if (button.Enabled && IsPointInButton(button, mouseWorldX, mouseWorldY))
                    {
                        if (button.Id == ButtonId::CasualGroupKyu20To11)      casualStrengthGroup = 0;
                        else if (button.Id == ButtonId::CasualGroupKyu10To1) casualStrengthGroup = 1;
                        else if (button.Id == ButtonId::CasualGroupDan1To9) casualStrengthGroup = 2;
                        break;
                    }
                }
                for (size_t i = 0; i < casualSlotRects.size(); ++i)
                {
                    const ButtonRect& button = casualSlotRects[i];
                    if (button.Enabled && IsPointInButton(button, mouseWorldX, mouseWorldY))
                    {
                        const int rankIndex = RankIndexForCasualSlot(casualStrengthGroup, static_cast<int>(i));
                        beginGameWithTargetRating(RatingForRankIndex(static_cast<double>(rankIndex)));
                        break;
                    }
                }
            }

            if (mouseInWindow && clicked)
            {
                for (const ButtonRect& button : buttonRects)
                {
                    if (!button.Enabled || !IsPointInButton(button, mouseWorldX, mouseWorldY))
                    {
                        continue;
                    }
                    switch (button.Id)
                    {
                    case ButtonId::ToggleTerritory: territoryOverlayEnabled = !territoryOverlayEnabled; break;
                    case ButtonId::ToggleHint:      hintOverlayEnabled = !hintOverlayEnabled; break;
                    case ButtonId::Pass:            passPressed = true; break;
                    case ButtonId::Resign:          resignPressed = true; break;
                    case ButtonId::StartReview:     reviewStartPressed = true; break;
                    case ButtonId::ReviewPrev:      reviewPrevPressed = true; break;
                    case ButtonId::ReviewNext:      reviewNextPressed = true; break;
                    case ButtonId::NewGame:         newGamePressed = true; break;
                    case ButtonId::Quit:            renderer.Close(); break;
                    case ButtonId::ChooseBoardSize9:
                        currentBoardSize = 9;
                        board = GoBoard(currentBoardSize);
                        turnState = TurnState::ChoosingGameMode;
                        break;
                    case ButtonId::ChooseBoardSize13:
                        currentBoardSize = 13;
                        board = GoBoard(currentBoardSize);
                        turnState = TurnState::ChoosingGameMode;
                        break;
                    case ButtonId::ChooseBoardSize19:
                        currentBoardSize = 19;
                        board = GoBoard(currentBoardSize);
                        turnState = TurnState::ChoosingGameMode;
                        break;
                    case ButtonId::ChooseRanked:
                        currentGameMode = GameMode::Ranked;
                        beginGameWithTargetRating(CurrentUserRating().Rating);
                        break;
                    case ButtonId::ChooseCasual:
                        currentGameMode = GameMode::Casual;
                        // 現在のレーティングが属する範囲のタブを既定選択にする(11.6節)
                        casualStrengthGroup = CasualGroupForRating(CurrentUserRating().Rating);
                        turnState = TurnState::ChoosingCasualStrength;
                        break;
                    case ButtonId::ShowMistakeStats:
                        statsReturnState = turnState;
                        turnState = TurnState::ViewingMistakeStats;
                        break;
                    case ButtonId::BackFromStats:
                        turnState = statsReturnState;
                        break;
                    }
                    break; // ボタンは重ならない配置のため、1個ヒットしたら以降は調べない
                }
            }

            // 解析結果のポーリング。要求元(通常の対局中/棋譜再生中)に応じて振り分ける
            KataGoAnalysisResult polledAnalysis;
            if (kataGo.TryGetAnalysisResult(polledAnalysis))
            {
                if (reviewAnalysisPendingIndex != -1)
                {
                    if (!polledAnalysis.Failed &&
                        reviewAnalysisPendingIndex < static_cast<int>(reviewHasCached.size()))
                    {
                        const size_t index = static_cast<size_t>(reviewAnalysisPendingIndex);
                        reviewWinrateCache[index] = ToBlackWinrate(polledAnalysis);
                        reviewScoreLeadCache[index] = ToBlackScoreLead(polledAnalysis);

                        int bestRow = -1;
                        int bestCol = -1;
                        for (const AnalysisMoveInfo& move : polledAnalysis.TopMoves)
                        {
                            if (move.Order == 0)
                            {
                                bestRow = move.Row;
                                bestCol = move.Col;
                                break;
                            }
                        }
                        reviewBestMoveRowCache[index] = bestRow;
                        reviewBestMoveColCache[index] = bestCol;

                        reviewHasCached[index] = true;
                    }
                    reviewAnalysisPendingIndex = -1;
                }
                else if (postGameAnalysisRequestPending)
                {
                    // レート戦終局後の自動解析結果を受け取る(11章参照)。着手前後の勝率が
                    // 両方揃った時点でその着手を分類し、初めて見る手数のみ苦手分野の統計へ加算する
                    if (!polledAnalysis.Failed &&
                        postGameAnalysisIndex < static_cast<int>(postGameHasCached.size()))
                    {
                        const size_t index = static_cast<size_t>(postGameAnalysisIndex);
                        postGameWinrateCache[index] = ToBlackWinrate(polledAnalysis);
                        postGameHasCached[index] = true;

                        int bestRow = -1;
                        int bestCol = -1;
                        for (const AnalysisMoveInfo& move : polledAnalysis.TopMoves)
                        {
                            if (move.Order == 0)
                            {
                                bestRow = move.Row;
                                bestCol = move.Col;
                                break;
                            }
                        }
                        postGameBestMoveRowCache[index] = bestRow;
                        postGameBestMoveColCache[index] = bestCol;

                        if (index >= 1 && postGameHasCached[index - 1])
                        {
                            const SgfMove& playedMove = postGameAnalysisMoves[index - 1];
                            const Stone mover = playedMove.Color;
                            const float winrateBefore = (mover == Stone::Black)
                                ? postGameWinrateCache[index - 1] : 1.0f - postGameWinrateCache[index - 1];
                            const float winrateAfter = (mover == Stone::Black)
                                ? postGameWinrateCache[index] : 1.0f - postGameWinrateCache[index];
                            const float deltaPercent = (winrateAfter - winrateBefore) * 100.0f;
                            const bool isBestMove = !playedMove.IsPass &&
                                postGameBestMoveRowCache[index - 1] == playedMove.Row &&
                                postGameBestMoveColCache[index - 1] == playedMove.Col;

                            const MoveQuality quality = ClassifyMoveQuality(deltaPercent, isBestMove);
                            const GamePhase phase = DeterminePhase(
                                static_cast<int>(index), static_cast<int>(postGameAnalysisMoves.size()));
                            const std::string dedupKey = postGameAnalysisSgfFileName + ":" + std::to_string(index);
                            if (mistakeStats.SeenKeys.count(dedupKey) == 0)
                            {
                                mistakeStats.Counts[static_cast<int>(phase)][static_cast<int>(quality)] += 1;
                                mistakeStats.SeenKeys.insert(dedupKey);
                                try
                                {
                                    AppendMistakeEntry(mistakeStatsPath, postGameAnalysisSgfFileName,
                                        static_cast<int>(index), phase, quality);
                                }
                                catch (const std::exception& e)
                                {
                                    std::ofstream log("error.log", std::ios::app);
                                    log << "苦手分野の統計の保存に失敗しました: " << e.what() << std::endl;
                                }
                            }
                        }
                    }
                    postGameAnalysisRequestPending = false;
                    ++postGameAnalysisIndex;
                }
                else
                {
                    latestAnalysis = polledAnalysis;
                    hasAnalysis = !latestAnalysis.Failed;

                    // 初期レーティング決定(プレースメント)モード中は、黒(人間)の手番ごとの
                    // 勝率を追跡する(10.4節参照)。TryGetAnalysisResultは同じ結果を読み出す
                    // たびtrueを返し続けるため、この手番でまだサンプリングしていない場合のみ
                    // 実行する。ある程度収束したと判定されたら、対局の自然な終了を待たず
                    // その場でレーティングを確定し対局を終了する(この時点でturnStateは
                    // 必ずHumanToMove。RequestAnalysis(Stone::Black)はenterHumanToMoveでのみ
                    // 呼んでいるため)
                    if (hasAnalysis && placementTracker.Active && !placementSampledForCurrentTurn)
                    {
                        placementSampledForCurrentTurn = true;
                        const bool converged = placementTracker.Update(latestAnalysis.WinrateForColorToMove);
                        if (converged)
                        {
                            finalizePlacementRating();
                            renderer.PlaySound(gameEndSound);
                            lastSavedGamePath = FinalizeGameResult(gamesDir, CurrentRatingPath(), moveHistory, "Void",
                                currentGameMode, currentAiTargetRating, isCurrentGamePlacement, CurrentUserRating(), currentBoardSize);
                            if (currentGameMode == GameMode::Ranked)
                            {
                                beginPostGameAnalysis(lastSavedGamePath);
                            }
                            const std::string message =
                                "初期レーティングの推定が収束したため、対局を終了します。\n確定レーティング: " +
                                std::to_string(std::lround(CurrentUserRating().Rating));
                            ShowMessageBoxUtf8(renderer.GetWindowHandle(), message,
                                "KurenaiGo - 初期レーティング確定", MB_OK | MB_ICONINFORMATION);
                            turnState = TurnState::GameOver;
                        }
                    }
                }
            }

            advancePostGameAnalysisIfNeeded();

            switch (turnState)
            {
            case TurnState::ChoosingGameMode:
            case TurnState::ChoosingCasualStrength:
            case TurnState::ViewingMistakeStats:
                break; // ボタン選択のみで、ここでの追加処理は無い

            case TurnState::EngineStarting:
                if (kataGo.IsStartupComplete())
                {
                    if (kataGo.StartupFailed())
                    {
                        throw std::runtime_error("KataGoの起動に失敗しました: " + kataGo.LastError());
                    }
                    enterHumanToMove();
                }
                break;

            case TurnState::HumanToMove:
                if (resignPressed)
                {
                    renderer.PlaySound(gameEndSound);
                    lastSavedGamePath = FinalizeGameResult(gamesDir, CurrentRatingPath(), moveHistory, "W+R", currentGameMode, currentAiTargetRating, isCurrentGamePlacement, CurrentUserRating(), currentBoardSize);
                    if (currentGameMode == GameMode::Ranked)
                    {
                        beginPostGameAnalysis(lastSavedGamePath);
                    }
                    ShowMessageBoxUtf8(renderer.GetWindowHandle(), "投了しました。KataGoの勝ちです。", "KurenaiGo", MB_OK | MB_ICONINFORMATION);
                    turnState = TurnState::GameOver;
                }
                else if (passPressed)
                {
                    board.Pass();
                    kataGo.PlayPass(Stone::Black);
                    moveHistory.push_back({ Stone::Black, true, -1, -1 });
                    if (board.ConsecutivePasses() >= 2)
                    {
                        kataGo.RequestFinalScore();
                        turnState = TurnState::WaitingForScore;
                    }
                    else
                    {
                        kataGo.RequestGenMove(Stone::White);
                        turnState = TurnState::AIThinking;
                    }
                }
                else if (clicked && isHovering)
                {
                    if (board.TryPlay(hoverRow, hoverCol, Stone::Black))
                    {
                        renderer.PlaySound(stonePlaceSound);
                        kataGo.PlayMove(Stone::Black, hoverRow, hoverCol);
                        moveHistory.push_back({ Stone::Black, false, hoverRow, hoverCol });
                        kataGo.RequestGenMove(Stone::White);
                        turnState = TurnState::AIThinking;
                    }
                    // 非合法手(コウ等)の場合は無視して手番を継続する
                }
                break;

            case TurnState::AIThinking:
            {
                KataGoMoveResult result;
                if (kataGo.TryGetGenMoveResult(result))
                {
                    if (result.Failed)
                    {
                        const std::string message = "KataGoとの通信でエラーが発生しました:\n" + kataGo.LastError();
                        ShowMessageBoxUtf8(renderer.GetWindowHandle(), message, "KurenaiGo - KataGoエラー", MB_OK | MB_ICONERROR);
                        turnState = TurnState::GameOver;
                    }
                    else if (result.IsResign)
                    {
                        renderer.PlaySound(gameEndSound);
                        lastSavedGamePath = FinalizeGameResult(gamesDir, CurrentRatingPath(), moveHistory, "B+R", currentGameMode, currentAiTargetRating, isCurrentGamePlacement, CurrentUserRating(), currentBoardSize);
                        if (currentGameMode == GameMode::Ranked)
                        {
                            beginPostGameAnalysis(lastSavedGamePath);
                        }
                        ShowMessageBoxUtf8(renderer.GetWindowHandle(), "KataGoが投了しました。あなたの勝ちです。", "KurenaiGo", MB_OK | MB_ICONINFORMATION);
                        turnState = TurnState::GameOver;
                    }
                    else if (result.IsPass)
                    {
                        board.Pass();
                        moveHistory.push_back({ Stone::White, true, -1, -1 });
                        if (board.ConsecutivePasses() >= 2)
                        {
                            kataGo.RequestFinalScore();
                            turnState = TurnState::WaitingForScore;
                        }
                        else
                        {
                            enterHumanToMove();
                        }
                    }
                    else if (!board.TryPlay(result.Row, result.Col, Stone::White))
                    {
                        // KataGoは常に合法手を返す前提のため、ここに来るのは想定外の異常事態
                        ShowMessageBoxUtf8(renderer.GetWindowHandle(), "KataGoの着手を反映できませんでした(想定外の座標)。",
                            "KurenaiGo - エラー", MB_OK | MB_ICONERROR);
                        turnState = TurnState::GameOver;
                    }
                    else
                    {
                        renderer.PlaySound(stonePlaceSound);
                        moveHistory.push_back({ Stone::White, false, result.Row, result.Col });
                        enterHumanToMove();
                    }
                }
                break;
            }

            case TurnState::WaitingForScore:
            {
                std::string score;
                if (kataGo.TryGetFinalScore(score))
                {
                    renderer.PlaySound(gameEndSound);
                    lastSavedGamePath = FinalizeGameResult(gamesDir, CurrentRatingPath(), moveHistory, score, currentGameMode, currentAiTargetRating, isCurrentGamePlacement, CurrentUserRating(), currentBoardSize);
                    if (currentGameMode == GameMode::Ranked)
                    {
                        beginPostGameAnalysis(lastSavedGamePath);
                    }
                    const std::string message = "対局終了\n結果: " + score;
                    ShowMessageBoxUtf8(renderer.GetWindowHandle(), message, "KurenaiGo - 対局終了", MB_OK | MB_ICONINFORMATION);
                    turnState = TurnState::GameOver;
                }
                break;
            }

            case TurnState::GameOver:
                // 対局後の自動解析中(11章参照)は新規対局・棋譜再生を受け付けない
                // (KataGoの解析チャンネルを取り合わないようにするため)
                if (newGamePressed && !postGameAnalysisActive)
                {
                    startNewGame();
                }
                else if (reviewStartPressed && !postGameAnalysisActive && !lastSavedGamePath.empty())
                {
                    try
                    {
                        reviewRecord = ReadSgfFile(lastSavedGamePath);
                        reviewMoveIndex = 0;
                        reviewBoard = GoBoard(reviewRecord.BoardSize);
                        hasAnalysis = false; // 直前の対局の解析結果を棋譜再生画面に持ち越さない
                        reviewWinrateCache.assign(reviewRecord.Moves.size() + 1, 0.5f);
                        reviewScoreLeadCache.assign(reviewRecord.Moves.size() + 1, 0.0f);
                        reviewBestMoveRowCache.assign(reviewRecord.Moves.size() + 1, -1);
                        reviewBestMoveColCache.assign(reviewRecord.Moves.size() + 1, -1);
                        reviewHasCached.assign(reviewRecord.Moves.size() + 1, false);
                        reviewAnalysisPendingIndex = -1;
                        turnState = TurnState::Reviewing;
                        triggerReviewAnalysisIfNeeded(); // 0手目の解析をすぐに開始する
                    }
                    catch (const std::exception& e)
                    {
                        std::ofstream log("error.log", std::ios::app);
                        log << "棋譜の読み込みに失敗しました: " << e.what() << std::endl;
                    }
                }
                break;

            case TurnState::Reviewing:
            {
                // reviewAnalysisPendingIndex != -1(解析要求中)の間はKataGoClientの
                // ワーカースレッドがパイプI/Oのmutexを保持し続けるため、ここでResetBoard()を
                // 呼ぶと解放されるまで描画ループが止まってしまう。requestReviewAnalysisForと
                // 同じ理由で、解析要求中は新規対局の開始を1フレーム見送る
                if (newGamePressed && reviewAnalysisPendingIndex == -1)
                {
                    startNewGame();
                    break;
                }
                const int totalMoves = static_cast<int>(reviewRecord.Moves.size());
                bool indexChanged = false;
                if (reviewNextPressed && reviewMoveIndex < totalMoves)
                {
                    ++reviewMoveIndex;
                    indexChanged = true;
                }
                else if (reviewPrevPressed && reviewMoveIndex > 0)
                {
                    --reviewMoveIndex;
                    indexChanged = true;
                }
                if (indexChanged)
                {
                    reviewBoard = ReplayMoves(reviewRecord.Moves, reviewMoveIndex, reviewRecord.BoardSize);
                }
                triggerReviewAnalysisIfNeeded();
                break;
            }
            }

            const GoBoard& displayBoard = (turnState == TurnState::Reviewing) ? reviewBoard : board;

            // 表示する勝率・目差の決定(通常の対局中は最新解析、棋譜再生中はその手数のキャッシュ)
            bool haveWinrateToShow = false;
            float displayBlackWinrate = 0.5f;
            float displayBlackScoreLead = 0.0f;
            if (turnState == TurnState::Reviewing)
            {
                if (reviewMoveIndex < static_cast<int>(reviewHasCached.size()) &&
                    reviewHasCached[static_cast<size_t>(reviewMoveIndex)])
                {
                    haveWinrateToShow = true;
                    displayBlackWinrate = reviewWinrateCache[static_cast<size_t>(reviewMoveIndex)];
                    displayBlackScoreLead = reviewScoreLeadCache[static_cast<size_t>(reviewMoveIndex)];
                }
            }
            else if (hasAnalysis)
            {
                haveWinrateToShow = true;
                displayBlackWinrate = ToBlackWinrate(latestAnalysis);
                displayBlackScoreLead = ToBlackScoreLead(latestAnalysis);
            }

            renderer.BeginFrame(kClearColorR, kClearColorG, kClearColorB);
            if (turnState == TurnState::ViewingMistakeStats)
            {
                // ボタン列の真上には表示しないよう、盤を内包する左側の利用可能幅を基準にする
                DrawMistakeStatsScreen(renderer, static_cast<uint32_t>(layout.ContentWidth), height, mistakeStats);
            }
            else
            {
                DrawBoard(renderer, whiteTexture, layout, activeBoardSize);
                if (hasAnalysis && territoryOverlayEnabled)
                {
                    DrawTerritoryOverlay(renderer, displayBoard, layout, whiteTexture, latestAnalysis);
                }
                DrawStones(renderer, displayBoard, layout);
                if (hasAnalysis && hintOverlayEnabled && turnState == TurnState::HumanToMove)
                {
                    DrawMoveHints(renderer, layout, whiteTexture, latestAnalysis);
                }
                if (haveWinrateToShow)
                {
                    DrawWinrateBar(renderer, whiteTexture, layout, displayBlackWinrate);
                    DrawWinrateText(renderer, layout, displayBlackWinrate, displayBlackScoreLead);
                }
                else if (turnState == TurnState::Reviewing)
                {
                    DrawWinratePending(renderer, layout);
                }
                if (turnState == TurnState::Reviewing)
                {
                    // 上部帯全体(盤を内包する幅、ボタン列の真上は含めない)を覆う背景パネルを、
                    // 言語化コメント・損失グラフより先に描画してボタン列と統一感のある配色にする
                    renderer.DrawRoundedRect(
                        layout.ContentWidth * 0.5f, static_cast<float>(height) - kGraphAreaHeight * 0.5f,
                        layout.ContentWidth, kGraphAreaHeight, kTopPanelCornerRadius,
                        kTopPanelColorR, kTopPanelColorG, kTopPanelColorB, kTopPanelAlpha,
                        kTopPanelBorderThickness, kTopPanelBorderColorR, kTopPanelBorderColorG, kTopPanelBorderColorB, 1.0f);

                    const std::wstring commentary = BuildMoveCommentary(reviewRecord, reviewMoveIndex,
                        reviewWinrateCache, reviewBestMoveRowCache, reviewBestMoveColCache, reviewHasCached);
                    DrawMoveCommentary(renderer, height, commentary);
                    DrawLossGraph(renderer, static_cast<uint32_t>(layout.ContentWidth), height,
                        reviewWinrateCache, reviewHasCached, reviewMoveIndex);
                }
                DrawHud(renderer, layout, turnState, displayBoard,
                    reviewMoveIndex, static_cast<int>(reviewRecord.Moves.size()), reviewRecord.Result,
                    CurrentUserRating().Rating, currentGameMode, currentAiTargetRating,
                    placementTracker.Active, placementTracker.CurrentEstimate(), placementTracker.ConvergenceRate(),
                    postGameAnalysisActive, postGameAnalysisIndex,
                    static_cast<int>(postGameAnalysisMoves.size()));
            }

            // 中央グループと「終了」ボタン(下グループ)の間に区切り線を描き、誤クリックしやすい
            // 「終了」の手前を視覚的に区切る
            {
                const float separatorY = kButtonColumnMarginY + kButtonHeight + kButtonSpacing * 0.5f;
                const float separatorLeftX = layout.ContentWidth + kButtonSeparatorMarginX;
                const float separatorRightX = layout.ContentWidth + kButtonColumnWidth - kButtonSeparatorMarginX;
                renderer.DrawLine(separatorLeftX, separatorY, separatorRightX, separatorY, kButtonSeparatorThickness,
                    kButtonSeparatorColorR, kButtonSeparatorColorG, kButtonSeparatorColorB, 1.0f);
            }

            // 着手以外の操作ボタンを右側のボタン列に描画する
            const bool isMouseDown = renderer.IsMouseButtonDown(MouseButton::Left);
            for (size_t i = 0; i < buttonRects.size(); ++i)
            {
                const bool isButtonHovered = mouseInWindow && IsPointInButton(buttonRects[i], mouseWorldX, mouseWorldY);
                const bool isButtonPressed = isButtonHovered && isMouseDown;
                DrawButton(renderer, buttonRects[i], buttonSpecs[i].Label, isButtonHovered, isButtonPressed);
            }

            // カジュアル対局の強さ選択(11.6節)。盤中央のタブ+グリッドを描画する
            for (size_t i = 0; i < casualTabRects.size(); ++i)
            {
                const bool isButtonHovered = mouseInWindow && IsPointInButton(casualTabRects[i], mouseWorldX, mouseWorldY);
                const bool isButtonPressed = isButtonHovered && isMouseDown;
                DrawButton(renderer, casualTabRects[i], casualTabSpecs[i].Label, isButtonHovered, isButtonPressed,
                    casualTabStyle);
            }
            for (size_t i = 0; i < casualSlotRects.size(); ++i)
            {
                const bool isButtonHovered = mouseInWindow && IsPointInButton(casualSlotRects[i], mouseWorldX, mouseWorldY);
                const bool isButtonPressed = isButtonHovered && isMouseDown;
                DrawButton(renderer, casualSlotRects[i], casualSlotSpecs[i].Label, isButtonHovered, isButtonPressed,
                    casualGridStyle);
            }

            // 手番中、カーソルが空点の交点上にあれば半透明のプレビューを表示する
            if (turnState == TurnState::HumanToMove && isHovering)
            {
                const float x = GridIndexToCoordinate(layout, layout.CenterX, hoverCol);
                const float y = GridIndexToCoordinate(layout, layout.CenterY, hoverRow);
                const float previewRadius = layout.LineSpacing * 0.46f;
                renderer.DrawCircle(x, y, previewRadius, 0.2f, 0.2f, 0.2f, 0.35f);
            }

            renderer.EndFrame(true);
        }
    }
    catch (const std::exception& e)
    {
        std::ofstream log("error.log", std::ios::app);
        log << e.what() << std::endl;
        ShowMessageBoxUtf8(nullptr, e.what(), "KurenaiGo - エラー", MB_OK | MB_ICONERROR);
        exitCode = 1;
    }

    if (SUCCEEDED(comResult))
    {
        CoUninitialize();
    }

    return exitCode;
}
