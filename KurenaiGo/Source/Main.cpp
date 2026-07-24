// KurenaiGo - 囲碁アプリ。
// KurenaiEngine2D(公開API)で19路盤・石を描画し、KataGo(https://github.com/lightvector/katago)を
// GTP(Go Text Protocol)で動かして人間(黒)とKataGo(白)の対局を行う。
// 座標系はワールド=ピクセル座標(原点は画面左下、Y-up)。
//
// 操作: 交点クリックで着手 / Pキーでパス / Rキーで投了 / Tキーで地合い表示切替 /
//       Hキーで着手ヒント表示切替 / Escで終了

#include <Windows.h>

#include <objbase.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "GoBoard.h"
#include "KataGoClient.h"
#include "KurenaiEngine2D.h"
#include "KurenaiTypes.h"
#include "PathUtil.h"

using namespace Kurenai;
using namespace KurenaiGo;

namespace
{
    // 盤の目の数(19路盤)
    constexpr int kBoardLines = 19;

    // 19路盤の星(hoshi)の位置。0-indexed(0〜18)で、標準的な4線交点
    constexpr std::array<int, 3> kHoshiIndices = { 3, 9, 15 };

    constexpr uint32_t kWindowWidth = 900;
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

    // 盤下の余白に表示するHUDテキストの見た目。DrawTextはASCII印字可能文字のみ対応のため
    // (かな漢字は表示されない)、表示文言は英語表記にする
    constexpr float kHudFontSize = 18.0f;
    constexpr float kHudColorR = 0.92f, kHudColorG = 0.92f, kHudColorB = 0.90f;

    const wchar_t* kWindowTitle = L"KurenaiGo";

    // 現在のウィンドウサイズから盤のレイアウト(中心・格子の一辺・目の間隔)を求める
    struct BoardLayout
    {
        float CenterX = 0.0f;
        float CenterY = 0.0f;
        float BoardExtent = 0.0f;
        float GridExtent = 0.0f;
        float LineSpacing = 0.0f;
    };

    BoardLayout ComputeBoardLayout(uint32_t windowWidth, uint32_t windowHeight)
    {
        const float minDimension = static_cast<float>((std::min)(windowWidth, windowHeight));

        BoardLayout layout;
        layout.CenterX = static_cast<float>(windowWidth) * 0.5f;
        layout.CenterY = static_cast<float>(windowHeight) * 0.5f;
        layout.BoardExtent = minDimension * kBoardExtentRatio;
        layout.GridExtent = layout.BoardExtent * kGridExtentRatio;
        layout.LineSpacing = layout.GridExtent / static_cast<float>(kBoardLines - 1);
        return layout;
    }

    // 格子線上のインデックス(0〜kBoardLines-1)からワールド座標へ変換する
    float GridIndexToCoordinate(const BoardLayout& layout, float center, int index)
    {
        const float origin = center - layout.GridExtent * 0.5f;
        return origin + layout.LineSpacing * static_cast<float>(index);
    }

    // ワールド座標(worldX, worldY)に最も近い交点を求め、スナップ範囲内であれば
    // outRow/outColに書き込みtrueを返す。盤外・範囲外ならfalse
    bool TryGetHoveredIntersection(const BoardLayout& layout, float worldX, float worldY, int& outRow, int& outCol)
    {
        const auto nearestIndex = [](const BoardLayout& layoutRef, float center, float coord) -> int
        {
            const float origin = center - layoutRef.GridExtent * 0.5f;
            return static_cast<int>(std::lround((coord - origin) / layoutRef.LineSpacing));
        };

        const int col = nearestIndex(layout, layout.CenterX, worldX);
        const int row = nearestIndex(layout, layout.CenterY, worldY);
        if (col < 0 || col >= kBoardLines || row < 0 || row >= kBoardLines)
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

    void DrawBoard(KurenaiEngine2D& renderer, TextureHandle whiteTexture, const BoardLayout& layout)
    {
        // 木目の盤面
        renderer.DrawSprite(
            layout.CenterX, layout.CenterY, layout.BoardExtent, layout.BoardExtent, 0.0f,
            whiteTexture, kBoardColorR, kBoardColorG, kBoardColorB, 1.0f);

        const float lineThickness = (std::max)(1.5f, layout.LineSpacing * 0.035f);
        const float halfGrid = layout.GridExtent * 0.5f;

        // 横線(各行ごとに1本、盤の幅いっぱいに伸ばす)
        for (int row = 0; row < kBoardLines; ++row)
        {
            const float y = GridIndexToCoordinate(layout, layout.CenterY, row);
            renderer.DrawLine(
                layout.CenterX - halfGrid, y, layout.CenterX + halfGrid, y, lineThickness,
                kLineColorR, kLineColorG, kLineColorB, 1.0f);
        }

        // 縦線(各列ごとに1本、盤の高さいっぱいに伸ばす)
        for (int col = 0; col < kBoardLines; ++col)
        {
            const float x = GridIndexToCoordinate(layout, layout.CenterX, col);
            renderer.DrawLine(
                x, layout.CenterY - halfGrid, x, layout.CenterY + halfGrid, lineThickness,
                kLineColorR, kLineColorG, kLineColorB, 1.0f);
        }

        // 星(hoshi)。3x3の標準的な交点に小さな点を描く
        const float hoshiRadius = layout.LineSpacing * 0.11f;
        for (int hoshiRow : kHoshiIndices)
        {
            const float y = GridIndexToCoordinate(layout, layout.CenterY, hoshiRow);
            for (int hoshiCol : kHoshiIndices)
            {
                const float x = GridIndexToCoordinate(layout, layout.CenterX, hoshiCol);
                renderer.DrawCircle(x, y, hoshiRadius, kLineColorR, kLineColorG, kLineColorB, 1.0f);
            }
        }
    }

    void DrawStones(KurenaiEngine2D& renderer, const GoBoard& board, const BoardLayout& layout)
    {
        const float stoneRadius = layout.LineSpacing * 0.46f;
        for (int row = 0; row < kBoardLines; ++row)
        {
            for (int col = 0; col < kBoardLines; ++col)
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

    // 石のない交点に、地の所有率(黒視点)に応じた色つきオーバーレイを描く(Tキーでトグル)
    void DrawTerritoryOverlay(KurenaiEngine2D& renderer, const GoBoard& board, const BoardLayout& layout,
        TextureHandle whiteTexture, const KataGoAnalysisResult& analysis)
    {
        if (analysis.Ownership.size() != static_cast<size_t>(kBoardLines) * static_cast<size_t>(kBoardLines))
        {
            return;
        }

        const float overlaySize = layout.LineSpacing * 0.82f;
        for (int row = 0; row < kBoardLines; ++row)
        {
            for (int col = 0; col < kBoardLines; ++col)
            {
                if (board.At(row, col) != Stone::Empty)
                {
                    continue;
                }

                const float blackOwnership = ToBlackOwnership(analysis, OwnershipIndex(row, col, kBoardLines));
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
        EngineStarting, // KataGo起動中(OpenCL初回チューニング等で数十秒かかることがある)
        HumanToMove,     // 黒(人間)の手番
        AIThinking,      // 白(KataGo)がgenmove応答待ち
        WaitingForScore, // 両者パス後、final_score応答待ち
        GameOver,
    };

    // 盤下のHUDに表示する手番状態・アゲハマ数のテキストを組み立てる
    std::wstring BuildStatusText(TurnState turnState, const GoBoard& board)
    {
        std::wstring text;
        switch (turnState)
        {
        case TurnState::EngineStarting:  text = L"Starting KataGo..."; break;
        case TurnState::HumanToMove:     text = L"Your move (Black)"; break;
        case TurnState::AIThinking:      text = L"KataGo is thinking..."; break;
        case TurnState::WaitingForScore: text = L"Computing final score..."; break;
        case TurnState::GameOver:        text = L"Game over"; break;
        }
        text += L"   Captures B:" + std::to_wstring(board.CapturesBy(Stone::Black)) +
            L" W:" + std::to_wstring(board.CapturesBy(Stone::White));
        return text;
    }

    // 盤の下マージンにHUDテキストを描画する
    void DrawHud(KurenaiEngine2D& renderer, const BoardLayout& layout, TurnState turnState, const GoBoard& board)
    {
        const float hudX = layout.CenterX - layout.GridExtent * 0.5f;
        const float bottomMarginCenterY = (layout.CenterY - layout.BoardExtent * 0.5f) * 0.5f;
        const float hudY = bottomMarginCenterY - kHudFontSize * 0.5f;
        renderer.DrawText(hudX, hudY, BuildStatusText(turnState, board), kHudFontSize,
            kHudColorR, kHudColorG, kHudColorB, 1.0f);
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
        const std::filesystem::path soundsDir = ResolveAppDataPath(L"Assets/Sounds");

        KurenaiEngine2D renderer(kWindowTitle, kWindowWidth, kWindowHeight, GraphicsAPI::DX11);

        const TextureHandle whiteTexture = renderer.CreateSolidColorTexture(255, 255, 255, 255);
        const SoundHandle stonePlaceSound = renderer.LoadSound((soundsDir / L"stone_place.wav").wstring());
        const SoundHandle gameEndSound = renderer.LoadSound((soundsDir / L"game_end.wav").wstring());

        GoBoard board(kBoardLines);
        KataGoClient kataGo;
        TurnState turnState = TurnState::EngineStarting;

        // 解析(kata-analyze)の最新結果。勝率表示・地合い可視化・着手ヒントが共通で使う
        KataGoAnalysisResult latestAnalysis;
        bool hasAnalysis = false;

        // HumanToMoveへ遷移すると同時に、その局面の解析(黒=人間視点)を要求する
        const auto enterHumanToMove = [&]()
        {
            turnState = TurnState::HumanToMove;
            hasAnalysis = false;
            kataGo.RequestAnalysis(Stone::Black);
        };

        kataGo.StartAsync(
            kataGoDir / L"katago.exe",
            kataGoDir / L"model.bin.gz",
            kataGoDir / L"gtp.cfg",
            kataGoDir / L"katago_stderr.log",
            kBoardLines);

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

            const BoardLayout layout = ComputeBoardLayout(width, height);

            // マウス位置をワールド座標(原点は画面左下、Y-up)へ変換する。GetClientMousePosition()は
            // Win32標準のクライアント座標(原点は左上、Y-down)を返すため、Yを反転する
            int hoverRow = -1;
            int hoverCol = -1;
            bool isHovering = false;
            if (renderer.IsMouseOverWindow())
            {
                const POINT cursor = renderer.GetClientMousePosition();
                const float worldX = static_cast<float>(cursor.x);
                const float worldY = static_cast<float>(height) - static_cast<float>(cursor.y);
                isHovering = TryGetHoveredIntersection(layout, worldX, worldY, hoverRow, hoverCol) &&
                    board.At(hoverRow, hoverCol) == Stone::Empty;
            }

            const bool clicked = renderer.WasMouseButtonPressed(MouseButton::Left);
            const bool passPressed = renderer.WasKeyPressed('P');
            const bool resignPressed = renderer.WasKeyPressed('R');

            if (renderer.WasKeyPressed('T'))
            {
                territoryOverlayEnabled = !territoryOverlayEnabled;
            }

            if (renderer.WasKeyPressed('H'))
            {
                hintOverlayEnabled = !hintOverlayEnabled;
            }

            // 解析結果のポーリング(HumanToMove遷移時にenterHumanToMove()から要求している)
            KataGoAnalysisResult polledAnalysis;
            if (kataGo.TryGetAnalysisResult(polledAnalysis))
            {
                latestAnalysis = polledAnalysis;
                hasAnalysis = !latestAnalysis.Failed;
            }

            switch (turnState)
            {
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
                    MessageBoxA(nullptr, "投了しました。KataGoの勝ちです。", "KurenaiGo", MB_OK | MB_ICONINFORMATION);
                    turnState = TurnState::GameOver;
                }
                else if (passPressed)
                {
                    board.Pass();
                    kataGo.PlayPass(Stone::Black);
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
                        MessageBoxA(nullptr, message.c_str(), "KurenaiGo - KataGoエラー", MB_OK | MB_ICONERROR);
                        turnState = TurnState::GameOver;
                    }
                    else if (result.IsResign)
                    {
                        renderer.PlaySound(gameEndSound);
                        MessageBoxA(nullptr, "KataGoが投了しました。あなたの勝ちです。", "KurenaiGo", MB_OK | MB_ICONINFORMATION);
                        turnState = TurnState::GameOver;
                    }
                    else if (result.IsPass)
                    {
                        board.Pass();
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
                        MessageBoxA(nullptr, "KataGoの着手を反映できませんでした(想定外の座標)。",
                            "KurenaiGo - エラー", MB_OK | MB_ICONERROR);
                        turnState = TurnState::GameOver;
                    }
                    else
                    {
                        renderer.PlaySound(stonePlaceSound);
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
                    const std::string message = "対局終了\n結果: " + score;
                    MessageBoxA(nullptr, message.c_str(), "KurenaiGo - 対局終了", MB_OK | MB_ICONINFORMATION);
                    turnState = TurnState::GameOver;
                }
                break;
            }

            case TurnState::GameOver:
                break;
            }

            renderer.BeginFrame(kClearColorR, kClearColorG, kClearColorB);
            DrawBoard(renderer, whiteTexture, layout);
            if (hasAnalysis && territoryOverlayEnabled)
            {
                DrawTerritoryOverlay(renderer, board, layout, whiteTexture, latestAnalysis);
            }
            DrawStones(renderer, board, layout);
            if (hasAnalysis && hintOverlayEnabled && turnState == TurnState::HumanToMove)
            {
                DrawMoveHints(renderer, layout, whiteTexture, latestAnalysis);
            }
            if (hasAnalysis)
            {
                DrawWinrateBar(renderer, whiteTexture, layout, ToBlackWinrate(latestAnalysis));
            }
            DrawHud(renderer, layout, turnState, board);

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
        MessageBoxA(nullptr, e.what(), "KurenaiGo - エラー", MB_OK | MB_ICONERROR);
        exitCode = 1;
    }

    if (SUCCEEDED(comResult))
    {
        CoUninitialize();
    }

    return exitCode;
}
