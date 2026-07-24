// KurenaiGo - 囲碁アプリ。
// KurenaiEngine2D(公開API)のみを使い、19路盤を描画する。
// 座標系はワールド=ピクセル座標(原点は画面左下、Y-up)。Escキーで終了する。

#include <Windows.h>

#include <objbase.h>

#include <array>
#include <exception>
#include <fstream>
#include <string>

#include "KurenaiEngine2D.h"
#include "KurenaiTypes.h"

using namespace Kurenai;

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

    void DrawBoard(KurenaiEngine2D& renderer, TextureHandle whiteTexture, const BoardLayout& layout)
    {
        // 木目の盤面
        renderer.DrawSprite(
            layout.CenterX, layout.CenterY, layout.BoardExtent, layout.BoardExtent, 0.0f,
            whiteTexture, kBoardColorR, kBoardColorG, kBoardColorB, 1.0f);

        const float lineThickness = (std::max)(1.5f, layout.LineSpacing * 0.035f);

        // 横線(各行ごとに1本、盤の幅いっぱいに伸ばす)
        for (int row = 0; row < kBoardLines; ++row)
        {
            const float y = GridIndexToCoordinate(layout, layout.CenterY, row);
            renderer.DrawSprite(
                layout.CenterX, y, layout.GridExtent, lineThickness, 0.0f,
                whiteTexture, kLineColorR, kLineColorG, kLineColorB, 1.0f);
        }

        // 縦線(各列ごとに1本、盤の高さいっぱいに伸ばす)
        for (int col = 0; col < kBoardLines; ++col)
        {
            const float x = GridIndexToCoordinate(layout, layout.CenterX, col);
            renderer.DrawSprite(
                x, layout.CenterY, lineThickness, layout.GridExtent, 0.0f,
                whiteTexture, kLineColorR, kLineColorG, kLineColorB, 1.0f);
        }

        // 星(hoshi)。3x3の標準的な交点に小さな点を描く
        const float hoshiSize = layout.LineSpacing * 0.22f;
        for (int hoshiRow : kHoshiIndices)
        {
            const float y = GridIndexToCoordinate(layout, layout.CenterY, hoshiRow);
            for (int hoshiCol : kHoshiIndices)
            {
                const float x = GridIndexToCoordinate(layout, layout.CenterX, hoshiCol);
                renderer.DrawSprite(
                    x, y, hoshiSize, hoshiSize, 0.0f,
                    whiteTexture, kLineColorR, kLineColorG, kLineColorB, 1.0f);
            }
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
        KurenaiEngine2D renderer(L"KurenaiGo", kWindowWidth, kWindowHeight, GraphicsAPI::DX11);

        const TextureHandle whiteTexture = renderer.CreateSolidColorTexture(255, 255, 255, 255);

        while (!renderer.ShouldClose())
        {
            renderer.PumpEvents();

            if (GetAsyncKeyState(VK_ESCAPE) & 0x8000)
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

            renderer.BeginFrame(kClearColorR, kClearColorG, kClearColorB);
            DrawBoard(renderer, whiteTexture, layout);
            renderer.EndFrame(true);
        }
    }
    catch (const std::exception& e)
    {
        std::ofstream log("error.log", std::ios::app);
        log << e.what() << std::endl;
        MessageBoxA(nullptr, e.what(), "KurenaiGo - 初期化エラー", MB_OK | MB_ICONERROR);
        exitCode = 1;
    }

    if (SUCCEEDED(comResult))
    {
        CoUninitialize();
    }

    return exitCode;
}
