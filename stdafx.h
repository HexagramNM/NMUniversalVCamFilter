
//参考ページ

//32bitと64bitの問題
//https://docs.microsoft.com/ja-jp/cpp/build/common-visual-cpp-64-bit-migration-issues?view=vs-2019

//skype等で認識されるには、Pin側のクラスにIKsPropertySetとIAMStreamConfig、IAMFilterMiscFlagsのインタフェースが必要
//これらを追加する際にIUnknownインタフェースのメソッドを実装する必要もあり。
//https://community.osr.com/discussion/245023/virtual-camera-source-filter-directshow
//https://docs.microsoft.com/ja-jp/office/client-developer/outlook/mapi/implementing-iunknown-in-c-plus-plus

//仮想カメラに必要なインタフェースの実装例
//https://github.com/aricatamoy/svcam

//DirectShowFilterカテゴリに配置する登録処理
//https://docs.microsoft.com/en-us/windows/win32/directshow/implementing-dllregisterserver

//WinRTによるデスクトップキャプチャ
//https://qiita.com/eguo/items/90604787a6098af404d9
//https://github.com/opysky/examples/tree/master/winrt/GraphicsCapture/CapturePreview

//D3D11のテクスチャから画素情報を読みだす方法
//（CPUで読み出し可能なバッファテクスチャを作り、そこからID3D11DeviceContextのMapを使う必要がある。）
//http://www-fps.nifs.ac.jp/ito/memo/d3dx11_02.html

//CopySubresourceRegion
//テクスチャのサイズが違うとCopyResourceが使用できないため、代わりにCopySubresourceRegionを使用する
//https://docs.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-copysubresourceregion

//DirectInput...キーボード入力用のCOM
//http://www.charatsoft.com/develop/otogema/page/07input/dinput.html

#pragma once

#define WIN32_LEAN_AND_MEAN
#define _WIN32_DCOM 

#define _CRT_SECURE_NO_DEPRECATE
#pragma warning(disable : 4995)
#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "winmm.lib")

#ifdef _DEBUG
#pragma comment(lib, "strmbasd.lib")
#else
#pragma comment(lib, "strmbase.lib")
#endif

#include <streams.h>
#include <initguid.h>

#include <chrono>
#include <thread>

#define TEMPLATE_NAME	(L"NM Universal Virtual Cam")
#define FILTER_NAME		(TEMPLATE_NAME)
#define OUTPUT_PIN_NAME (L"Output")

//DirectInput
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#pragma comment(lib, "dinput8.lib")
#pragma comment(lib, "dxguid.lib")

//WindowRT GraphicsCapture

//windows.storage.0.hのエラーは
//directshow\baseclasses\wxdebug.hのマクロ（DisplayType）が
//関数名を書き換えてしまったため。
#undef DisplayType

#include <shobjidl_core.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3d11.h>

#include <d3d11_4.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <windows.h>
#pragma comment(lib, "windowsapp")

#define MAX_CAP_WIDTH 3840
#define MAX_CAP_HEIGHT 2160
#define WINDOW_WIDTH 960
#define WINDOW_HEIGHT 720
#define PIXEL_BIT 24
//#define REFINED_PROCESS

#include "NMVCamFilter.h"




